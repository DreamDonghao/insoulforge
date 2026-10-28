/// @file MemoryStore.hpp
/// @brief 会话外号、聊天摘要与待迁移历史记忆存储
/// @details 复用 short_term_memory 表，历史内容成功整理后保存 aliases 和独立的 conversation_summaries。

#pragma once

#include <optional>
#include <string>
#include <vector>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 会话外号与历史内容存储
namespace insoulforge::MemoryStore {
    /// @brief 规范化外号，兼容尚未迁移的 recent_events 和 legacy_entries。
    [[nodiscard]] auto normalizeSessionMemory(const json &value) -> std::optional<json>;

    /// @brief 只向会话上下文提供结构化外号，不暴露待迁移历史内容。
    [[nodiscard]] auto projectShortTermMemory(const std::string &memory) -> json;

    /// @brief 按消息时间从旧到新返回最新 N 条会话聊天摘要，不作为普通消息重新提取。
    [[nodiscard]] auto conversationSummaries(const std::string &memory) -> json;

    /// @brief 读取尚未迁入长期记忆的旧事件、过渡条目或纯文本，去重。
    [[nodiscard]] auto pendingLegacyEntries(const std::string &memory) -> std::vector<std::string>;

    /// @brief 读取会话外号、聊天摘要和待迁移历史内容；不存在时返回空字符串
    [[nodiscard]] auto getShortTermMemory(u64 sessionId) -> std::string;

    /// @brief 保存会话外号、聊天摘要和待迁移历史内容；已有记录时覆盖内容并更新时间
    void updateShortTermMemory(u64 sessionId, const std::string &memory);

} // namespace insoulforge::MemoryStore
