/// @file SessionWorkflowState.cpp
/// @brief 单个会话的消息工作流并发状态实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/workflow/SessionWorkflowState.hpp>

namespace insoulforge {
    SessionWorkflowState::SessionWorkflowState(const u64 sessionId) :
        m_messageList(std::make_shared<MessageList>(sessionId)) {}

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

    auto SessionWorkflowState::requestReplyProcessing(std::string triggerMessageId, const bool mayWaitForCurrentReply)
      -> std::optional<std::string> {
        std::lock_guard lock(m_mutex);
        if (m_isReplyProcessing) {
            if (mayWaitForCurrentReply) {
                m_pendingReplyMessageId = std::move(triggerMessageId);
            }
            return std::nullopt;
        }
        m_isReplyProcessing = true;
        return triggerMessageId;
    }

    auto SessionWorkflowState::completeReplyProcessing() -> std::optional<std::string> {
        std::lock_guard lock(m_mutex);
        if (m_pendingReplyMessageId) {
            return std::exchange(m_pendingReplyMessageId, std::nullopt);
        }
        m_isReplyProcessing = false;
        return std::nullopt;
    }
} // namespace insoulforge
