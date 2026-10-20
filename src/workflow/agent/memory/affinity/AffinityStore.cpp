/// @file AffinityStore.cpp
/// @brief 好感度存储 - 实现

#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>


namespace insoulforge::AffinityStore {
    auto getAffinityMap() -> std::unordered_map<u64, i32> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT qq_number, affinity FROM person_affinity");
        std::unordered_map<u64, i32> affinityMap;
        while (stmt.step()) {
            affinityMap.emplace(static_cast<u64>(stmt.getInt64(0)), stmt.getInt(1));
        }
        return affinityMap;
    }

    void adjustAffinity(const u64 qqNumber, const i32 delta) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        // max/min 标量函数在 SQL 层夹紧，并发叠加也不会越界
        const Statement stmt(db.handle(),
          "INSERT INTO person_affinity (qq_number, affinity) VALUES (?, max(min(?, 100), -100)) "
          "ON CONFLICT(qq_number) DO UPDATE SET affinity = max(min(affinity + ?, 100), -100)");
        stmt.bind(1, qqNumber);
        stmt.bind(2, delta);
        stmt.bind(3, delta);
        stmt.execOrThrow();
    }
} // namespace insoulforge::AffinityStore
