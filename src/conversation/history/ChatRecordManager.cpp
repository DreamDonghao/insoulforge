/// @file ChatRecordManager.cpp
/// @brief Agent 会话记录快照适配器实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/history/ChatRecordManager.hpp>

namespace insoulforge {
    ChatRecordManager::ChatRecordManager(const u64 sessionId, std::deque<json> records) :
        m_sessionId(sessionId), m_records(std::move(records)) {}

    auto ChatRecordManager::getSessionId() const -> u64 { return m_sessionId; }

    auto ChatRecordManager::getRecords() const -> std::deque<json> { return m_records; }
} // namespace insoulforge
