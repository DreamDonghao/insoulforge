/// @file LongTermMemory.cpp
/// @brief 长期记忆服务 - 实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/memory/LongTermMemory.hpp>
#include <workflow/agent/memory/LongTermMemoryStore.hpp>

namespace insoulforge {
    auto LongTermMemory::searchMemory(std::string query, const i32 topK, const SessionId sessionId)
      -> drogon::Task<std::expected<std::optional<std::string>, std::string>> {
        const auto embedding = co_await LlmClient::requestEmbedding(std::move(query), sessionId);
        if (!embedding) {
            co_return std::unexpected(embedding.error());
        }

        const auto rows = LongTermMemoryStore::searchSimilar(sessionId, *embedding, topK);
        std::string result;
        for (const auto &memory: rows) {
            if (memory.similarity < 0.3f)
                continue;
            result += memory.content + "\n";
        }

        if (result.empty()) {
            co_return std::nullopt;
        }
        co_return std::move(result);
    }
} // namespace insoulforge
