/// @file ToolRuntime.hpp
/// @brief 工具运行时门面
/// @details 聚合内置/自定义工具加载，以及自定义工具执行与 QQ 收藏表情支持。

#pragma once

#include <drogon/utils/coroutine.h>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <optional>
#include <string>

/// @brief 工具运行时服务
namespace insoulforge::ToolRuntime {
    /// @brief 注册全部编译期内置工具插件
    void registerBuiltinTools();

    /// @brief 从数据库重载启用的自定义工具
    /// @details 仅替换 custom 插件，不影响内置工具插件。
    void reloadCustomTools();

    /// @brief 执行 Python 脚本工具
    /// @param scriptContent Python脚本内容（直接存储在数据库中）
    /// @param args 传入参数
    auto executePythonTool(std::string scriptContent, json args) -> drogon::Task<std::string>;

    /// @brief 执行 HTTP 工具（sessionId 来自工具调用上下文）
    auto executeHttpTool(std::string config, json args, u64 sessionId) -> drogon::Task<std::string>;

    /// @brief 获取 QQ 收藏表情列表（调用 NapCat fetch_custom_face_detail，带60秒缓存）
    /// @return 归一化后的表情数组，失败时返回空数组
    auto fetchFavoriteEmojis(std::optional<u64> sessionId = std::nullopt) -> drogon::Task<json>;

    /// @brief 在收藏表情列表中按名称查找表情（名称 = desc 或 "表情N"）
    auto findFavoriteEmoji(std::string name, std::optional<u64> sessionId = std::nullopt) -> drogon::Task<json>;

    /// @brief 使收藏表情缓存失效（修改/删除后调用）
    void invalidateFavoriteEmojiCache();
} // namespace insoulforge::ToolRuntime
