/// @file ToolStore.hpp
/// @brief 自定义工具存储
/// @details 工具定义和 Lua 脚本位于数据库同级 tools 目录。

#pragma once

#include <string>
#include <vector>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class Database;
    /// @brief 自定义工具存储
    namespace ToolStore {
        /// @brief 自定义工具结构
        struct CustomTool {
            i32 id = 0;
            std::string name; ///< 工具名，如 search_web
            std::string description; ///< 提供给模型的工具说明
            std::string parameters; ///< JSON Schema 字符串
            std::string executorType; ///< lua
            std::string scriptContent; ///< Lua 脚本内容
            std::string readme; ///< 面向管理员的 Markdown 说明
            bool enabled = true;
        };

        /// @brief 获取所有自定义工具
        [[nodiscard]] auto getCustomTools() -> std::vector<CustomTool>;

        /// @brief 获取启用的自定义工具（供 ToolRuntime 使用）
        [[nodiscard]] auto getEnabledCustomTools(const Database &database) -> std::vector<CustomTool>;

        /// @brief 添加自定义工具
        [[nodiscard]] auto addCustomTool(const CustomTool &tool) -> i32;

        /// @brief 更新自定义工具
        void updateCustomTool(const CustomTool &tool);

        /// @brief 删除自定义工具
        void deleteCustomTool(i32 id);

        /// @brief 切换自定义工具启用状态
        void toggleCustomTool(i32 id);

        /// @brief 检查工具名是否已存在
        [[nodiscard]] auto hasCustomTool(const std::string &name) -> bool;

    } // namespace ToolStore
} // namespace insoulforge
