/// @file MessageContentEnricher.cpp
/// @brief 统一消息的媒体与长期记忆富化实现


#include <agent/memory/LongTermMemoryStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <infrastructure/config/Config.hpp>
#include <llm/LlmClient.hpp>
#include <media/ImageDescriptionService.hpp>

namespace insoulforge::MessageContentEnricher {
    namespace {
        constexpr int kRecallTopK = 3;
        constexpr size_t kMinRecallChars = 3;

        /// @brief 计算 UTF-8 代码点数量
        [[nodiscard]] size_t utf8Length(const std::string &text) {
            return static_cast<size_t>(std::ranges::count_if(
              text, [](const char character) { return (static_cast<unsigned char>(character) & 0xC0U) != 0x80U; }));
        }
    } // namespace

    drogon::Task<json> enrichImages(json message, const uint64_t sessionId) {
        if (!message.is_object()) {
            co_return message;
        }
        const auto assets = message.find("assets");
        if (assets == message.end() || !assets->is_object())
            co_return message;
        const auto images = assets->find("images");
        if (images == assets->end() || !images->is_array())
            co_return message;

        for (json &image: *images) {
            const std::string sourceUrl = getStr(atOrNull(image, "source"), "url");
            if (sourceUrl.empty()) {
                image["recognition_status"] = "failed";
                continue;
            }
            const auto description = co_await ImageDescriptionService::describe(sourceUrl, sessionId);
            if (!description) {
                image["recognition_status"] = "failed";
                continue;
            }
            image["recognition_status"] = "succeeded";
            image["description"] = description->description;
            image["content_hash"] = description->contentHash;
            image["media"] = {
              {"type", description->mediaType}, {"sampled_frame_count", description->sampledFrameCount}};
        }
        co_return message;
    }

    drogon::Task<json> injectMemories(json message, const uint64_t sessionId) {
        const std::string query = MessageRecord::extractRecallText(message);
        if (utf8Length(query) <= kMinRecallChars) {
            co_return message;
        }

        const auto embedding = co_await LlmClient::requestEmbedding(query, sessionId);
        if (!embedding) {
            co_return message;
        }

        const float threshold = static_cast<float>(Config::instance().longTermInjectThreshold);
        json memories = json::array();
        for (const auto &[id, content, similarity]:
          LongTermMemoryStore::searchSimilar(sessionId, *embedding, kRecallTopK)) {
            if (similarity >= threshold) {
                memories.push_back({{"id", id}, {"content", content}, {"similarity", similarity}});
            }
        }
        if (!memories.empty()) {
            message["memories"] = std::move(memories);
        }
        co_return message;
    }
} // namespace insoulforge::MessageContentEnricher
