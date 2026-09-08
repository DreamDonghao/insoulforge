/// @file ChatRecordManager.cpp
/// @brief Agent 会话记录快照适配器实现

#include <conversation/history/ChatRecordManager.hpp>

namespace insoulforge {
    ChatRecordManager::ChatRecordManager(const uint64_t sessionId, std::deque<json> records) :
        m_sessionId(sessionId), m_records(std::move(records)) {}

    uint64_t ChatRecordManager::getSessionId() const { return m_sessionId; }

    std::deque<json> ChatRecordManager::getRecords() const { return m_records; }
} // namespace insoulforge
