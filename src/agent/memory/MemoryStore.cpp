/// @file MemoryStore.cpp
/// @brief 短期记忆存储 - 实现
/// @author donghao
/// @date 2026-08-30

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::MemoryStore {
    std::string getShortTermMemory(const uint64_t sessionId) {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT memory_content FROM short_term_memory WHERE group_id = ?");
        stmt.bind(1, sessionId);
        return stmt.step() ? stmt.getText(0) : "";
    }

    // upsert 而非 REPLACE：手动编辑记忆(后台)时不能重置水位线
    void updateShortTermMemory(const uint64_t sessionId, const std::string &memory) {
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
