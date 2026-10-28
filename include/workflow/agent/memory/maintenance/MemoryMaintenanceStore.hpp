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
    };

    struct PersonMemoryEntry {
        u64 qq;
        std::string content;
    };

    using AffinityDeltas = std::vector<std::pair<u64, i32>>;

    /// @brief 聊天记忆计算结果；shortTerm 复用旧列保存外号和独立聊天摘要，为空值时保留原内容
    struct MemoryMaintenanceResult {
        std::optional<std::string> shortTerm;
        std::optional<std::string> conversationSummary; // 本批消息的聊天摘要，不参与人物或向量记忆提取
        std::vector<PreparedLongTermMemory> longTerm;
        // 实际交给整理模型的旧条目，由程序整体替换，不依赖模型报告来源。
        std::vector<i64> replacedLongTermIds;
        std::vector<PersonMemoryEntry> personEntries;
        AffinityDeltas affinity;
    };

    namespace MemoryMaintenanceStore {
        /// @brief 在同一事务中保存聊天记忆与好感度；任何写入失败时全部回滚
        /// @note 调用前完成所有模型请求，事务内不执行网络操作。
        void apply(const Database &database, u64 sessionId, const MemoryMaintenanceResult &memory);
    } // namespace MemoryMaintenanceStore
} // namespace insoulforge
