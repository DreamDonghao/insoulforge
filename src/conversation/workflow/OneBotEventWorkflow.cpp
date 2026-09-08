/// @file OneBotEventWorkflow.cpp
/// @brief OneBot 入站事件处理工作流实现

#include <admin/realtime/WebSocketManager.hpp>
#include <agent/memory/MemoryManager.hpp>
#include <agent/runtime/AgentSystem.hpp>
#include <agent/runtime/ExecutorAgent.hpp>
#include <conversation/history/ChatRecordManager.hpp>
#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/maintenance/ConversationMaintenanceService.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/SessionConfigManager.hpp>
#include <conversation/session/SessionStore.hpp>
#include <conversation/workflow/CommandProcessor.hpp>
#include <conversation/workflow/MessageContentEnricher.hpp>
#include <conversation/workflow/MessageList.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <conversation/workflow/OneBotEventNormalizer.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <deque>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <onebot/MessageService.hpp>
#include <ranges>
#include <stdexcept>
#include <vector>

namespace insoulforge {
    namespace {
        /// @brief 判断消息是否可在已有回复任务时继续排队
        [[nodiscard]] bool shouldQueueWhileReplying(const json &message) {
            if (MessageRecord::isSystem(message)) {
                return true;
            }
            return MessageRecord::mentions(message, Config::instance().selfQQNumber);
        }

        /// @brief 记录已完成主处理消息的会话统计
        void recordMessageProcessingStats(const uint64_t sessionId) {
            try {
                SessionConfigManager::incrementMessageCount(sessionId);
            } catch (const std::exception &error) {
                Logger::session(sessionId).error("会话统计更新失败: {}", error.what());
            } catch (...) {
                Logger::session(sessionId).error("会话统计更新失败: 未知异常");
            }
        }

        /// @brief 将已插入消息推送至管理后台，推送失败不得影响消息工作流
        /// @param sessionId
        /// @param message 已写入消息列表的完整消息
        /// @param displayContent 后台展示内容；为空时使用完整消息 JSON
        void pushRecordedMessage(
          const uint64_t sessionId, const json &message, const std::string &displayContent = {}) {
            try {
                const std::string serializedMessage = displayContent.empty() ? dumpJson(message) : displayContent;
                const std::string role = MessageRecord::isAssistant(message) ? "assistant" : "user";
                WebSocketManager::instance().pushMessage(sessionId, role, serializedMessage);
            } catch (const std::exception &error) {
                Logger::session(sessionId).error("管理后台消息推送失败: {}", error.what());
            } catch (...) {
                Logger::session(sessionId).error("管理后台消息推送失败: 未知异常");
            }
        }

        /// @brief 原子持久化会话派生状态任务，再分别启动异步消费者
        void scheduleConversationMaintenance(const uint64_t sessionId, const std::shared_ptr<MessageList> &messageList,
          const std::optional<MemorySummaryBatch> &batch) {
            if (!batch) {
                return;
            }
            try {
                ConversationMaintenanceService::enqueue(sessionId, batch->messages, batch->contextMessages);
            } catch (const std::exception &error) {
                messageList->cancelSummaryBatch();
                Logger::session(sessionId).error("会话派生状态维护任务持久化失败: {}", error.what());
            } catch (...) {
                messageList->cancelSummaryBatch();
                Logger::session(sessionId).error("会话派生状态维护任务持久化失败: 未知异常");
            }
        }
    } // namespace

    OneBotEventWorkflow &OneBotEventWorkflow::instance() {
        static OneBotEventWorkflow workflow;
        return workflow;
    }

    OneBotEventWorkflow::SessionWorkflowState::SessionWorkflowState(const uint64_t sessionId) :
        messageList(std::make_shared<MessageList>(sessionId)) {}

    OneBotEventWorkflow::OneBotEventWorkflow() {
        if (!Database::instance().handle()) {
            throw std::runtime_error("OneBotEventWorkflow requires an initialized database");
        }
        for (const uint64_t sessionId: ChatRecordStore::getSessionIds()) {
            m_sessions.emplace(sessionId, std::make_shared<SessionWorkflowState>(sessionId));
        }
        ConversationMaintenanceService::setMemorySummaryCompletedCallback([this](const uint64_t sessionId) {
            const auto sessionState = getOrCreateSessionState(sessionId);
            scheduleConversationMaintenance(
              sessionId, sessionState->messageList, sessionState->messageList->removeCompletedSummaryMessages());
        });
        for (const auto &[sessionId, sessionState]: m_sessions) {
            scheduleConversationMaintenance(
              sessionId, sessionState->messageList, sessionState->messageList->removeCompletedSummaryMessages());
        }
        ConversationMaintenanceService::resumePending();
    }

