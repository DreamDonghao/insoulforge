/// @file PromptStore.hpp
/// @brief 提示词存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：prompts（可编辑的系统提示词模板）

#pragma once
#include <string>
#include <unordered_map>

namespace insoulforge {
    /// @brief 提示词存储
    namespace PromptStore {
        [[nodiscard]] std::string getPrompt(const std::string &key, const std::string &defaultValue = "");

        void setPrompt(const std::string &key, const std::string &content, const std::string &description = "");

        [[nodiscard]] bool hasPrompt(const std::string &key);

        [[nodiscard]] std::unordered_map<std::string, std::string> getAllPrompts();
    } // namespace PromptStore
} // namespace insoulforge