/// @file PersonMemoryStore.hpp
/// @brief 全局人物档案的上下文读取，不受会话共享开关影响。

#pragma once

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::PersonMemoryStore {
    /// @brief 按最近聊天的真实发送者去重、批量读取档案，排除机器人、系统和没有记忆的人。
    [[nodiscard]] auto getForSenders(const json &recentConversation) -> json;
    [[nodiscard]] auto listPeople() -> json;
    [[nodiscard]] auto getPerson(u64 qq) -> json;
} // namespace insoulforge::PersonMemoryStore
