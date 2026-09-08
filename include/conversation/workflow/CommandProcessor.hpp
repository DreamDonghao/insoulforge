/// @file CommandProcessor.hpp
/// @brief 消息工作流的管理命令处理

#pragma once

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::CommandProcessor {
    /// @brief 判断统一消息是否为发给机器人的管理命令
    /// @param message 统一消息 JSON
    /// @return 私聊命令或 @ 机器人且以 `/` 开头的群聊命令返回 true
    [[nodiscard]] bool isCommand(const json &message);

    /// @brief 执行统一消息中的管理命令
    /// @param message 已经通过 isCommand 判断的统一消息 JSON
    /// @return 应回复给命令发起者的执行结果
    drogon::Task<std::string> execute(const json &message);
} // namespace insoulforge::CommandProcessor
