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

    SessionWorkflowState::ReplyEnqueueResult SessionWorkflowState::enqueueReplySnapshot(
      json snapshot, const bool mayQueueWhileProcessing) {
        std::lock_guard lock(m_mutex);
        if (m_isReplyProcessing) {
            if (!mayQueueWhileProcessing) {
                return ReplyEnqueueResult::Skipped;
            }
            m_pendingReplySnapshots.push(std::move(snapshot));
            return ReplyEnqueueResult::Queued;
        }
        m_pendingReplySnapshots.push(std::move(snapshot));
        m_isReplyProcessing = true;
        return ReplyEnqueueResult::StartConsumer;
    }

    std::optional<json> SessionWorkflowState::takeReplySnapshot() {
        std::lock_guard lock(m_mutex);
        if (m_pendingReplySnapshots.empty()) {
            m_isReplyProcessing = false;
            return std::nullopt;
        }
        json snapshot = std::move(m_pendingReplySnapshots.front());
        m_pendingReplySnapshots.pop();
        return snapshot;
    }
} // namespace insoulforge
