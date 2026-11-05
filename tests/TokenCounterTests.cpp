#include <infrastructure/tokenizer/TokenCounter.hpp>
#include <future>
#include <iostream>
#include <stdexcept>
using namespace insoulforge;
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    auto counter = TokenCounter::load(argv[1]);
    if (!counter) { std::cerr << counter.error(); return 1; }
    auto check = [&](std::string_view text) {
        auto r = counter->estimate(text);
        if (!r || r->average != (static_cast<uint64_t>(r->counts[0]) + r->counts[1] + r->counts[2] + 2) / 3)
            throw std::runtime_error("invalid estimate");
        return r->average;
    };
    if (check("") != 0) return 1;
    for (auto text : {"Hello world", "你好，世界", "int main() { return 0; }", "你好 hello 😀"}) {
        auto future = std::async(std::launch::async, [&] { return check(text); });
        if (check(text) == 0 || check(text) != future.get()) return 1;
    }
    if (!check(std::string_view("a\0b", 3))) return 1;
    if (TokenCounter::load("/nonexistent/tokenizer-directory")) return 1;
}
