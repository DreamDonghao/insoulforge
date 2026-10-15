/// @file MemoryMaintenanceStore.hpp
/// @brief 聊天记忆与好感度计算结果的事务存储

#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class Database;

    /// @brief 已完成向量化、可直接写入的长期记忆变更
    struct PreparedLongTermMemory {
        std::string content;
        std::vector<f32> embedding;
        std::vector<i64> replacedIds;
    };

    /// @brief 聊天记忆计算结果；短期记忆为空值时保留原内容
    struct MemoryMaintenanceResult {
        std::optional<std::string> shortTerm;
        std::vector<PreparedLongTermMemory> longTerm;
    };

    using AffinityDeltas = std::vector<std::pair<u64, i32>>;

    namespace MemoryMaintenanceStore {
        /// @brief 在同一事务中保存聊天记忆与好感度；任何写入失败时全部回滚
        /// @note 调用前完成所有模型请求，事务内不执行网络操作。
        void apply(const Database &database, u64 sessionId, const MemoryMaintenanceResult &memory,
          const AffinityDeltas &affinity);
    } // namespace MemoryMaintenanceStore
} // namespace insoulforge
