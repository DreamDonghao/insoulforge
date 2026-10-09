/// @file OneBotEventWorkflow.cpp
/// @brief OneBot 入站事件处理工作流实现

#include <cassert>

#include <admin/access/BlacklistStore.hpp>
#include <admin/events/WebSocketManager.hpp>
#include <agent/runtime/AgentSystem.hpp>
#include <agent/runtime/ExecutorAgent.hpp>
#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/maintenance/ConversationMaintenanceService.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/SessionConfigManager.hpp>
#include <conversation/workflow/CommandProcessor.hpp>
#include <conversation/workflow/MessageContentEnricher.hpp>
#include <conversation/workflow/MessageRouter.hpp>
#include <conversation/workflow/OneBotEventNormalizer.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <onebot/messaging/MessageService.hpp>

namespace insoulforge {
    namespace {
        /// @brief 判断已有回复任务时是否保留该消息作为下一轮触发目标
        [[nodiscard]] auto shouldQueueWhileReplying(const json &message, const Config &config) -> bool {
            return MessageRecord::isSystem(message) || MessageRecord::mentions(message, config.selfQQNumber);
        }

        /// @brief 记录已完成主处理消息的会话统计
        void recordMessageProcessingStats(const u64 sessionId) {
            try {
                SessionConfigManager::incrementMessageCount(sessionId);
            } catch (const std::exception &error) {
                Logger::error(sessionId, "Workflow", fmt::format("会话统计更新失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", fmt::format("会话统计更新失败: 未知异常"));
            }
        }

        /// @brief 将已插入的完整消息记录推送至管理后台，推送失败不得影响消息工作流
        /// @param sessionId 所属会话 ID
        /// @param message 已写入消息列表的完整消息
        void pushRecordedMessage(const u64 sessionId, const json &message) {
            try {
                const auto role = MessageRecord::isAssistant(message) ? "assistant" : "user";
                WebSocketManager::instance().pushMessage(sessionId, role, dumpJson(message));
            } catch (const std::exception &error) {
                Logger::error(sessionId, "Workflow", fmt::format("管理后台消息推送失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", fmt::format("管理后台消息推送失败: 未知异常"));
            }
        }

        /// @brief 原子持久化会话派生状态任务，再分别启动异步消费者
        void scheduleConversationMaintenance(const Database &database, const u64 sessionId,
          const std::shared_ptr<MessageList> &messageList, const MemorySummaryBatch &batch) {
            try {
                ConversationMaintenanceService::enqueue(database, sessionId, batch.messages, batch.contextMessages);
            } catch (const std::exception &error) {
                messageList->cancelSummaryBatch();
                Logger::error(sessionId, "Workflow", fmt::format("会话派生状态维护任务持久化失败: {}", error.what()));
            } catch (...) {
                messageList->cancelSummaryBatch();
                Logger::error(sessionId, "Workflow", fmt::format("会话派生状态维护任务持久化失败: 未知异常"));
            }
        }
    } // namespace

    auto OneBotEventWorkflow::instance() -> OneBotEventWorkflow & {
        static auto workflow = OneBotEventWorkflow{};
        return workflow;
    }


    auto OneBotEventWorkflow::initialize(const Database &database, const Config &config, const AgentSystem &agent)
      -> std::expected<void, std::string> {
        if (m_initialized.load(std::memory_order_acquire)) {
            if (&m_dependencies->database != &database || &m_dependencies->config != &config ||
                &m_dependencies->agent != &agent) {
                return std::unexpected("工作流已经初始化，不能替换依赖对象");
            }
            return {};
        }
        if (!database.handle()) {
            return std::unexpected("工作流初始化要求数据库已成功初始化");
        }
        if (!agent.isReady()) {
            return std::unexpected("工作流初始化要求 Agent 已就绪");
        }
        try {
            std::unordered_map<u64, std::shared_ptr<SessionWorkflowState>> sessions;
            for (const auto sessionId: ChatRecordStore::getSessionIds(database)) {
                sessions.emplace(sessionId, std::make_shared<SessionWorkflowState>(sessionId, database, config));
            }
            // 先完成恢复查询，再发布会话索引，避免查询失败留下部分恢复状态。
            std::vector<std::pair<u64, MemorySummaryBatch>> summaryBatches;
            for (const auto &[sessionId, sessionState]: sessions) {
                if (auto batch = sessionState->messageList()->removeCompletedSummaryMessages()) {
                    summaryBatches.emplace_back(sessionId, std::move(*batch));
                }
            }
            {
                std::lock_guard lock(m_sessionsMutex);
                m_sessions = std::move(sessions);
                m_dependencies.emplace(Dependencies{database, config, agent});
            }
            ConversationMaintenanceService::setMemorySummaryCompletedCallback([this](const u64 sessionId) {
                const auto sessionState = getOrCreateSessionState(sessionId);
                if (const auto batch = sessionState->messageList()->removeCompletedSummaryMessages()) {
                    scheduleConversationMaintenance(
                      m_dependencies->database, sessionId, sessionState->messageList(), *batch);
                }
            });
            m_initialized.store(true, std::memory_order_release);
            ConversationMaintenanceService::resumePending(database);
            for (const auto &[sessionId, batch]: summaryBatches) {
                scheduleConversationMaintenance(
                  database, sessionId, getOrCreateSessionState(sessionId)->messageList(), batch);
            }
            Logger::info(0, "Workflow", "事件工作流初始化完成");
            return {};
        } catch (const DbError &error) {
            m_initialized.store(false, std::memory_order_release);
            ConversationMaintenanceService::setMemorySummaryCompletedCallback({});
            std::lock_guard lock(m_sessionsMutex);
            m_sessions.clear();
            m_dependencies.reset();
            return std::unexpected(std::string("事件工作流恢复失败: ") + error.what());
        }
    }

    void OneBotEventWorkflow::flushMessageListsToStorage() {
        auto messageLists = std::vector<std::shared_ptr<MessageList>>{};
        {
            std::lock_guard lock(m_sessionsMutex);
            for (const auto &sessionState: m_sessions | std::views::values) {
                messageLists.push_back(sessionState->messageList());
            }
        }
        for (const auto &messageList: messageLists) {
            messageList->flushToStorage();
        }
    }

    void OneBotEventWorkflow::appendDeliveredAssistantMessage(const u64 sessionId, json message) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto update = sessionState->messageList()->append(std::move(message));
        if (!update) {
            return;
        }
        pushRecordedMessage(sessionId, update->messageSnapshot.back());
        if (update->summaryBatch) {
            scheduleConversationMaintenance(
              m_dependencies->database, sessionId, sessionState->messageList(), *update->summaryBatch);
        }
    }

