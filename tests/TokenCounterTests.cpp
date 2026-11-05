#include <iostream>
#include <memory>
#include <stdexcept>

#include <future>
#include <llama.h>

#include <infrastructure/tokenizer/TokenCounter.hpp>
using namespace insoulforge;
int main(int argc, char **argv) {
    if (argc != 2) {
        return 1;
    }
    if (countTokens("未初始化")) {
        return 1;
    }
    const auto ready = initializeTokenCounter(argv[1]);
    if (!ready) {
        std::cerr << ready.error();
        return 1;
    }
    if (!initializeTokenCounter(argv[1])) {
        return 1;
    }
    auto params = llama_model_default_params();
    params.vocab_only = true;
    const auto path = std::filesystem::path(argv[1]) / "ggml-vocab-qwen2.gguf";
    std::unique_ptr<llama_model, decltype(&llama_model_free)> model(
      llama_model_load_from_file(path.string().c_str(), params), llama_model_free);
    if (!model) {
        return 1;
    }
    auto check = [&](std::string_view text) {
        auto r = countTokens(text);
        const auto count = text.empty() ? 0
                                        : -llama_tokenize(llama_model_get_vocab(model.get()), text.data(),
                                            static_cast<i32>(text.size()), nullptr, 0, false, false);
        if (!r || *r != count) {
            throw std::runtime_error("tokenizer result mismatch");
        }
        return *r;
    };
    if (check("") != 0) {
        return 1;
    }
    for (auto text: {"Hello world", "你好，世界", "int main() { return 0; }", "你好 hello 😀"}) {
        auto future = std::async(std::launch::async, [&] { return check(text); });
        if (check(text) == 0 || check(text) != future.get()) {
            return 1;
        }
    }
    if (!check(std::string_view("a\0b", 3))) {
        return 1;
    }
}
