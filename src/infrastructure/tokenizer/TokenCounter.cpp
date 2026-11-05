#include <algorithm>
#include <limits>
#include <mutex>

#include <llama.h>

#include <infrastructure/tokenizer/TokenCounter.hpp>

namespace insoulforge {
    namespace {
        auto sharedCounter() -> const std::expected<TokenCounter, std::string> & {
            static const auto counter = TokenCounter::load("resources/tokenizers");
            return counter;
        }
    } // namespace

    auto TokenCounter::initialize() -> std::expected<void, std::string> {
        if (const auto &counter = sharedCounter(); !counter) {
            return std::unexpected(counter.error());
        }
        return {};
    }

    auto estimateTokens(const std::string_view text) -> std::expected<TokenEstimate, std::string> {
        const auto &counter = sharedCounter();
        if (!counter) {
            return std::unexpected(counter.error());
        }
        return counter->estimate(text);
    }

    struct TokenCounter::State {
        struct Free {
            void operator()(llama_model *model) const { llama_model_free(model); }
        };
        std::array<std::unique_ptr<llama_model, Free>, 3> models;
    };

    auto TokenCounter::load(const std::filesystem::path &directory) -> std::expected<TokenCounter, std::string> {
        // 当前仅本模块使用 llama；错误通过 expected 交由调用方统一记录。
        static std::once_flag logging;
        std::call_once(logging,
          [] -> void { llama_log_set([](ggml_log_level, const char *, void *) noexcept -> void {}, nullptr); });
        auto state = std::make_shared<State>();
        std::string errors;
        auto params = llama_model_default_params();
        params.vocab_only = true;
        params.n_gpu_layers = 0;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto path = directory / ("ggml-vocab-" + std::string(names[i]) + ".gguf");
            state->models[i].reset(llama_model_load_from_file(path.string().c_str(), params));
            if (!state->models[i]) {
                if (!errors.empty())
                    errors += '\n';
                errors += "无法加载词表: " + path.string();
            }
        }
        if (!errors.empty())
            return std::unexpected(std::move(errors));
        return TokenCounter(std::move(state));
    }

    auto TokenCounter::estimate(const std::string_view text) const -> std::expected<TokenEstimate, std::string> {
        if (!m_state)
            return std::unexpected("TokenCounter 已被移动");
        if (text.size() > static_cast<std::size_t>(std::numeric_limits<int32_t>::max()))
            return std::unexpected("文本长度超过分词器限制");
        TokenEstimate result;
        if (text.empty())
            return result;
        std::uint64_t sum = 0;
        for (std::size_t i = 0; i < names.size(); ++i) {
            const auto count = llama_tokenize(llama_model_get_vocab(m_state->models[i].get()), text.data(),
              static_cast<int32_t>(text.size()), nullptr, 0, false, false);
            if (count == std::numeric_limits<int32_t>::min() || count > 0)
                return std::unexpected("分词计数失败: " + std::string(names[i]));
            result.counts[i] = static_cast<std::uint32_t>(-count);
            sum += result.counts[i];
        }
        result.average = static_cast<std::uint32_t>((sum + 2) / 3);
        result.maximum = std::ranges::max(result.counts);
        result.minimum = std::ranges::min(result.counts);
        return result;
    }

    auto TokenCounter::average(const std::string_view text) const -> std::expected<std::uint32_t, std::string> {
        auto result = estimate(text);
        if (!result)
            return std::unexpected(result.error());
        return result->average;
    }

    auto TokenCounter::maximum(const std::string_view text) const -> std::expected<std::uint32_t, std::string> {
        auto result = estimate(text);
        if (!result)
            return std::unexpected(result.error());
        return result->maximum;
    }

    auto TokenCounter::minimum(const std::string_view text) const -> std::expected<std::uint32_t, std::string> {
        auto result = estimate(text);
        if (!result)
            return std::unexpected(result.error());
        return result->minimum;
    }
} // namespace insoulforge