    void OneBotEventWorkflow::appendAssistantExecutionRecord(const u64 sessionId, json message) {
        appendSystemStatusMessage(sessionId, std::move(message));
    }

    void OneBotEventWorkflow::appendSystemStatusMessage(const u64 sessionId, json message) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto update = sessionState->messageList()->append(std::move(message));
        if (!update) {
            return;
        }
        pushRecordedMessage(sessionId, update->messageSnapshot.back());
        if (update->summaryBatch) {
            scheduleConversationMaintenance(
              m_dependencies->database, sessionId, sessionState->messageList(), *update->summaryBatch);
        }
    }

    auto OneBotEventWorkflow::getSessionMessages(const u64 sessionId) -> std::optional<json> {
        std::shared_ptr<SessionWorkflowState> sessionState;
        {
            std::lock_guard lock(m_sessionsMutex);
            const auto found = m_sessions.find(sessionId);
            if (found == m_sessions.end()) {
                return std::nullopt;
            }
            sessionState = found->second;
        }
        return sessionState->messageList()->fullSnapshot();
    }

    auto OneBotEventWorkflow::getOrCreateSessionState(const u64 sessionId) -> std::shared_ptr<SessionWorkflowState> {
        std::lock_guard lock(m_sessionsMutex);
        if (const auto found = m_sessions.find(sessionId); found != m_sessions.end()) {
            return found->second;
        }
        assert(m_initialized.load(std::memory_order_acquire) && "OneBotEventWorkflow::initialize must succeed first");
        auto state =
          std::make_shared<SessionWorkflowState>(sessionId, m_dependencies->database, m_dependencies->config);
        m_sessions.emplace(sessionId, state);
        return state;
    }

