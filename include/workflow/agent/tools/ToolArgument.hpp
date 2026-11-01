/// @file ToolArgument.hpp
/// @brief 工具调用参数的读取与校验辅助函数

#pragma once

#include <string>

#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/UserId.hpp>

namespace insoulforge {
    /// @brief 从工具调用参数中读取字符串
    /// @param args 工具调用参数
    /// @param key 参数名称
    /// @return 参数值；缺失或类型不匹配时返回空字符串
    inline auto argString(const json &args, const char *key) -> std::string { return getStr(args, key); }

    /// @brief 读取用户身份；数字协议和私聊会话限制在各自边界校验。
    inline auto userIdArgument(const json &args) -> std::expected<UserId, std::string> {
        const auto &value = atOrNull(args, "user_id");
        if (!value.is_string())
            return std::unexpected("user_id 必须是字符串用户 ID");
        const auto id = UserId::parse(value.get_ref<const std::string &>());
        if (!id || id->toString() == "self" || id->toString() == "system")
            return std::unexpected("user_id 必须是有效的用户 ID，不能使用机器人或系统身份");
        return id;
    }

    /// @brief 将原始群 ID 转换为明确的群聊会话。
    inline auto groupIdArgument(const json &args) -> std::expected<SessionId, std::string> {
        const auto &value = atOrNull(args, "group_id");
        if (!value.is_string())
            return std::unexpected("group_id 必须是字符串群 ID");
        const auto target = UserId::parse(value.get_ref<const std::string &>());
        if (!target)
            return std::unexpected("group_id 无效：" + target.error());
        return SessionId::group(target->toString());
    }
} // namespace insoulforge
