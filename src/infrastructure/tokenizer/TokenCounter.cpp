#include <limits>
#include <memory>
#include <mutex>
#include <shared_mutex>

#include <llama.h>

#include <infrastructure/tokenizer/TokenCounter.hpp>

namespace insoulforge {
    namespace {
        struct FreeModel {
            void operator()(llama_model *model) const { llama_model_free(model); }
        };
        using Model = std::unique_ptr<llama_model, FreeModel>;

        struct State {
            std::shared_mutex mutex;
            bool initialized = false;
            Model model;
            std::string error;
        };

        auto state() -> State & {
            static State instance;
            return instance;
        }
    } // namespace

    auto initializeTokenCounter(const std::filesystem::path &directory) -> std::expected<void, std::string> {
        auto &[mutex, initialized, model, error] = state();
        std::unique_lock lock(mutex);
        if (!initialized) {
            // 仅本模块使用 llama；错误通过 expected 交给调用方记录。
            llama_log_set([](ggml_log_level, const char *, void *) noexcept -> void {}, nullptr);
            auto params = llama_model_default_params();
            params.vocab_only = true;
            params.n_gpu_layers = 0;
            const auto path = directory / "ggml-vocab-qwen2.gguf";
            model.reset(llama_model_load_from_file(path.string().c_str(), params));
            if (!model) {
                error = "无法加载词表: " + path.string();
            }
            initialized = true;
        }
        if (!model) {
            return std::unexpected(error);
        }
        return {};
    }

    auto countTokens(const std::string_view text) -> std::expected<u32, std::string> {
        auto &[mutex, initialized, model, error] = state();
        std::shared_lock lock(mutex);
        if (!initialized) {
            return std::unexpected("Token 计数器尚未初始化");
        }
        if (!model) {
            return std::unexpected(error);
        }
        if (text.size() > static_cast<size_t>(std::numeric_limits<i32>::max())) {
            return std::unexpected("文本长度超过分词器限制");
        }
        if (text.empty()) {
            return 0;
        }
        const auto count = llama_tokenize(llama_model_get_vocab(model.get()), text.data(),
          static_cast<i32>(text.size()), nullptr, 0, false, false);
        if (count == std::numeric_limits<i32>::min() || count > 0) {
            return std::unexpected("Qwen2 分词计数失败");
        }
        return static_cast<u32>(-count);
    }
} // namespace insoulforge