    auto OneBotEventWorkflow::executeCommand(const json &message) -> drogon::Task<> {
        const auto sessionId = getUInt(message, "session_id");
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto update = sessionState->messageList()->append(message);
        if (!update) {
            co_return;
        }
        pushRecordedMessage(sessionId, update->messageSnapshot.back());

        try {
            const auto response = co_await CommandProcessor::execute(message);
            if (SessionId::isPrivate(sessionId)) {
                co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), response);
            } else {
                co_await MessageService::sendGroupMsg(sessionId, response);
            }
        } catch (const std::exception &error) {
            Logger::error(sessionId, "Workflow", fmt::format("命令执行或回复失败: {}", error.what()));
        } catch (...) {
            Logger::error(sessionId, "Workflow", fmt::format("命令执行或回复失败: 未知错误"));
        }
        if (update->summaryBatch) {
            scheduleConversationMaintenance(
              m_dependencies->database, sessionId, sessionState->messageList(), *update->summaryBatch);
        }
        co_return;
    }

    void OneBotEventWorkflow::enqueueOneBotEvent(json body) {
        if (!m_initialized.load(std::memory_order_acquire)) {
            return;
        }
        // 将上报转换为统一消息，再按会话进入预处理队列。
        auto normalizedMessage = OneBotEventNormalizer::normalize(std::move(body));
        if (!normalizedMessage || MessageRecord::isAssistant(*normalizedMessage) || !m_dependencies->agent.isReady()) {
            return;
        }
        if (BlacklistStore::contains(getUInt(atOrNull(*normalizedMessage, "sender"), "qq"))) {
            return;
        }
        const auto sessionId = MessageRecord::getSessionId(*normalizedMessage);
        const auto sessionState = getOrCreateSessionState(sessionId);
        if (!sessionState->enqueuePreparation(std::move(*normalizedMessage))) {
            return;
        }
        drogon::async_run([this, sessionId]() -> drogon::Task<> { co_await processPreparationQueue(sessionId); });
    }

    auto OneBotEventWorkflow::processPreparationQueue(const u64 sessionId) -> drogon::Task<> {
        const auto sessionState = getOrCreateSessionState(sessionId);
        while (true) {
            auto nextMessage = sessionState->takePreparationMessage();
            if (!nextMessage) {
                co_return;
            }
            auto currentMessage = json(std::move(*nextMessage));

            try {
                if (!SessionConfigManager::contains(sessionId)) {
                    SessionConfigManager::addConfig(sessionId);
                }
                if (CommandProcessor::isCommand(currentMessage)) {
                    Logger::info(
                      sessionId, "Command", fmt::format("执行: {}", MessageRecord::extractText(currentMessage)));
                    co_await executeCommand(currentMessage);
                    continue;
                }
                if (!SessionStore::isSessionEnabled(sessionId)) {
                    continue;
                }

                Logger::info(sessionId, "Message",
                  fmt::format("收到: {}", dumpJson(MessageRecord::projectForAgent(currentMessage))));

                currentMessage = co_await MessageContentEnricher::enrichImages(std::move(currentMessage), sessionId);
                currentMessage = co_await MessageContentEnricher::injectMemories(std::move(currentMessage), sessionId);
                currentMessage.erase("session_id");

                const auto update = sessionState->messageList()->append(std::move(currentMessage));
                if (!update) {
                    continue;
                }

                pushRecordedMessage(sessionId, update->messageSnapshot.back());

                if (update->summaryBatch) {
                    scheduleConversationMaintenance(
                      m_dependencies->database, sessionId, sessionState->messageList(), *update->summaryBatch);
                }

                const auto triggerMessageId = getStr(update->messageSnapshot.back(), "message_id");
                if (const auto replyTrigger = sessionState->requestReplyProcessing(triggerMessageId,
                      shouldQueueWhileReplying(update->messageSnapshot.back(), m_dependencies->config));
                  replyTrigger) {
                    drogon::async_run([this, sessionId, triggerMessageId = *replyTrigger]() -> drogon::Task<> {
                        co_await processReplyWorkflow(sessionId, triggerMessageId);
                    });
                } else {
                    recordMessageProcessingStats(sessionId);
                }
            } catch (const std::exception &error) {
                // 单条消息富化或收尾失败不能阻塞同会话后续消息。
                Logger::error(sessionId, "Workflow", fmt::format("消息处理失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", "消息处理失败: 未知异常");
            }
        }
    }

    auto OneBotEventWorkflow::processReplyWorkflow(u64 sessionId, std::string triggerMessageId) -> drogon::Task<> {
        const auto sessionState = getOrCreateSessionState(sessionId);
        bool isInitialReply = true;
        while (true) {
            // 每轮回复均读取最新快照，避免遗漏等待期间已发送的助手消息。
            const auto messageSnapshot = sessionState->messageList()->snapshot();

            try {
                if (!m_dependencies->agent.isReady()) {
                    Logger::warn(sessionId, "Workflow", "回复任务跳过：Agent 不可用");
                } else {
                    auto routerDecision = co_await MessageRouter::route(sessionId, triggerMessageId, messageSnapshot);

                    Logger::info(sessionId, "Router",
                      fmt::format("决策={} | reason={} | priority={} | maxLength={}",
                        routerDecision.shouldReply ? "reply" : "skip", routerDecision.reason, routerDecision.isPriority,
                        routerDecision.maxLength));

                    if (routerDecision.shouldReply) {
                        auto recordSnapshot = std::deque<json>{};
                        for (const auto &message: messageSnapshot) {
                            recordSnapshot.push_back({{"content", dumpJson(message)}});
                        }
                        const auto records = ChatRecordManager{sessionId, std::move(recordSnapshot)};
                        const auto memory = MemoryManager{sessionId};
                        const auto replyDecision =
                          co_await ExecutorAgent::execute(records, memory, std::move(routerDecision), messageSnapshot);
                        if (replyDecision && replyDecision->shouldReply && !replyDecision->content.empty()) {
                            std::optional<u64> sentMessageId;
                            if (SessionId::isPrivate(sessionId)) {
                                sentMessageId =
                                  co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId),
                                    replyDecision->content, std::nullopt, replyDecision->toolHistory);
                            } else {
                                sentMessageId = co_await MessageService::sendGroupMsg(
                                  sessionId, replyDecision->content, std::nullopt, replyDecision->toolHistory);
                            }
                            if (!sentMessageId) {
                                appendAssistantExecutionRecord(sessionId,
                                  MessageRecord::createAssistantExecutionRecord(m_dependencies->config.botName + "(我)",
                                    replyDecision->toolHistory, "send_failed",
                                    "OneBot 未确认发送成功，不能认为回复已送达"));
                            }
                        } else {
                            Logger::info(sessionId, "Executor", "未生成可发送的回复");
                            if (replyDecision) {
                                appendAssistantExecutionRecord(sessionId,
                                  MessageRecord::createAssistantExecutionRecord(m_dependencies->config.botName + "(我)",
                                    replyDecision->toolHistory,
                                    replyDecision->failureReason.empty() ? "no_reply" : "failed",
                                    replyDecision->failureReason));
                            }
                        }
                    }
                }
            } catch (const std::exception &error) {
                // Router、Executor 或发送失败不影响同会话后续回复任务。
                Logger::error(sessionId, "Workflow", fmt::format("回复处理失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", "回复处理失败: 未知异常");
            }
            if (isInitialReply) {
                recordMessageProcessingStats(sessionId);
                isInitialReply = false;
            }
            const auto nextTriggerMessageId = sessionState->completeReplyProcessing();
            if (!nextTriggerMessageId) {
                co_return;
            }
            triggerMessageId = *nextTriggerMessageId;
        }
    }
} // namespace insoulforge
