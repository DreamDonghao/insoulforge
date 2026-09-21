/// @file LongTermMemory.cpp
/// @brief 长期记忆服务 - 实现
/// @author donghao
/// @date 2026-09-01

#include <agent/memory/LongTermMemory.hpp>

#include <agent/memory/LongTermMemoryStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>

namespace insoulforge {
    drogon::Task<std::optional<std::string>> LongTermMemory::searchMemory(
      std::string query, const int topK, const uint64_t sessionId) {
        const auto embedding = co_await LlmClient::requestEmbedding(std::move(query), sessionId);
        if (!embedding) {
            Logger::warn(sessionId, "Memory", "记忆检索向量化失败（Embedding 未配置或请求失败）");
            co_return std::nullopt;
        }

        const auto rows = LongTermMemoryStore::searchSimilar(sessionId, *embedding, topK);
        std::string result;
        for (const auto &memory: rows) {
            if (memory.similarity < 0.3f)
                continue;
            result += memory.content + "\n";
        }

        if (result.empty())
            co_return std::optional<std::string>("未找到相关信息");
        co_return result;
    }
} // namespace insoulforge
