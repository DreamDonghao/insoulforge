/// @file SessionWorkflowState.cpp
/// @brief 单个会话的消息工作流并发状态实现

#include <infrastructure/NumericTypes.hpp>
#include <workflow/SessionWorkflowState.hpp>

namespace insoulforge {
    SessionWorkflowState::SessionWorkflowState(const u64 sessionId, const Database &database, const Config &config) :
        m_messageList(std::make_shared<MessageList>(sessionId, database, config)) {}

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

    auto SessionWorkflowState::isReplyProcessing() -> bool {
        std::lock_guard lock(m_mutex);
        return m_isReplyProcessing;
    }

    auto SessionWorkflowState::requestReplyProcessing(std::string triggerMessageId, const bool mayWaitForCurrentReply,
      std::optional<RouterDecision> decision) -> std::optional<ReplyRequest> {
        std::lock_guard lock(m_mutex);
        if (m_isReplyProcessing) {
            if (mayWaitForCurrentReply) {
                m_pendingReply = ReplyRequest{std::move(triggerMessageId), std::move(decision)};
            }
            return std::nullopt;
        }
        m_isReplyProcessing = true;
        return ReplyRequest{std::move(triggerMessageId), std::move(decision)};
    }

    auto SessionWorkflowState::completeReplyProcessing() -> std::optional<ReplyRequest> {
        std::lock_guard lock(m_mutex);
        if (m_pendingReply) {
            return std::exchange(m_pendingReply, std::nullopt);
        }
        m_isReplyProcessing = false;
        return std::nullopt;
    }
} // namespace insoulforge
