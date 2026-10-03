/// @file AsyncTaskManager.cpp
/// @brief 会话级后台任务执行与结果交付

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <fmt/format.h>

#include <agent/ability/AsyncTaskManager.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/messaging/MessageService.hpp>

namespace insoulforge {
    namespace {
        /// @brief 时间戳加进程内序号，避免重启后系统状态消息 ID 与旧记录重复
        auto nextTaskId() -> std::string {
            static std::atomic<u64> sequence{0};
            const auto now = std::chrono::system_clock::now().time_since_epoch();
            return fmt::format("{}-{}", std::chrono::duration_cast<std::chrono::microseconds>(now).count(),
              sequence.fetch_add(1, std::memory_order_relaxed));
        }

        /// @brief 只记录系统状态；走入站队列会令 Router 对系统消息再次触发回复
        void recordStatus(
          const u64 sessionId, const std::string &taskId, const std::string_view phase, const std::string &text) {
            json message;
            message["time"] = currentDateTime();
            message["sender"] = {{"name", "系统异步任务"}, {"qq", std::to_string(SessionId::kSystemAccountId)}};
            message["message_id"] = fmt::format("async-task:{}:{}", taskId, phase);
            message["segments"] = {{{"type", "text"}, {"text", text}}};
            OneBotEventWorkflow::instance().appendSystemStatusMessage(sessionId, std::move(message));
        }
    } // namespace

    auto AsyncTaskManager::instance() -> AsyncTaskManager & {
        static AsyncTaskManager manager;
        return manager;
    }

    auto AsyncTaskManager::start(const u64 sessionId, std::string description, Handler handler) -> StartResult {
        if (sessionId == 0 || !handler) {
            throw std::invalid_argument("后台任务缺少会话或处理器");
        }
        const std::string taskId = nextTaskId();
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return {StartResult::Status::Stopping, {}};
            }
            if (const auto active = m_active.find(sessionId); active != m_active.end()) {
                return {StartResult::Status::Busy, active->second};
            }
            m_active.emplace(sessionId, taskId);
            try {
                // 先记录启动，再允许 stop() 标记中断，保证同一任务的状态顺序。
                recordStatus(sessionId, taskId, "started", fmt::format("异步任务 #{} 已开始：{}", taskId, description));
            } catch (...) {
                m_active.erase(sessionId);
                throw;
            }
        }

        try {
            // 普通 lambda 立即构造协程，参数由 run 的协程帧持有。
            drogon::async_run([this, sessionId, taskId, handler = std::move(handler)]() mutable -> drogon::Task<> {
                return run(sessionId, taskId, std::move(handler));
            });
        } catch (...) {
            release(sessionId, taskId);
            try {
                recordStatus(sessionId, taskId, "failed", fmt::format("异步任务 #{} 启动失败。", taskId));
            } catch (...) {
                Logger::error(sessionId, "AsyncTask", fmt::format("任务 #{} 启动失败状态记录失败", taskId));
            }
            throw;
        }
        Logger::info(sessionId, "AsyncTask", fmt::format("任务 #{} 已启动", taskId));
        return {StartResult::Status::Started, taskId};
    }

    auto AsyncTaskManager::run(const u64 sessionId, std::string taskId, Handler handler) -> drogon::Task<> {
        std::string failureReason;
        try {
            const std::string content = co_await handler();
            bool maySend;
            {
                std::lock_guard lock(m_mutex);
                maySend = !m_stopping && m_active.contains(sessionId) && m_active.at(sessionId) == taskId;
            }
            if (maySend) {
                if (content.empty()) {
                    throw std::runtime_error("任务结果为空");
                }
                std::optional<u64> sent;
                if (SessionId::isPrivate(sessionId)) {
                    sent = co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), content);
                } else {
                    sent = co_await MessageService::sendGroupMsg(sessionId, content);
                }
                if (!sent) {
                    throw std::runtime_error("结果发送失败");
                }
                Logger::info(sessionId, "AsyncTask", fmt::format("任务 #{} 已完成并发送", taskId));
            }
        } catch (const std::exception &error) {
            failureReason = error.what();
        } catch (...) {
            failureReason = "未知异常";
        }
        if (!failureReason.empty()) {
            Logger::error(sessionId, "AsyncTask", fmt::format("任务 #{} 失败: {}", taskId, failureReason));
            try {
                bool maySend;
                {
                    std::lock_guard lock(m_mutex);
                    maySend = !m_stopping && m_active.contains(sessionId) && m_active.at(sessionId) == taskId;
                    if (maySend) {
                        recordStatus(sessionId, taskId, "failed", fmt::format("异步任务 #{} 执行失败。", taskId));
                    }
                }
                if (maySend) {
                    const std::string failure = fmt::format("异步任务 #{} 执行失败，请稍后重试。", taskId);
                    if (SessionId::isPrivate(sessionId)) {
                        co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), failure);
                    } else {
                        co_await MessageService::sendGroupMsg(sessionId, failure);
                    }
                }
            } catch (const std::exception &sendError) {
                Logger::error(sessionId, "AsyncTask", fmt::format("失败通知发送异常: {}", sendError.what()));
            } catch (...) {
                Logger::error(sessionId, "AsyncTask", "失败通知发送发生未知异常");
            }
        }
        release(sessionId, taskId);
    }

    void AsyncTaskManager::release(const u64 sessionId, const std::string &taskId) {
        std::lock_guard lock(m_mutex);
        if (const auto it = m_active.find(sessionId); it != m_active.end() && it->second == taskId) {
            m_active.erase(it);
        }
    }

    void AsyncTaskManager::stop() {
        std::vector<std::pair<u64, std::string>> interrupted;
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            interrupted.assign(m_active.begin(), m_active.end());
            m_active.clear();
        }
        for (const auto &[sessionId, taskId]: interrupted) {
            recordStatus(sessionId, taskId, "interrupted", fmt::format("异步任务 #{} 因程序退出而中断。", taskId));
        }
    }
} // namespace insoulforge
