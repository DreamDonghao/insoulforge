/// @file LogBufferTests.cpp
/// @brief Verify log eviction, ordering, filtering and pagination.

#include <iostream>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

#include <infrastructure/logging/LogBuffer.hpp>
#include <infrastructure/logging/LogRedaction.hpp>

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

        const std::string secret = "fixture-admin-secret";
        const std::string loginUrl = "http://127.0.0.1:7778/index.html#token=" + secret;
        require(redactAdminLogCredentials("ordinary token count: 42") == "ordinary token count: 42",
          "ordinary logs must remain unchanged");
        require(redactAdminLogCredentials("管理后台访问令牌（重启后失效）: " + secret).find(secret) == std::string::npos,
          "the startup token must be hidden");
        const auto links = redactAdminLogCredentials("\"" + loginUrl + "\" " + loginUrl + "&view=logs");
        require(redactAdminLogCredentials(links) == links, "redacting an already redacted log must be stable");
        require(links.find(secret) == std::string::npos && links.find("&view=logs") != std::string::npos &&
                  links.find("http://127.0.0.1:7778/index.html#token=[已隐藏]") != std::string::npos,
          "all login links must hide only their token values");
        const auto published = buffer.append({.level = "info", .source = "Admin", .content = loginUrl});
        require(published.content.find(secret) == std::string::npos,
          "the append result used by WebSocket pushes must already be redacted");
        require(buffer.query({.keyword = secret}).entries.empty(),
          "the HTTP log query must not expose or match a token");

        const auto directory = std::filesystem::temp_directory_path() /
          ("insoulforge-log-privacy-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); }
        } cleanup{directory};
        {
            std::ofstream current(directory / "bot.log");
            current << "[2026-10-31 12:00:00.000] [info][0][Admin][" << loginUrl << "]\n";
            current << "[2026-10-31 12:00:01.000] [info][0][Main][ordinary history]\n";
            std::ofstream rotated(directory / "bot.1.log");
            rotated << "[2026-10-31 11:00:00.000] [info][0][Admin][管理后台访问令牌（重启后失效）: " << secret << "]\n";
            rotated << "[2026-10-31 11:00:01.000] [info] 管理后台访问令牌（重启后失效）: " << secret << "\n";
        }
        buffer.loadFromDirectory(directory.string());
        const auto history = buffer.query({});
        require(history.entries.size() == 4 && history.entries.back().content == "ordinary history",
          "redaction must preserve structured and legacy history, ordering and ordinary messages");
        for (const auto &entry: history.entries) {
            require(entry.content.find(secret) == std::string::npos,
              "restored current and rotated history must not expose previous startup credentials");
        }
        require(buffer.query({.keyword = secret}).entries.empty(), "restored tokens must not be searchable");
        std::cout << "Log buffer tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
