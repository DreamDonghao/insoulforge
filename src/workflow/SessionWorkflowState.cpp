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

        const bool startPreparation = !m_isPreparationRunning;
        m_isPreparationRunning = true;
        return startPreparation;
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

    auto SessionWorkflowState::recordIncoming(json message) -> bool {
        std::lock_guard lock(m_mutex);
        m_messageList->append(std::move(message));
        m_pendingProcessing = true;

        const bool startProcessing = !m_isProcessingRunning;
        m_isProcessingRunning = true;
        return startProcessing;
    }

    auto SessionWorkflowState::takeProcessingRequest() -> std::optional<json> {
        std::lock_guard lock(m_mutex);
        if (!m_pendingProcessing) {
            m_isProcessingRunning = false;
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
