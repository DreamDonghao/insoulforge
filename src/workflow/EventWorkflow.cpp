/// @file EventWorkflow.cpp
/// @brief 统一事件处理工作流实现

#include <algorithm>

#include <cassert>
#include <drogon/utils/Utilities.h>

#include <admin/events/WebSocketManager.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <onebot/incoming/OneBotEventNormalizer.hpp>
#include <workflow/EventWorkflow.hpp>
#include <workflow/MessageRouter.hpp>
#include <workflow/access/BlacklistStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/command/CommandProcessor.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/MessageContentEnricher.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/MessageService.hpp>
#include <workflow/message/image/ImageDescriptionStore.hpp>
#include <workflow/session/SessionConfigManager.hpp>

namespace insoulforge {
    namespace {
        /// @brief 记录已完成主处理消息的会话统计
        void recordMessageProcessingStats(const SessionId &sessionId) {
            try {
                SessionConfigManager::incrementMessageCount(sessionId);
                WebSocketManager::instance().broadcastEvent(
                  "session_stats_updated", {{"sessionId", sessionId.toString()},
                                             {"messageCount", SessionConfigManager::getConfig(sessionId).allMesCount}});
            } catch (const std::exception &error) {
                Logger::error(sessionId, "Workflow", fmt::format("会话统计更新失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", fmt::format("会话统计更新失败: 未知异常"));
            }
        }

        /// @brief 将已插入的完整消息记录推送至管理后台，推送失败不得影响消息工作流
        /// @param sessionId 所属会话 ID
        /// @param message 已写入消息列表的完整消息
        void pushRecordedMessage(const SessionId &sessionId, const json &message) {
            try {
                const auto role = MessageRecord::isAssistant(message) ? "assistant" : "user";
                WebSocketManager::instance().pushMessage(sessionId, role, dumpJson(message));
            } catch (const std::exception &error) {
                Logger::error(sessionId, "Workflow", fmt::format("管理后台消息推送失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", fmt::format("管理后台消息推送失败: 未知异常"));
            }
        }

        /// @brief 消息数达到阈值且没有记忆维护任务时，启动后台记忆维护。
        /// @details 成功后从内存消息列表中移除已处理的旧消息，失败则保留。
        void scheduleMemoryMaintenance(
          const Database &database, const SessionId &sessionId, const std::shared_ptr<MessageList> &messageList) {
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

    auto EventWorkflow::instance() -> EventWorkflow & {
        static auto workflow = EventWorkflow{};
        return workflow;
    }

    auto EventWorkflow::initialize(const Database &database, const Config &config, const AgentSystem &agent)
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
        try {
            // 在接收消息前清理超过十天未使用的图片描述缓存。
            const auto expiredImageCount = u64{ImageDescriptionStore::purgeExpired()};
            Logger::info(
              std::nullopt, "Media", fmt::format("过期图片描述缓存清理完成 | deleted={}", expiredImageCount));

            std::unordered_map<SessionId, std::shared_ptr<SessionWorkflowState>> sessions;
            for (const auto &sessionId: ChatRecordStore::getSessionIds(database)) {
                sessions.emplace(sessionId, std::make_shared<SessionWorkflowState>(sessionId, database, config));
            }
            {
                std::lock_guard lock(m_sessionsMutex);
                m_sessions = std::move(sessions);
                m_dependencies.emplace(Dependencies{.database = database, .config = config, .agent = agent});
            }
            m_initialized.store(true, std::memory_order_release);
            Logger::info(std::nullopt, "Workflow", "事件工作流初始化完成");
            return {};
        } catch (const DbError &error) {
            m_initialized.store(false, std::memory_order_release);
            std::lock_guard lock(m_sessionsMutex);
            m_sessions.clear();
            m_dependencies.reset();
            return std::unexpected(std::string("事件工作流恢复失败: ") + error.what());
        }
    }

    void EventWorkflow::flushMessageListsToStorage() {
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

    void EventWorkflow::appendDeliveredAssistantMessage(const SessionId &sessionId, json message) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto update = sessionState->messageList()->append(std::move(message));
        pushRecordedMessage(sessionId, update.back());
    }

    void EventWorkflow::appendAssistantExecutionRecord(const SessionId &sessionId, json message) {
        appendSystemStatusMessage(sessionId, std::move(message));
    }

    void EventWorkflow::appendSystemStatusMessage(const SessionId &sessionId, json message) {
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto update = sessionState->messageList()->append(std::move(message));
        pushRecordedMessage(sessionId, update.back());
    }

    auto EventWorkflow::getSessionMessages(const SessionId &sessionId) -> std::optional<json> {
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

    auto EventWorkflow::getSharedSessionMessages(
      const SessionId &sessionId, const size_t limit, const std::string &earliest, const std::string &latest) -> json {
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

    void EventWorkflow::clearSessionMessages(const SessionId &sessionId) {
        std::lock_guard lock(m_sessionsMutex);
        if (const auto found = m_sessions.find(sessionId); found != m_sessions.end()) {
            found->second->messageList()->clear();
        } else {
            ChatRecordStore::clearSessionChatRecords(sessionId);
        }
    }

    auto EventWorkflow::getOrCreateSessionState(const SessionId &sessionId) -> std::shared_ptr<SessionWorkflowState> {
        std::lock_guard lock(m_sessionsMutex);
        if (const auto found = m_sessions.find(sessionId); found != m_sessions.end()) {
            return found->second;
        }
        assert(m_initialized.load(std::memory_order_acquire) && "EventWorkflow::initialize must succeed first");
        auto state =
          std::make_shared<SessionWorkflowState>(sessionId, m_dependencies->database, m_dependencies->config);
        m_sessions.emplace(sessionId, state);
        return state;
    }

    auto EventWorkflow::executeCommand(const json &message) -> drogon::Task<> {
        const auto sessionId = MessageRecord::getSessionId(message);
        const auto sessionState = getOrCreateSessionState(sessionId);
        const auto update = sessionState->messageList()->append(message);
        pushRecordedMessage(sessionId, update.back());

        try {
            const auto response = co_await CommandProcessor::execute(message);
            co_await MessageService::sendToSession(sessionId, response);
        } catch (const std::exception &error) {
            Logger::error(sessionId, "Workflow", fmt::format("命令执行或回复失败: {}", error.what()));
        } catch (...) {
            Logger::error(sessionId, "Workflow", fmt::format("命令执行或回复失败: 未知错误"));
        }
        co_return;
    }

    void EventWorkflow::enqueueOneBotEvent(const json &body) {
        if (!m_initialized.load(std::memory_order_acquire)) {
            return;
        }
        if (auto message = OneBotEventNormalizer::normalize(body)) {
            enqueueNormalizedMessage(std::move(*message));
        }
    }

    void EventWorkflow::enqueueSystemMessage(const SessionId &sessionId, std::string text, std::string senderName) {
        static std::atomic<u64> next{0};
        enqueueNormalizedMessage({{"session_id", sessionId.toString()}, {"time", currentDateTime()},
          {"message_id", fmt::format("{}", 9000000000ULL + next.fetch_add(1))},
          {"sender", {{"user_id", "system"}, {"name", std::move(senderName)}}},
          {"segments", {{{"type", "text"}, {"text", std::move(text)}}}}});
    }

    void EventWorkflow::enqueueNormalizedMessage(json message) {
        if (!m_initialized.load(std::memory_order_acquire) || !m_dependencies->agent.isRunning() ||
            MessageRecord::isAssistant(message)) {
            return;
        }
        if (const auto sender = UserId::parse(getStr(atOrNull(message, "sender"), "user_id"));
          !sender || BlacklistStore::contains(*sender)) {
            return;
        }
        const auto sessionId = MessageRecord::getSessionId(message);
        const auto sessionState = getOrCreateSessionState(sessionId);
        if (!sessionState->enqueuePreparation(std::move(message))) {
            return;
        }
        drogon::async_run([this, sessionId]() -> drogon::Task<> { co_await processPreparationQueue(sessionId); });
    }

    void EventWorkflow::enqueueActivityMessage(const SessionId &sessionId, std::string text, std::string name,
      std::string mode, const std::vector<Image> &attachments) {
        auto message = json({{"session_id", sessionId.toString()}, {"time", currentDateTime()},
          {"message_id", "activity-" + drogon::utils::getUuid()},
          {"sender", {{"user_id", "system"}, {"name", "Activity：" + name}}},
          {"activity", {{"name", std::move(name)}, {"mode", std::move(mode)}}},
          {"segments", {{{"type", "text"}, {"text", std::move(text)}}}}});
        for (const auto &image: attachments) {
            const auto index = message["assets"]["images"].is_array() ? message["assets"]["images"].size() : 0;
            message["segments"].push_back({{"type", "image"}, {"image_index", index}});
            message["assets"]["images"].push_back({{"image_id", image.id()}, {"recognition_status", "available"}});
        }
        enqueueNormalizedMessage(std::move(message));
    }

    auto EventWorkflow::processPreparationQueue(const SessionId sessionId) -> drogon::Task<> {
        const auto sessionState = getOrCreateSessionState(sessionId);
        while (auto nextMessage = sessionState->takePreparationMessage()) {
            auto currentMessage = json(std::move(*nextMessage));

            try {
                if (!SessionConfigManager::contains(sessionId)) {
                    SessionStore::initializeSession(
                      sessionId, m_dependencies->config.defaultPrivateEnabled.load(std::memory_order_relaxed));
                }
                if (CommandProcessor::isCommand(currentMessage)) {
                    Logger::info(
                      sessionId, "Command", fmt::format("执行: {}", MessageRecord::extractText(currentMessage)));
                    co_await executeCommand(currentMessage);
                } else if (SessionStore::isSessionEnabled(sessionId)) {
                    Logger::info(sessionId, "Message",
                      fmt::format("收到: {}", dumpJson(MessageRecord::projectForAgent(currentMessage))));

                    currentMessage =
                      co_await MessageContentEnricher::enrichImages(std::move(currentMessage), sessionId);
                    currentMessage =
                      co_await MessageContentEnricher::injectMemories(std::move(currentMessage), sessionId);
                    currentMessage.erase("session_id");

                    const auto [records, startReply] = sessionState->recordIncoming(std::move(currentMessage));

                    pushRecordedMessage(sessionId, records.back());
                    recordMessageProcessingStats(sessionId);

                    if (startReply) {
                        drogon::async_run(
                          [this, sessionId]() -> drogon::Task<> { co_await processReplyWorkflow(sessionId); });
                    }
                } else {
                    continue;
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

    auto EventWorkflow::processReplyWorkflow(SessionId sessionId) -> drogon::Task<> {
        const auto sessionState = getOrCreateSessionState(sessionId);
        while (auto request = sessionState->takeReplyRequest()) {
            try {
                if (m_dependencies->agent.isRunning() && SessionStore::isSessionEnabled(sessionId)) {
                    auto routerDecision = co_await MessageRouter::route(sessionId, request->records);

                    Logger::info(sessionId, "Router",
                      fmt::format("决策={} | reason={} | priority={} | maxLength={}", routerDecision.shouldReply,
                        routerDecision.reason, routerDecision.isPriority,
                        routerDecision.maxLength ? std::to_string(*routerDecision.maxLength) : "未指定"));

                    if (routerDecision.shouldReply) {
                        auto [inputRecords] = sessionState->takeExecutorRequest();
                        const auto records = ChatRecordManager{sessionId, std::move(inputRecords)};
                        co_await ExecutorAgent::execute(
                          records, sessionId.isPrivate() ? std::nullopt : std::optional{std::move(routerDecision)});
                    }
                } else {
                    Logger::warn(sessionId, "Workflow", "回复任务跳过：Agent 不可用");
                }
            } catch (const std::exception &error) {
                // Router、Executor 或发送失败不影响同会话后续回复任务。
                Logger::error(sessionId, "Workflow", fmt::format("回复处理失败: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Workflow", "回复处理失败: 未知异常");
            }
        }
    }
} // namespace insoulforge
