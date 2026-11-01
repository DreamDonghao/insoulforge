/// @file PersonNameResolver.hpp
/// @brief 管理后台人物昵称的非阻塞查询队列。
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

#include <drogon/utils/coroutine.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>

namespace insoulforge {
    class PersonNameResolver {
    public:
        using Clock = std::chrono::steady_clock;
        struct Name {
            std::string value;
            bool pending = false;
        };
        static auto instance() -> PersonNameResolver &;
        /// @brief 立即返回缓存值，缺失时排队；now 用于失败冷却判断。
        [[nodiscard]] auto lookup(const UserId &userId, Clock::time_point now = Clock::now()) -> Name;
        void stop();

    private:
        struct State {
            bool pending = false;
            Clock::time_point retryAfter{};
        };
        void pump();
        auto query(UserId userId) -> drogon::Task<>;
        std::mutex m_mutex;
        std::deque<UserId> m_queue;
        std::unordered_map<UserId, State> m_states;
        size_t m_active = 0;
        bool m_stopping = false;
    };
} // namespace insoulforge
