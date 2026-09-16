/// @file MessageList.cpp
/// @brief 单会话完整消息列表实现

#include <algorithm>
#include <limits>

#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/maintenance/ConversationMaintenanceService.hpp>
#include <conversation/workflow/MessageList.hpp>
#include <infrastructure/config/Config.hpp>

namespace insoulforge {
    MessageList::MessageList(const uint64_t sessionId) : m_sessionId(sessionId) {
        // 总结任务完成前不能丢失其待删除前缀；模型可见范围由 snapshotLocked 单独限制。
        for (const json &record: ChatRecordStore::getChatRecords(sessionId, std::numeric_limits<int>::max())) {
            if (json message; tryParseJson(getStr(record, "content"), message) && message.is_object()) {
                m_messages.push_back(std::move(message));
            }
        }
        m_summaryBatchPending = ConversationMaintenanceService::hasPendingMemorySummary(sessionId);
    }

    std::optional<MessageListAppendResult> MessageList::append(json message) {
        message.erase("session_id");
        const std::string messageId = getStr(message, "message_id");
        std::lock_guard lock(m_mutex);
        if (!messageId.empty() && std::ranges::any_of(m_messages, [&messageId](const json &existing) {
                return getStr(existing, "message_id") == messageId;
            })) {
            return std::nullopt;
        }
        m_messages.push_back(std::move(message));
        return MessageListAppendResult{.messageSnapshot = snapshotLocked(), .summaryBatch = createSummaryBatchLocked()};
    }

    std::optional<MemorySummaryBatch> MessageList::removeCompletedSummaryMessages() {
        std::lock_guard lock(m_mutex);
        while (const auto completed = ConversationMaintenanceService::takeCompletedMemorySummary(m_sessionId)) {
            const size_t count = std::min(*completed, m_messages.size());
            for (size_t index = 0; index < count; ++index) {
                m_messages.pop_front();
            }
            m_summaryBatchPending = false;
        }
        return createSummaryBatchLocked();
    }

    void MessageList::cancelSummaryBatch() {
        std::lock_guard lock(m_mutex);
        m_summaryBatchPending = false;
    }

    json MessageList::snapshot() const {
        std::lock_guard lock(m_mutex);
        return snapshotLocked();
    }

    json MessageList::fullSnapshot() const {
        std::lock_guard lock(m_mutex);
        return fullSnapshotLocked();
    }

    void MessageList::flushToStorage() const {
        const json currentMessages = fullSnapshot();
        ChatRecordStore::clearSessionChatRecords(m_sessionId);
        for (const json &message: currentMessages) {
            const bool isAssistant = getStr(atOrNull(message, "sender"), "qq") == "self";
            ChatRecordStore::addChatRecord(m_sessionId, isAssistant ? "assistant" : "user", dumpJson(message));
        }
    }

    json MessageList::snapshotLocked() const {
        json result = json::array();
        const size_t limit = static_cast<size_t>(std::max(Config::instance().contextWindowLimit, 1));
        const size_t first = m_messages.size() > limit ? m_messages.size() - limit : 0;
        for (size_t index = first; index < m_messages.size(); ++index) {
            result.push_back(m_messages[index]);
        }
        return result;
    }

    json MessageList::fullSnapshotLocked() const {
        json result = json::array();
        for (const json &message: m_messages) {
            result.push_back(message);
        }
        return result;
    }

    std::optional<MemorySummaryBatch> MessageList::createSummaryBatchLocked() {
        const auto &config = Config::instance();
        if (const size_t trigger = static_cast<size_t>(std::max(config.memorySummaryTriggerCount, 1));
          m_summaryBatchPending || m_messages.size() < trigger) {
            return std::nullopt;
        }

        const size_t messageCount =
          std::min(static_cast<size_t>(std::max(config.memorySummaryBatchSize, 1)), m_messages.size());
        const size_t contextCount = std::min(
          static_cast<size_t>(std::max(config.memorySummaryContextCount, 0)), m_messages.size() - messageCount);
        MemorySummaryBatch batch{.messages = json::array(), .contextMessages = json::array()};
        for (size_t index = 0; index < messageCount; ++index) {
            batch.messages.push_back(m_messages[index]);
        }
        for (size_t index = messageCount; index < messageCount + contextCount; ++index) {
            batch.contextMessages.push_back(m_messages[index]);
        }
        m_summaryBatchPending = true;
        return batch;
    }
} // namespace insoulforge
