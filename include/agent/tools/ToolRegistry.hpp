/// @file ToolRegistry.hpp
/// @brief 工具注册中心 - Agent 工具的分类管理与执行
/// @author donghao
/// @date 2026-03-28
/// @details 提供工具的注册、管理和执行功能：
///          - 分类管理：REPLY（回复工具）、INFORMATION（信息工具）、ACTION（动作工具）
///          - 工具执行：支持异步执行和上下文传递
///          - 工具定义生成：生成 OpenAI 兼容的工具定义 JSON

#pragma once
#include <drogon/utils/coroutine.h>
#include <functional>
#include <infrastructure/JsonUtil.hpp>
#include <map>
#include <string>
#include <vector>

namespace insoulforge {
    /// @brief 单次工具调用的执行环境（随调用显式传参，无共享状态）
    struct ToolCallContext {
        uint64_t sessionId = 0;
        json conversationContext; ///< system 之后的完整消息列表，deep_think 等需要会话上下文的工具使用
        json messageSnapshot; ///< 本轮冻结的完整消息快照，图片保存等工具读取服务端媒体来源
    };

    /// @brief 异步工具处理器
    // args/ctx 按值传递：协程帧独立持有参数（规范见 docs/CODING_STYLE.md 协程参数规范）
    using ToolHandler = std::function<drogon::Task<std::string>(json args, ToolCallContext ctx)>;

    /// @brief 工具可被注入到哪些会话类型
    enum class ToolScope {
        ALL_SESSIONS, ///< 群聊和私聊请求均可注入
        GROUP_ONLY, ///< 仅群聊请求可注入，私聊在请求 LLM 前排除
    };

    /// @brief 一次模型请求的工具筛选条件
    struct ToolQuery {
        bool isPrivateSession = false; ///< 是否为私聊会话
    };

    /// @brief 工具定义与运行时元数据
    struct Tool {
        std::string name;
        std::string description;
        json parameters; // JSON Schema
        ToolHandler handler;
        ToolScope scope = ToolScope::ALL_SESSIONS; ///< 工具注入的会话范围
        /// 同分类内的稳定展示优先级；数值越小越靠前，名称作为最终排序键
        int promptOrder = 0;
    };

    /// @brief 工具分类
    enum class ToolCategory {
        REPLY, // 回复工具：reply, no_reply, reply_with_quote（结束处理）
        INFORMATION, // 信息工具：recall_memory, list_stickers（获取数据）
        ACTION // 动作工具：send_face, send_sticker, ban_user, send_poke 等（执行操作）
    };

    /// @brief 工具注册中心，分类管理工具
    class ToolRegistry {
    public:
        /// @brief 在插件注册上下文中调用的工具声明函数
        using PluginRegistrar = std::function<void(ToolRegistry &registry)>;

        static ToolRegistry &instance();

        /// @brief 注册或重载一个进程内工具插件
        /// @details 注册失败时保留该插件此前的工具；其他插件不受影响。
        bool registerPlugin(std::string pluginId, const PluginRegistrar &registrar);

        /// @brief 卸载一个插件及其注册的全部工具
        void unregisterPlugin(const std::string &pluginId);

        /// @brief 在当前插件注册回调中注册工具到指定分类
        /// @return 成功注册返回 true；名称已被其他插件占用时返回 false
        bool registerTool(const Tool &tool, ToolCategory category);

        /// @brief 按会话能力筛选并获取工具定义
        /// @details 输出顺序固定为：分类、promptOrder、名称，避免请求间顺序漂移。
        [[nodiscard]] json getTools(const ToolQuery &query) const;

        /// @brief 获取所有工具定义（兼容管理与诊断用途）
        [[nodiscard]] json getAllTools() const;

        /// @brief 执行工具（异步，ctx 随调用传给 handler）
        [[nodiscard]] drogon::Task<std::string> executeTool(std::string name, json args, ToolCallContext ctx) const;

        /// @brief 检查工具是否存在
        [[nodiscard]] bool hasTool(const std::string &name) const;

        /// @brief 注销工具
        void unregisterTool(const std::string &name);

    private:
        ToolRegistry() = default;

        /// @brief 带归属信息的内部工具记录
        struct RegisteredTool {
            Tool tool;
            ToolCategory category;
            std::string pluginId;
        };

        // 名称全局唯一，杜绝自定义工具覆盖内置工具后出现定义与执行不一致。
        std::map<std::string, RegisteredTool> m_tools;
        std::map<std::string, std::vector<std::string>> m_pluginTools;
        std::string m_activePluginId;
        bool m_pluginRegistrationFailed = false;

        static std::string categoryToString(ToolCategory category);
        static int categoryOrder(ToolCategory category);
    };
} // namespace insoulforge
