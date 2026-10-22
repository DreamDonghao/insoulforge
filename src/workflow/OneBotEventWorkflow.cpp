/// @file OneBotEventWorkflow.cpp
/// @brief OneBot 入站事件处理工作流实现

#include <cassert>

#include <admin/events/WebSocketManager.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <onebot/incoming/OneBotEventNormalizer.hpp>
#include <workflow/MessageRouter.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/access/BlacklistStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/command/CommandProcessor.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/MessageContentEnricher.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/MessageService.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/message/image/ImageDescriptionStore.hpp>
#include <workflow/session/SessionConfigManager.hpp>

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

        /// @brief 预留一个内存批次并启动后台维护，成功后删除对应数量的队首消息
        void scheduleMemoryMaintenance(
          const Database &database, const u64 sessionId, const std::shared_ptr<MessageList> &messageList) {
            auto batch = messageList->createSummaryBatch();
            if (!batch) {
                return;
            }
            try {
                drogon::async_run(
                  [&database, sessionId, messageList, batch = std::move(*batch)]() mutable -> drogon::Task<> {
                      const size_t removeCount = batch.messages.size();
                      bool completed = false;
                      try {
                          completed = co_await MemoryMaintenanceCoordinator::process(
                            database, sessionId, std::move(batch.messages), std::move(batch.contextMessages));
                      } catch (const std::exception &error) {
                          Logger::error(sessionId, "Memory", fmt::format("记忆维护失败: {}", error.what()));
                      } catch (...) {
                          Logger::error(sessionId, "Memory", "记忆维护失败: 未知异常");
                      }
                      messageList->finishSummaryBatch(completed ? removeCount : 0);
                      if (!completed) {
                          Logger::warn(sessionId, "Memory", "本次记忆维护未完成，等待后续消息重新触发");
                      }
                  });
            } catch (...) {
                messageList->finishSummaryBatch(0);
                throw;
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
            // 在接收消息前清理超过十天未使用的图片描述缓存。
            const auto expiredImageCount = u64{ImageDescriptionStore::purgeExpired()};
            Logger::info(0, "Media", fmt::format("过期图片描述缓存清理完成 | deleted={}", expiredImageCount));

            std::unordered_map<u64, std::shared_ptr<SessionWorkflowState>> sessions;
            for (const auto sessionId: ChatRecordStore::getSessionIds(database)) {
                sessions.emplace(sessionId, std::make_shared<SessionWorkflowState>(sessionId, database, config));
            }
            {
                std::lock_guard lock(m_sessionsMutex);
                m_sessions = std::move(sessions);
                m_dependencies.emplace(Dependencies{.database = database, .config = config, .agent = agent});
            }
            m_initialized.store(true, std::memory_order_release);
            Logger::info(0, "Workflow", "事件工作流初始化完成");
            return {};
        } catch (const DbError &error) {
            m_initialized.store(false, std::memory_order_release);
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
        pushRecordedMessage(sessionId, update->back());
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
        pushRecordedMessage(sessionId, update->back());
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

    auto OneBotEventWorkflow::getSharedSessionMessages(
      const u64 sessionId, const size_t limit, const std::string &earliest, const std::string &latest) -> json {
        std::shared_ptr<SessionWorkflowState> state;
        {
            std::lock_guard lock(m_sessionsMutex);
            const auto found = m_sessions.find(sessionId);
            if (found == m_sessions.end()) {
                return json::array();
            }
            state = found->second;
        }
        return state->messageList()->sharedSnapshot(limit, earliest, latest);
    }

    void OneBotEventWorkflow::clearSessionMessages(const u64 sessionId) {
        std::lock_guard lock(m_sessionsMutex);
        if (const auto found = m_sessions.find(sessionId); found != m_sessions.end()) {
            found->second->messageList()->clear();
        } else {
            ChatRecordStore::clearSessionChatRecords(sessionId);
        }
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
        pushRecordedMessage(sessionId, update->back());

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
        co_return;
    }

    void OneBotEventWorkflow::enqueueOneBotEvent(const json &body) {
        if (!m_initialized.load(std::memory_order_acquire)) {
            return;
        }
        // 将上报转换为统一消息，再按会话进入预处理队列。
        auto normalizedMessage = OneBotEventNormalizer::normalize(body);
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
                } else if (!SessionStore::isSessionEnabled(sessionId)) {
                    continue;
                } else {
                    Logger::info(sessionId, "Message",
                      fmt::format("收到: {}", dumpJson(MessageRecord::projectForAgent(currentMessage))));

                    currentMessage =
                      co_await MessageContentEnricher::enrichImages(std::move(currentMessage), sessionId);
                    currentMessage =
                      co_await MessageContentEnricher::injectMemories(std::move(currentMessage), sessionId);
                    currentMessage.erase("session_id");

                    const auto update = sessionState->messageList()->append(std::move(currentMessage));
                    if (!update) {
                        continue;
                    }

                    pushRecordedMessage(sessionId, update->back());

                    const auto triggerMessageId = getStr(update->back(), "message_id");
                    if (const auto replyTrigger = sessionState->requestReplyProcessing(
                          triggerMessageId, shouldQueueWhileReplying(update->back(), m_dependencies->config));
                      replyTrigger) {
                        drogon::async_run([this, sessionId, triggerMessageId = *replyTrigger]() -> drogon::Task<> {
                            co_await processReplyWorkflow(sessionId, triggerMessageId);
                        });
                    } else {
                        recordMessageProcessingStats(sessionId);
                    }
                }
                scheduleMemoryMaintenance(m_dependencies->database, sessionId, sessionState->messageList());
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
            auto messageSnapshot = sessionState->messageList()->snapshot();

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
                        const auto records = ChatRecordManager{sessionId, std::move(messageSnapshot)};
                        const auto memory = MemoryManager{sessionId};
                        const auto replyDecision =
                          co_await ExecutorAgent::execute(records, memory, std::move(routerDecision));
                        if (replyDecision && replyDecision->shouldReply && !replyDecision->content.empty()) {
                            if (SessionId::isPrivate(sessionId)) {
                                co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId),
                                  replyDecision->content, std::nullopt, replyDecision->toolHistory);
                            } else {
                                co_await MessageService::sendGroupMsg(
                                  sessionId, replyDecision->content, std::nullopt, replyDecision->toolHistory);
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
