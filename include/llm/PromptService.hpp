/// @file PromptService.hpp
/// @brief 提示词服务 - 动态加载与管理 LLM 提示词
/// @author donghao
/// @date 2026-04-02
/// @details 从数据库加载和管理提示词，支持：
///          - 运行时动态修改提示词
///          - 占位符替换（如 {botName}）
///          - 默认提示词初始化

#pragma once
#include <string>

namespace insoulforge {
    /// @brief 提示词服务 - 管理所有 LLM 使用的提示词，支持运行时修改
    namespace PromptService {
        /// @brief 初始化提示词（如果不存在则插入默认值）
        void initialize();

        /// @brief 获取提示词（支持占位符替换）
        /// @param key 提示词键名
        /// @return 提示词内容，已替换 {botName} 等占位符
        std::string getPrompt(const std::string &key);

        /// @brief 设置提示词（运行时修改）
        /// @param key 提示词键名
        /// @param content 提示词内容
        void setPrompt(const std::string &key, const std::string &content);

        /// @brief 获取 Executor 系统提示词（群聊）
        /// @return Executor 角色系统提示词
        [[nodiscard]] std::string getExecutorSystemPrompt();

        /// @brief 获取 Executor 系统提示词（私聊）
        /// @return Executor 私聊角色系统提示词
        [[nodiscard]] std::string getExecutorPrivateSystemPrompt();

        /// @brief 获取 Router 系统提示词（群聊）
        /// @return Router 消息路由决策提示词
        [[nodiscard]] std::string getRouterSystemPrompt();

        /// @brief 获取 Router 系统提示词（私聊）
        /// @return Router 私聊消息路由决策提示词
        [[nodiscard]] std::string getRouterPrivateSystemPrompt();
    } // namespace PromptService
}