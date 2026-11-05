#include <algorithm>
#include <iostream>
#include <stdexcept>

#include <future>
#include <llama.h>

#include <infrastructure/tokenizer/TokenCounter.hpp>
using namespace insoulforge;
int main(int argc, char **argv) {
    if (argc != 2)
        return 1;
    auto counter = TokenCounter::load(argv[1]);
    if (!counter) {
        std::cerr << counter.error();
        return 1;
    }
    auto params = llama_model_default_params();
    params.vocab_only = true;
    std::array<llama_model *, 3> models{};
    struct Cleanup {
        std::array<llama_model *, 3> &models;
        ~Cleanup() {
            for (auto *model: models)
                llama_model_free(model);
        }
    } cleanup{models};
    for (std::size_t i = 0; i < models.size(); ++i) {
        const auto path =
          std::filesystem::path(argv[1]) / ("ggml-vocab-" + std::string(TokenCounter::names[i]) + ".gguf");
        models[i] = llama_model_load_from_file(path.string().c_str(), params);
        if (!models[i])
            return 1;
    }
    auto check = [&](std::string_view text) {
        auto r = counter->estimate(text);
        if (!r || r->average != (static_cast<uint64_t>(r->counts[0]) + r->counts[1] + r->counts[2] + 2) / 3)
            throw std::runtime_error("invalid estimate");
        if (r->minimum != std::ranges::min(r->counts) || r->maximum != std::ranges::max(r->counts))
            throw std::runtime_error("invalid extrema");
        for (std::size_t i = 0; i < models.size(); ++i) {
            const auto count = text.empty() ? 0
                                            : -llama_tokenize(llama_model_get_vocab(models[i]), text.data(),
                                                static_cast<int32_t>(text.size()), nullptr, 0, false, false);
            if (r->counts[i] != count)
                throw std::runtime_error("tokenizer result mismatch");
        }
        return r->average;
    };
    if (check("") != 0)
        return 1;
    for (auto text: {"Hello world", "你好，世界", "int main() { return 0; }", "你好 hello 😀"}) {
        auto future = std::async(std::launch::async, [&] { return check(text); });
        if (check(text) == 0 || check(text) != future.get())
            return 1;
    }
    if (!check(std::string_view("a\0b", 3)))
        return 1;
    if (TokenCounter::load("/nonexistent/tokenizer-directory"))
        return 1;
}
