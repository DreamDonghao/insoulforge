/// @file ToolArgument.hpp
/// @brief 工具调用参数的读取与校验辅助函数

#pragma once

#include <string>
#include <charconv>
#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge {
    /// @brief 从工具调用参数中读取字符串
    /// @param args 工具调用参数
    /// @param key 参数名称
    /// @return 参数值；缺失或类型不匹配时返回空字符串
    inline auto argString(const json &args, const char *key) -> std::string { return getStr(args, key); }

    /// @brief 严格读取必填 group_id，仅接受有效的数字字符串群号。
    inline auto groupIdArgument(const json &args) -> std::expected<u64, std::string> {
        const auto &value = atOrNull(args, "group_id");
        if (!value.is_string()) return std::unexpected("group_id 必须是数字字符串群号");
        const auto &text = value.get_ref<const std::string &>();
        u64 groupId = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), groupId);
        if (error != std::errc{} || end != text.data() + text.size() || groupId == 0 ||
            SessionId::isPrivate(groupId) || groupId == SessionId::kSystemAccountId)
            return std::unexpected("group_id 无效，必须填写有效的群号，不能指定私聊会话");
        return groupId;
    }
} // namespace insoulforge
