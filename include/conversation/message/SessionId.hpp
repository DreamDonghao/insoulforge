/// @file SessionId.hpp
/// @brief 统一会话 ID 的构造与解析工具

#pragma once

#include <cstdint>
#include <infrastructure/NumericTypes.hpp>
#include <string>
#include <utility>

namespace insoulforge::SessionId {
    /// @brief 系统定时任务使用的虚拟发送者 QQ 号
    /// @details 该号码不对应真实 QQ 账号，用于让路由与后处理可靠识别系统生成的消息。
    inline constexpr u64 kSystemAccountId = 10'000'000'000ULL;

    /// @brief 私聊会话标志位，私聊会话 ID = 用户 QQ 号 | 此标志位
    inline constexpr u64 kPrivateSessionFlag = 1ULL << 63;

    /// @brief 构造私聊会话 ID
    /// @param userId 私聊对象的 QQ 号
    /// @return 带私聊标志位的统一会话 ID
    [[nodiscard]] constexpr u64 fromPrivateUser(const u64 userId) { return userId | kPrivateSessionFlag; }

    /// @brief 判断统一会话 ID 是否表示私聊
    [[nodiscard]] constexpr bool isPrivate(const u64 sessionId) { return (sessionId & kPrivateSessionFlag) != 0; }

    /// @brief 从私聊会话 ID 提取目标用户 QQ 号
    [[nodiscard]] constexpr u64 privateUserId(const u64 sessionId) { return sessionId & ~kPrivateSessionFlag; }

    /// @brief 将统一会话 ID 转换为存储层使用的会话类型与目标 ID
    /// @param sessionId 群聊 ID 或带私聊标志位的私聊 ID
    /// @return {"group"|"private", 群号或 QQ 号}
    [[nodiscard]] inline std::pair<std::string, u64> toStorageTarget(const u64 sessionId) {
        return {
          isPrivate(sessionId) ? "private" : "group", isPrivate(sessionId) ? privateUserId(sessionId) : sessionId};
    }
} // namespace insoulforge::SessionId
