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

    void SessionWorkflowState::enqueueCompose(json message) {
        std::lock_guard lock(m_mutex);
        m_pendingComposeMessages.push(std::move(message));
    }

    auto SessionWorkflowState::tryClaimCompose() -> bool {
        std::lock_guard lock(m_mutex);
        if (m_isComposing || m_pendingComposeMessages.empty()) {
            return false;
        }
        m_isComposing = true;
        return true;
    }

    void SessionWorkflowState::recordIncoming(json message) {
        std::lock_guard lock(m_mutex);
        m_messageList->append(std::move(message));
        m_pendingProcessing = true;
    }
    auto SessionWorkflowState::tryClaimProcess() -> bool {
        std::lock_guard lock(m_mutex);
        if (m_isProcessing || !m_pendingProcessing) {
            return false;
        }
        m_isProcessing = true;
        return true;
    }

    auto SessionWorkflowState::takeComposeEvent() -> std::optional<json> {
        std::lock_guard lock(m_mutex);
        if (m_pendingComposeMessages.empty()) {
            m_isComposing = false;
            return std::nullopt;
        }
        json message = std::move(m_pendingComposeMessages.front());
        m_pendingComposeMessages.pop();
        return message;
    }

    auto SessionWorkflowState::takeProcessEvents() -> std::optional<json> {
        std::lock_guard lock(m_mutex);
        if (!m_pendingProcessing) {
            m_isProcessing = false;
            return std::nullopt;
        }
        m_pendingProcessing = false;
        return m_messageList->snapshot();
    }

    auto SessionWorkflowState::takeExecutorRequest() -> json {
        std::lock_guard lock(m_mutex);
        auto records = m_messageList->executorSnapshot();
        m_pendingProcessing = false;
        return records;
    }
} // namespace insoulforge
