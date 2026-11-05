/// @file TokenCounter.hpp
/// @brief 使用本地 GGUF 词表估算文本 Token 数量，无需模型权重或网络。
#pragma once
#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include <expected>

namespace insoulforge {
    /// @brief 三个代表词表的计数及汇总；不保证与目标模型的实际用量一致。
    /// @details 仅统计文本，不包含聊天模板、图片或工具协议等额外开销。
    struct TokenEstimate {
        /// @brief 各词表的计数，顺序与 TokenCounter::names 一致。
        std::array<std::uint32_t, 3> counts{};
        /// @brief 三个计数的算术平均值，向上取整，用于跨模型粗略估算。
        std::uint32_t average{};
        /// @brief 三个计数中的最大值，不代表目标模型实际计数的上界。
        std::uint32_t maximum{};
        /// @brief 三个计数中的最小值，不代表目标模型实际计数的下界。
        std::uint32_t minimum{};
    };

    /// @brief 可复用的本地文本计数器，加载后支持并发调用 estimate()。
    /// @details 仅加载词表，不创建推理上下文；复制实例共享只读词表资源。
    class TokenCounter {
    public:
        /// @brief 初始化全局计数器，加载 resources/tokenizers 下的三个词表。
        /// @return 全部成功返回空值，否则汇总加载失败的路径；结果缓存至进程退出。
        /// @note 内部关闭 llama 日志，由调用方在失败时记录返回的错误。
        [[nodiscard]] static auto initialize() -> std::expected<void, std::string>;

        /// @brief 从指定目录加载全部三个词表；成功后可重复计数，无需重新读文件。
        /// @param directory 包含 ggml-vocab-<name>.gguf 的目录，name 取自 names。
        /// @return 成功返回计数器；尝试加载全部词表后汇总失败路径。
        static auto load(const std::filesystem::path &directory) -> std::expected<TokenCounter, std::string>;

        /// @brief 分别计算文本的 Token 数并汇总，不自动添加 BOS/EOS 或解析特殊 Token。
        /// @param text 待分词的 UTF-8 文本，允许内嵌 NUL；空文本返回全零。
        /// @return 计数结果；实例已被移动、文本过长或计数溢出时返回错误信息。
        [[nodiscard]] auto estimate(std::string_view text) const -> std::expected<TokenEstimate, std::string>;

        /// @brief 计算向上取整的平均值；错误语义与 estimate() 相同。
        [[nodiscard]] auto average(std::string_view text) const -> std::expected<std::uint32_t, std::string>;
        /// @brief 计算三个词表计数的最大值；错误语义与 estimate() 相同。
        [[nodiscard]] auto maximum(std::string_view text) const -> std::expected<std::uint32_t, std::string>;
        /// @brief 计算三个词表计数的最小值；错误语义与 estimate() 相同。
        /// @note 每个便捷接口都会重新分词；需要多个统计值时应调用一次 estimate()。
        [[nodiscard]] auto minimum(std::string_view text) const -> std::expected<std::uint32_t, std::string>;

        /// @brief 词表文件名中的标识，同时规定 TokenEstimate::counts 的顺序。
        static constexpr std::array<std::string_view, 3> names{"llama-bpe", "qwen2", "gemma-4"};

    private:
        struct State;
        explicit TokenCounter(std::shared_ptr<const State> state) : m_state(std::move(state)) {}
        std::shared_ptr<const State> m_state;
    };

    /// @brief 使用全局共享词表估算文本，返回语义与 TokenCounter::estimate() 相同。
    /// @details 首次调用从工作目录下 resources/tokenizers 加载，初始化和计数均支持并发。
    /// 加载结果（包括错误）缓存至进程退出；修复资源后需重启，或自行调用 TokenCounter::load()。
    [[nodiscard]] auto estimateTokens(std::string_view text) -> std::expected<TokenEstimate, std::string>;
} // namespace insoulforge
