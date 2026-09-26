/// @file ToolStore.hpp
/// @brief 自定义工具存储
/// @details 表：custom_tools（工具定义与脚本）、settings（Python 解释器路径）

#pragma once

#include <string>
#include <vector>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 自定义工具存储
    namespace ToolStore {
        /// @brief 自定义工具结构
        struct CustomTool {
            i32 id = 0;
            std::string name; ///< 工具名，如 search_web
            std::string description; ///< 提供给模型的工具说明
            std::string parameters; ///< JSON Schema 字符串
            std::string executorType; ///< python 或 http
            std::string executorConfig; ///< HTTP 执行配置 JSON
            std::string scriptContent; ///< Python 脚本内容
            std::string readme; ///< 面向管理员的 Markdown 说明
            bool enabled = true;
        };

        /// @brief 获取所有自定义工具
        [[nodiscard]] auto getCustomTools() -> std::vector<CustomTool>;

        /// @brief 获取启用的自定义工具（供 ToolRuntime 使用）
        [[nodiscard]] auto getEnabledCustomTools() -> std::vector<CustomTool>;

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

        /// @brief 获取自定义工具使用的 Python 解释器路径
        [[nodiscard]] auto getCustomToolPython() -> std::string;

        /// @brief 设置自定义工具使用的 Python 解释器路径
        void setCustomToolPython(const std::string &pythonPath);
    } // namespace ToolStore
} // namespace insoulforge
