/// @file AsyncTaskManager.cpp
/// @brief 会话级后台任务执行与结果交付

#include <atomic>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <ctime>
#include <fmt/format.h>

#include <agent/ability/AsyncTaskManager.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/config/Config.hpp>
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

        /// @brief 记录启动或退出中断状态，不触发回复。
        void recordStatus(
          const u64 sessionId, const std::string &taskId, const std::string_view phase, const std::string &text) {
            json message;
            message["time"] = currentDateTime();
            message["sender"] = {{"name", "系统异步任务"}, {"qq", std::to_string(SessionId::kSystemAccountId)}};
            message["message_id"] = fmt::format("async-task:{}:{}", taskId, phase);
            message["segments"] = {{{"type", "text"}, {"text", text}}};
            OneBotEventWorkflow::instance().appendSystemStatusMessage(sessionId, std::move(message));
        }

        /// @brief 将任务结果作为新系统消息加入入站队列，按定时任务相同的流程触发回复。
        void enqueueResultStatus(const u64 sessionId, const std::string &text) {
            static std::atomic<i64> nextMessageId{
              std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch())
                .count()};
            json event;
            event["post_type"] = "message";
            event["self_id"] = Config::instance().selfQQNumber;
            event["time"] = std::time(nullptr);
            event["message_id"] = nextMessageId.fetch_add(1, std::memory_order_relaxed);
            event["raw_message"] = text;
            event["sender"] = {{"user_id", SessionId::kSystemAccountId}, {"nickname", "系统异步任务"}};
            if (SessionId::isPrivate(sessionId)) {
                event["message_type"] = "private";
                event["user_id"] = SessionId::privateUserId(sessionId);
            } else {
                event["message_type"] = "group";
                event["group_id"] = sessionId;
            }
            event["message"] = json::array({{{"type", "text"}, {"data", {{"text", text}}}}});
            OneBotEventWorkflow::instance().enqueueOneBotEvent(std::move(event));
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
                enqueueResultStatus(
                  sessionId, fmt::format("【系统异步任务】任务 #{} 启动失败。请根据上下文告知用户任务失败。", taskId));
            } catch (...) {
                Logger::error(sessionId, "AsyncTask", fmt::format("任务 #{} 启动失败消息入队失败", taskId));
            }
            throw;
        }
        Logger::info(sessionId, "AsyncTask", fmt::format("任务 #{} 已启动", taskId));
        return {StartResult::Status::Started, taskId};
    }

    auto AsyncTaskManager::run(const u64 sessionId, std::string taskId, Handler handler) -> drogon::Task<> {
        std::string failureReason;
        try {
            const Result result = co_await handler();
            bool maySend;
            {
                std::lock_guard lock(m_mutex);
                maySend = !m_stopping && m_active.contains(sessionId) && m_active.at(sessionId) == taskId;
            }
            if (maySend) {
                if (result.content.empty()) {
                    throw std::runtime_error("任务结果为空");
                }
                std::optional<u64> sent;
                if (SessionId::isPrivate(sessionId)) {
                    sent = co_await MessageService::sendPrivateMsg(
                      SessionId::privateUserId(sessionId), result.content, result.imageDescription);
                } else {
                    sent = co_await MessageService::sendGroupMsg(sessionId, result.content, result.imageDescription);
                }
                if (!sent) {
                    throw std::runtime_error("结果发送失败");
                }
                Logger::info(sessionId, "AsyncTask", fmt::format("任务 #{} 已完成并发送", taskId));
                try {
                    bool shouldNotify;
                    {
                        std::lock_guard lock(m_mutex);
                        shouldNotify = !m_stopping && m_active.contains(sessionId) && m_active.at(sessionId) == taskId;
                    }
                    if (shouldNotify) {
                        enqueueResultStatus(
                          sessionId, fmt::format("【系统异步任务】任务 #{} 已完成，结果已发送到当前会话。"
                                                 "请结合上一条结果简短回应，不要重复发送结果。",
                                       taskId));
                    }
                } catch (const std::exception &error) {
                    Logger::error(
                      sessionId, "AsyncTask", fmt::format("任务 #{} 完成消息入队失败: {}", taskId, error.what()));
                }
            }
        } catch (const std::exception &error) {
            failureReason = error.what();
        } catch (...) {
            failureReason = "未知异常";
        }
        if (!failureReason.empty()) {
            Logger::error(sessionId, "AsyncTask", fmt::format("任务 #{} 失败: {}", taskId, failureReason));
            try {
                bool shouldNotify;
                {
                    std::lock_guard lock(m_mutex);
                    shouldNotify = !m_stopping && m_active.contains(sessionId) && m_active.at(sessionId) == taskId;
                }
                if (shouldNotify) {
                    enqueueResultStatus(
                      sessionId, fmt::format("【系统异步任务】任务 #{} 执行或发送失败，结果未成功交付。"
                                             "请根据上下文告知用户任务失败，不要声称结果已发送。",
                                   taskId));
                }
            } catch (const std::exception &statusError) {
                Logger::error(sessionId, "AsyncTask", fmt::format("失败消息入队异常: {}", statusError.what()));
            } catch (...) {
                Logger::error(sessionId, "AsyncTask", "失败消息入队发生未知异常");
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
