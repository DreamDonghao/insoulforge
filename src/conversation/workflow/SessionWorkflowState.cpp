/// @file SessionWorkflowState.cpp
/// @brief 单个会话的消息工作流并发状态实现

#include <conversation/workflow/SessionWorkflowState.hpp>

namespace insoulforge {
    SessionWorkflowState::SessionWorkflowState(const uint64_t sessionId) :
        m_messageList(std::make_shared<MessageList>(sessionId)) {}

    const std::shared_ptr<MessageList> &SessionWorkflowState::messageList() const noexcept { return m_messageList; }

    bool SessionWorkflowState::enqueuePreparation(json message) {
        std::lock_guard lock(m_mutex);
        m_pendingPreparationMessages.push(std::move(message));
        if (m_isPreparationRunning) {
            return false;
        }
        m_isPreparationRunning = true;
        return true;
    }

    std::optional<json> SessionWorkflowState::takePreparationMessage() {
        std::lock_guard lock(m_mutex);
        if (m_pendingPreparationMessages.empty()) {
            m_isPreparationRunning = false;
            return std::nullopt;
        }
        json message = std::move(m_pendingPreparationMessages.front());
        m_pendingPreparationMessages.pop();
        return message;
    }

    SessionWorkflowState::ReplyRequestResult SessionWorkflowState::requestReplyProcessing(
      const bool mayWaitForCurrentReply) {
        std::lock_guard lock(m_mutex);
        if (m_isReplyProcessing) {
            if (!mayWaitForCurrentReply) {
                return ReplyRequestResult::Skipped;
            }
            m_hasPendingReply = true;
            return ReplyRequestResult::Pending;
        }
        m_isReplyProcessing = true;
        return ReplyRequestResult::StartProcessor;
    }

    bool SessionWorkflowState::completeReplyProcessing() {
        std::lock_guard lock(m_mutex);
        if (m_hasPendingReply) {
            m_hasPendingReply = false;
            return true;
        }
        m_isReplyProcessing = false;
        return false;
    }
} // namespace insoulforge
