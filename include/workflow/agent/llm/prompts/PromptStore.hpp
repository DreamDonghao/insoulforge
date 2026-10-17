/// @file PromptStore.hpp
/// @brief 提示词存储
/// @details 表：prompts（可编辑的系统提示词模板）

#pragma once
#include <string>
#include <unordered_map>

namespace insoulforge {
    class Database;
}


/// @brief 提示词存储
namespace insoulforge::PromptStore {
    /// @brief 使用指定数据库读取提示词；缺失时返回默认值。
    [[nodiscard]] auto getPrompt(const Database &database, const std::string &key, const std::string &defaultValue = "")
      -> std::string;

    /// @brief 使用指定数据库保存提示词。
    void setPrompt(const Database &database, const std::string &key, const std::string &content,
      const std::string &description = "");

    /// @brief 使用指定数据库检查提示词是否存在。
    [[nodiscard]] auto hasPrompt(const Database &database, const std::string &key) -> bool;

    // 运行时接口沿用进程级数据库；启动初始化使用上方显式数据库接口。
    [[nodiscard]] auto getPrompt(const std::string &key, const std::string &defaultValue = "") -> std::string;

    void setPrompt(const std::string &key, const std::string &content, const std::string &description = "");

    [[nodiscard]] auto hasPrompt(const std::string &key) -> bool;

    [[nodiscard]] auto getAllPrompts() -> std::unordered_map<std::string, std::string>;
} // namespace insoulforge::PromptStore
