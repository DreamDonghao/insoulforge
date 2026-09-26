/// @file MemoryStore.cpp
/// @brief 短期记忆存储 - 实现

#include <infrastructure/NumericTypes.hpp>

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::MemoryStore {
    std::string getShortTermMemory(const u64 sessionId) {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT memory_content FROM short_term_memory WHERE group_id = ?");
        stmt.bind(1, sessionId);
        return stmt.step() ? stmt.getText(0) : "";
    }

    void updateShortTermMemory(const u64 sessionId, const std::string &memory) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(), "INSERT INTO short_term_memory (group_id, memory_content) VALUES (?, ?) "
                                          "ON CONFLICT(group_id) DO UPDATE SET memory_content = "
                                          "excluded.memory_content, updated_at = CURRENT_TIMESTAMP");
        stmt.bind(1, sessionId);
        stmt.bind(2, memory);
        stmt.exec();
    }

} // namespace insoulforge::MemoryStore
