/// @file SessionWorkflowState.cpp
/// @brief 单个会话的消息工作流并发状态实现

#include <utility>

#include <infrastructure/SessionId.hpp>
#include <workflow/SessionWorkflowState.hpp>

namespace insoulforge {
    SessionWorkflowState::SessionWorkflowState(const SessionId &sessionId, const Database &database,
      const Config &config) : m_messageList(std::make_shared<MessageList>(sessionId, database, config)) {}

    auto SessionWorkflowState::messageList() const noexcept -> const std::shared_ptr<MessageList> & {
        return m_messageList;
    }

    auto SessionWorkflowState::enqueuePreparation(json message) -> bool {
        std::lock_guard lock(m_mutex);
        m_pendingPreparationMessages.push(std::move(message));
        if (m_isPreparationRunning) {
            return false;
        }
        m_isPreparationRunning = true;
        return true;
    }

    auto SessionWorkflowState::takePreparationMessage() -> std::optional<json> {
        std::lock_guard lock(m_mutex);
        if (m_pendingPreparationMessages.empty()) {
            m_isPreparationRunning = false;
            return std::nullopt;
        }
        json message = std::move(m_pendingPreparationMessages.front());
        m_pendingPreparationMessages.pop();
        return message;
    }

    auto SessionWorkflowState::recordIncoming(json message) -> IncomingMessageUpdate {
        std::lock_guard lock(m_mutex);
        auto records = m_messageList->append(std::move(message));
        m_pendingReply = true;
        const bool startReply = !m_isReplyProcessing;
        m_isReplyProcessing = true;
        return IncomingMessageUpdate{.records = std::move(records), .startReply = startReply};
    }

    auto SessionWorkflowState::takeReplyRequest() -> std::optional<ReplyRequest> {
        std::lock_guard lock(m_mutex);
        if (!m_pendingReply) {
            m_isReplyProcessing = false;
            return std::nullopt;
        }
        m_pendingReply = false;
        return ReplyRequest{m_messageList->snapshot()};
    }

    auto SessionWorkflowState::takeExecutorRequest() -> ReplyRequest {
        std::lock_guard lock(m_mutex);
        auto records = m_messageList->executorSnapshot();
        m_pendingReply = false;
        return ReplyRequest{std::move(records)};
    }
} // namespace insoulforge
