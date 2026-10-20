/// @file MemoryStore.hpp
/// @brief 短期记忆存储
/// @details 表：short_term_memory（当前会话的短期记忆）

#pragma once

#include <optional>
#include <string>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

/// @brief 短期记忆存储
namespace insoulforge::MemoryStore {
    /// @brief 校验并规范化会话称呼、近期事件及可选旧内容过渡区，去重且不限制条数。
    [[nodiscard]] auto normalizeSessionMemory(const json &value) -> std::optional<json>;

    /// @brief 新结构直接注入对象，旧纯文本仍按行注入，等待下一次成功整理转换。
    [[nodiscard]] auto projectShortTermMemory(const std::string &memory) -> json;

    /// @brief 读取会话短期记忆；不存在时返回空字符串
    [[nodiscard]] auto getShortTermMemory(u64 sessionId) -> std::string;

    /// @brief 保存会话短期记忆；已有记录时覆盖内容并更新时间
    void updateShortTermMemory(u64 sessionId, const std::string &memory);

} // namespace insoulforge::MemoryStore
