/// @file AffinityStore.cpp
/// @brief 好感度存储 - 实现
/// @author donghao
/// @date 2026-08-31

#include <conversation/maintenance/affinity/AffinityStore.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>


namespace insoulforge::AffinityStore {
    std::unordered_map<uint64_t, int> getAffinityMap(const uint64_t sessionId) {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT qq_number, affinity FROM group_affinity WHERE group_id = ?");
        stmt.bind(1, sessionId);
        std::unordered_map<uint64_t, int> affinityMap;
        while (stmt.step()) {
            affinityMap.emplace(static_cast<uint64_t>(stmt.getInt64(0)), stmt.getInt(1));
        }
        return affinityMap;
    }

    void adjustAffinity(const uint64_t sessionId, const uint64_t qqNumber, const int delta) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        // max/min 标量函数在 SQL 层夹紧，并发叠加也不会越界
        const Statement stmt(db.handle(),
          "INSERT INTO group_affinity (group_id, qq_number, affinity) VALUES (?, ?, max(min(?, 100), -100)) "
          "ON CONFLICT(group_id, qq_number) DO UPDATE SET affinity = max(min(affinity + ?, 100), -100)");
        stmt.bind(1, sessionId);
        stmt.bind(2, qqNumber);
        stmt.bind(3, delta);
        stmt.bind(4, delta);
        stmt.exec();
    }
} // namespace insoulforge::AffinityStore
