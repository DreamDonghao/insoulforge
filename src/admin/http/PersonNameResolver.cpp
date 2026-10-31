/// @file PersonNameResolver.cpp
/// @brief 查询最多四路并发，失败冷却一分钟，停止后丢弃迟到结果。

#include <vector>

#include <drogon/HttpAppFramework.h>

#include <admin/http/PersonNameResolver.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/session/QQNameDirectory.hpp>

namespace insoulforge {
    namespace {
        constexpr size_t kMaxConcurrentQueries = 4;
        constexpr f64 kQueryTimeoutSeconds = 5.0;
        constexpr auto kFailureCooldown = std::chrono::seconds(60);

        auto isKnownName(const std::string &name) -> bool { return !name.empty() && name != "未知"; }
    } // namespace

    auto PersonNameResolver::instance() -> PersonNameResolver & {
        static PersonNameResolver resolver;
        return resolver;
    }

    auto PersonNameResolver::lookup(const u64 userId, const Clock::time_point now) -> Name {
        std::lock_guard lock(m_mutex);
        auto name = QQNameDirectory::getName(userId);
        if (m_stopping || userId == 0 || isKnownName(name))
            return {std::move(name), false};
        auto &state = m_states[userId];
        if (!state.pending && now >= state.retryAfter) {
            state.pending = true;
            m_queue.push_back(userId);
            // 不在 HTTP 响应路径上启动网络请求，锁释放后由事件循环推进。
            drogon::app().getLoop()->queueInLoop([this] { pump(); });
        }
        return {std::move(name), state.pending};
    }

    void PersonNameResolver::pump() {
        std::vector<u64> ready;
        {
            std::lock_guard lock(m_mutex);
            while (!m_stopping && m_active < kMaxConcurrentQueries && !m_queue.empty()) {
                ready.push_back(m_queue.front());
                m_queue.pop_front();
                ++m_active;
            }
        }
        for (const auto userId: ready)
            drogon::async_run([this, userId] { return query(userId); });
    }

    auto PersonNameResolver::query(const u64 userId) -> drogon::Task<> {
        std::string name;
        try {
            bool shouldQuery;
            {
                std::lock_guard lock(m_mutex);
                name = QQNameDirectory::getName(userId);
                shouldQuery = !m_stopping && !isKnownName(name);
            }
            if (shouldQuery) {
                const auto response = co_await OneBotClient::getStrangerInfo(userId, std::nullopt, kQueryTimeoutSeconds);
                name = getStr(atOrNull(response, "data"), "nickname");
            }
        } catch (const std::exception &error) {
            Logger::warn(0, "Admin", fmt::format("查询人物昵称失败: userId={}, {}", userId, error.what()));
        }
        {
            std::lock_guard lock(m_mutex);
            --m_active;
            if (!m_stopping) {
                if (isKnownName(name)) {
                    QQNameDirectory::recordName(userId, std::move(name));
                    m_states.erase(userId);
                } else {
                    m_states[userId] = {false, Clock::now() + kFailureCooldown};
                }
            }
        }
        pump();
    }

    void PersonNameResolver::stop() {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
        m_queue.clear();
        m_states.clear();
    }
} // namespace insoulforge
