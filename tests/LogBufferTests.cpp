/// @file LogBufferTests.cpp
/// @brief Verify log eviction, ordering, filtering and pagination.

#include <iostream>
#include <stdexcept>
#include <string>

#include <infrastructure/logging/LogBuffer.hpp>

namespace {
    void require(const bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }
} // namespace

auto main() -> int {
    using namespace insoulforge;
    try {
        auto &buffer = LogBuffer::instance();
        u64 firstId = 0;
        for (i32 index = 0; index < 5010; ++index) {
            const auto entry = buffer.append({
              .level = index % 2 == 0 ? "info" : "error",
              .sessionId = static_cast<u64>(index % 2),
              .source = "fixture",
              .content = std::to_string(index),
            });
            if (index == 0) {
                firstId = entry.id;
            }
            require(entry.id == firstId + static_cast<u64>(index), "IDs must remain increasing");
        }
        require(buffer.size() == 5000, "buffer must retain exactly 5000 entries");
        const auto latest = buffer.query({.limit = 3});
        require(latest.oldestId == firstId + 10 && latest.newestId == firstId + 5009,
          "only the oldest entries must be evicted");
        require(latest.entries.size() == 3 && latest.entries.front().content == "5007" &&
                  latest.entries.back().content == "5009",
          "latest page must remain chronological");
        const auto older = buffer.query({.beforeId = latest.nextBeforeId, .limit = 3});
        require(older.entries.size() == 3 && older.entries.front().content == "5004" &&
                  older.entries.back().content == "5006",
          "backward pagination must exclude the cursor");
        const auto newer = buffer.query({.afterId = older.nextAfterId, .limit = 3});
        require(newer.nextBeforeId == latest.nextBeforeId && newer.nextAfterId == latest.nextAfterId,
          "forward pagination must recover the latest page");
        const auto filtered = buffer.query({
          .sessionId = 1,
          .level = "error",
          .keyword = "fixture",
          .limit = 3,
        });
        require(filtered.entries.size() == 3 && filtered.entries.front().content == "5005" &&
                  filtered.entries.back().content == "5009",
          "filters must retain chronological order");
        std::cout << "Log buffer tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