    void OneBotEventWorkflow::flushMessageListsToStorage() {
        std::vector<std::shared_ptr<MessageList>> messageLists;
        {
            std::lock_guard lock(m_sessionsMutex);
            for (const auto &sessionState: m_sessions | std::views::values) {
                messageLists.push_back(sessionState->messageList);
            }
        }
        for (const auto &messageList: messageLists) {
            messageList->flushToStorage();
        }
    }

    void OneBotEventWorkflow::appendDeliveredAssistantMessage(
      const uint64_t sessionId, json message, const std::string &displayContent) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto [messageSnapshot, summaryBatch, wasInserted] = sessionState->messageList->append(std::move(message));
        if (wasInserted) {
            pushRecordedMessage(sessionId, messageSnapshot.back(), displayContent);
        }
        scheduleConversationMaintenance(sessionId, sessionState->messageList, summaryBatch);
    }

    std::shared_ptr<OneBotEventWorkflow::SessionWorkflowState> OneBotEventWorkflow::getOrCreateSessionState(
      const uint64_t sessionId) {
        std::lock_guard lock(m_sessionsMutex);
        // 以有会话工作流状态则直接返回
        if (const auto found = m_sessions.find(sessionId); found != m_sessions.end()) {
            return found->second;
        }
        // 没有则创建新的会话工作流状态并返回
        auto state = std::make_shared<SessionWorkflowState>(sessionId);
        m_sessions.emplace(sessionId, state);
        return state;
    }

    drogon::Task<> OneBotEventWorkflow::executeCommand(const json &message) {
        const uint64_t sessionId = getUInt(message, "session_id");
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto [messageSnapshot, summaryBatch, wasInserted] = sessionState->messageList->append(message);
        if (wasInserted) {
            pushRecordedMessage(sessionId, messageSnapshot.back());
        }

        try {
            const std::string response = co_await CommandProcessor::execute(message);
            if (SessionId::isPrivate(sessionId)) {
                co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), response);
            } else {
                co_await MessageService::sendGroupMsg(sessionId, response);
            }
        } catch (const std::exception &error) {
            Logger::session(sessionId).error("命令执行或回复失败: {}", error.what());
        } catch (...) {
            Logger::session(sessionId).error("命令执行或回复失败: 未知错误");
        }
        scheduleConversationMaintenance(sessionId, sessionState->messageList, summaryBatch);
        co_return;
    }

    void OneBotEventWorkflow::enqueueOneBotEvent(json body) {
        // 格式化 OneBot 上报原始内容
        std::optional<json> normalizedMessage = OneBotEventNormalizer::normalize(std::move(body));
        // 格式化失败或机器人没有启动则终止
        if (!normalizedMessage || !AgentSystem::instance().isReady()) {
            return;
        }
        const uint64_t sessionId = MessageRecord::getSessionId(*normalizedMessage);
        const auto sessionState = getOrCreateSessionState(sessionId);
        {
            std::lock_guard lock(sessionState->queueMutex);
            sessionState->pendingPreparationMessages.push(std::move(*normalizedMessage));
            // 已经在进行该会话消息队列的预处理，加入队列即可，不需要启动该会话消息队列的预处理
            if (sessionState->isPreparationRunning) {
                return;
            }
            sessionState->isPreparationRunning = true;
        }
        Logger::session(sessionId).debug("消息预处理队列已唤醒");
        drogon::async_run([this, sessionId]() -> drogon::Task<> { co_await processPreparationQueue(sessionId); });
    }

    drogon::Task<> OneBotEventWorkflow::processPreparationQueue(const uint64_t sessionId) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        while (true) {
            json currentMessage;
            {
                std::lock_guard lock(sessionState->queueMutex);
                // 该会话需要预处理的队列中的消息已为空时退出
                if (sessionState->pendingPreparationMessages.empty()) {
                    sessionState->isPreparationRunning = false;
                    Logger::session(sessionId).debug("消息预处理队列已清空");
                    co_return;
                }
                currentMessage = std::move(sessionState->pendingPreparationMessages.front());
                sessionState->pendingPreparationMessages.pop();
            }

            try {
                if (!SessionConfigManager::contains(sessionId)) {
                    SessionConfigManager::addConfig(sessionId);
                }
                if (CommandProcessor::isCommand(currentMessage)) {
                    co_await executeCommand(currentMessage);
                    continue;
                }
                if (!SessionStore::isSessionEnabled(sessionId)) {
                    continue;
                }

                currentMessage = co_await MessageContentEnricher::enrichImages(std::move(currentMessage), sessionId);
                currentMessage = co_await MessageContentEnricher::injectMemories(std::move(currentMessage), sessionId);
                const bool queueWhileReplying = shouldQueueWhileReplying(currentMessage); // 是否可在以有回复任务时排队
                currentMessage.erase("session_id");

                const std::string serializedMessage = dumpJson(currentMessage);
                auto [messageSnapshot, summaryBatch, wasInserted] =
                  sessionState->messageList->append(std::move(currentMessage));
                if (wasInserted) {
                    pushRecordedMessage(sessionId, messageSnapshot.back(), serializedMessage);
                }
                scheduleConversationMaintenance(sessionId, sessionState->messageList, summaryBatch);

                bool shouldSkipReply = false; // 是否跳过回复
                {
                    std::lock_guard lock(sessionState->queueMutex);
                    if (sessionState->isReplyProcessing) {
                        if (queueWhileReplying) {
                            sessionState->pendingReplySnapshots.push(std::move(messageSnapshot));
                            continue;
                        }
                        shouldSkipReply = true;
                    } else {
                        sessionState->pendingReplySnapshots.push(std::move(messageSnapshot));
                        sessionState->isReplyProcessing = true;
                    }
                }
                if (shouldSkipReply) {
                    // 普通消息在同会话回复进行中仍会完成预处理，但不触发第二个 Agent 请求
                    recordMessageProcessingStats(sessionId);
                    continue;
                }
                drogon::async_run([this, sessionId]() -> drogon::Task<> { co_await processReplyQueue(sessionId); });
            } catch (...) {
                // 单条消息富化或收尾失败不能阻塞同会话后续消息。
            }
        }
    }

    drogon::Task<> OneBotEventWorkflow::processReplyQueue(const uint64_t sessionId) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        while (true) {
            json messageSnapshot;
            {
                std::lock_guard lock(sessionState->queueMutex);
                if (sessionState->pendingReplySnapshots.empty()) {
                    sessionState->isReplyProcessing = false;
                    co_return;
                }
                messageSnapshot = std::move(sessionState->pendingReplySnapshots.front());
                sessionState->pendingReplySnapshots.pop();
            }

            try {
                if (!AgentSystem::instance().isReady()) {
                    Logger::session(sessionId).debug("回复任务跳过：Agent 不可用");
                } else {
                    if (RouterDecision routerDecision = co_await MessageRouter::route(sessionId, messageSnapshot);
                      routerDecision.shouldReply) {
                        std::deque<json> recordSnapshot;
                        for (const json &message: messageSnapshot) {
                            recordSnapshot.push_back({{"content", dumpJson(message)}});
                        }
                        const ChatRecordManager records(sessionId, std::move(recordSnapshot));
                        const MemoryManager memory(sessionId);
                        const std::optional<ReplyDecision> replyDecision =
                          co_await execute(records, memory, std::move(routerDecision), messageSnapshot);
                        if (replyDecision && replyDecision->shouldReply && !replyDecision->content.empty()) {
                            if (SessionId::isPrivate(sessionId)) {
                                co_await MessageService::sendPrivateMsg(
                                  SessionId::privateUserId(sessionId), replyDecision->content);
                            } else {
                                co_await MessageService::sendGroupMsg(sessionId, replyDecision->content);
                            }
                        }
                    }
                }
            } catch (...) {
                // Router、Executor 或发送失败不影响同会话后续回复任务。
            }
            recordMessageProcessingStats(sessionId);
        }
    }
} // namespace insoulforge
