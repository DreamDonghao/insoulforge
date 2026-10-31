/// @file MemoryStore.hpp
/// @brief 会话外号、聊天摘要与待整理条目存储
/// @details 复用 short_term_memory 表，条目成功整理后保存 aliases 和独立的 conversation_summaries。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 会话外号与待整理条目存储
namespace insoulforge::MemoryStore {
    /// @brief 规范化当前外号、待整理条目和聊天摘要。
    [[nodiscard]] auto normalizeSessionMemory(const json &value) -> std::optional<json>;

    /// @brief 只向会话上下文提供结构化外号，不暴露待整理条目。
    [[nodiscard]] auto projectShortTermMemory(const std::string &memory) -> json;

    /// @brief 按消息时间从旧到新返回最新 N 条会话聊天摘要，不作为普通消息重新提取。
    [[nodiscard]] auto conversationSummaries(const std::string &memory) -> json;

    /// @brief 读取尚未整理进长期记忆的条目。
    [[nodiscard]] auto pendingEntries(const std::string &memory) -> std::vector<std::string>;

    /// @brief 读取会话外号、聊天摘要和待整理条目；不存在时返回空字符串
    [[nodiscard]] auto getShortTermMemory(u64 sessionId) -> std::string;

    /// @brief 保存会话外号、聊天摘要和待整理条目；已有记录时覆盖内容并更新时间
    void updateShortTermMemory(u64 sessionId, const std::string &memory);

} // namespace insoulforge::MemoryStore
