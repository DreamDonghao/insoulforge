/// @file LongTermMemoryStore.hpp
/// @brief 长期记忆存储
/// @details 表：long_term_memory（记忆内容 + embedding 向量 BLOB，检索为暴力余弦）

#pragma once

#include <cstdint>
#include <infrastructure/NumericTypes.hpp>
#include <string>
#include <vector>

namespace insoulforge {
    /// @brief 长期记忆条目（管理端展示用）
    struct LongTermMemoryEntry {
        i64 id;
        u64 groupId;
        std::string content;
        std::string createdAt;
    };

    /// @brief 相似检索命中（含 id，供召回合并后删除被取代的原条目）
    struct SimilarMemory {
        i64 id{0};
        std::string content;
        f32 similarity{0.0F};
    };

    /// @brief 长期记忆存储
    namespace LongTermMemoryStore {
        /// @brief 暴力余弦检索 topK 条相似记忆
        /// @return (id, 内容, 余弦相似度)，按相似度降序；维度不匹配的行跳过
        [[nodiscard]] auto searchSimilar(u64 groupId, const std::vector<f32> &query, i32 topK)
          -> std::vector<SimilarMemory>;

        /// @brief 分页列出长期记忆（新→旧）；sessionId 为 0 时列出全部会话
        [[nodiscard]] auto listMemories(u64 sessionId, i32 limit, i32 offset) -> std::vector<LongTermMemoryEntry>;

        /// @brief 统计长期记忆条数；sessionId 为 0 时统计全部会话
        [[nodiscard]] auto countMemories(u64 sessionId) -> i64;

        /// @brief 删除一条长期记忆
        /// @return 是否删除成功（id 不存在返回 false）
        auto deleteMemory(i64 id) -> bool;
    } // namespace LongTermMemoryStore
} // namespace insoulforge
