/// @file LongTermMemoryStore.hpp
/// @brief 长期记忆存储
/// @author donghao
/// @date 2026-09-01
/// @details 表：long_term_memory（记忆内容 + embedding 向量 BLOB，检索为暴力余弦）

#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace insoulforge {
    /// @brief 长期记忆条目（管理端展示用）
    struct LongTermMemoryEntry {
        int64_t id;
        uint64_t groupId;
        std::string content;
        std::string createdAt;
    };

    /// @brief 相似检索命中（含 id，供召回合并后删除被取代的原条目）
    struct SimilarMemory {
        int64_t id{0};
        std::string content;
        float similarity{0.0F};
    };

    /// @brief 长期记忆存储
    namespace LongTermMemoryStore {
        /// @brief 暴力余弦检索 topK 条相似记忆
        /// @return (id, 内容, 余弦相似度)，按相似度降序；维度不匹配的行跳过
        [[nodiscard]] std::vector<SimilarMemory> searchSimilar(
          uint64_t groupId, const std::vector<float> &query, int topK);

        /// @brief 分页列出长期记忆（新→旧）；sessionId 为 0 时列出全部会话
        [[nodiscard]] std::vector<LongTermMemoryEntry> listMemories(uint64_t sessionId, int limit, int offset);

        /// @brief 统计长期记忆条数；sessionId 为 0 时统计全部会话
        [[nodiscard]] int64_t countMemories(uint64_t sessionId);

        /// @brief 删除一条长期记忆
        /// @return 是否删除成功（id 不存在返回 false）
        bool deleteMemory(int64_t id);
    } // namespace LongTermMemoryStore
} // namespace insoulforge
