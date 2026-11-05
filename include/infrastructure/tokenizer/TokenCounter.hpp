/// @file TokenCounter.hpp
/// @brief 使用 Qwen2 词表在本地统计文本 Token，无需模型权重或网络。
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <expected>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 加载共享 Qwen2 词表；首次初始化的路径与结果（包括错误）缓存至进程退出。
    /// @details directory 显式指定词表目录。应在启动阶段调用，后续计数复用只读词表。
    [[nodiscard]] auto initializeTokenCounter(const std::filesystem::path &directory)
      -> std::expected<void, std::string>;

    /// @brief 返回 Qwen2 词表的文本 Token 数，支持并发和内嵌 NUL，空文本返回 0。
    /// @details 未初始化或初始化失败时返回错误，不隐式加载；不添加 BOS/EOS、不解析特殊 Token，
    ///          不包含聊天模板、图片或工具协议开销，不保证与其他模型的计数一致。
    [[nodiscard]] auto countTokens(std::string_view text) -> std::expected<u32, std::string>;
} // namespace insoulforge
