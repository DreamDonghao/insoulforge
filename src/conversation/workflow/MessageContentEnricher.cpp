/// @file MessageContentEnricher.cpp
/// @brief 统一消息的媒体与长期记忆富化实现


#include <fmt/format.h>

#include <agent/memory/LongTermMemoryStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>
#include <media/ImageDescriptionService.hpp>

namespace insoulforge::MessageContentEnricher {
    namespace {
        constexpr i32 kRecallTopK = 3;
        constexpr size_t kMinRecallChars = 3;

        /// @brief 计算 UTF-8 代码点数量
        [[nodiscard]] auto utf8Length(const std::string &text) -> size_t {
            return static_cast<size_t>(std::ranges::count_if(text,
              [](const char character) -> bool { return (static_cast<unsigned char>(character) & 0xC0U) != 0x80U; }));
        }
    } // namespace

    auto enrichImages(json message, const u64 sessionId) -> drogon::Task<json> {
        if (!message.is_object()) {
            co_return message;
        }
        const auto assets = message.find("assets");
        if (assets == message.end() || !assets->is_object())
            co_return message;
        const auto images = assets->find("images");
        if (images == assets->end() || !images->is_array())
            co_return message;

        size_t succeededCount = 0;
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
            ++succeededCount;
        }
        if (!images->empty()) {
            Logger::info(
              sessionId, "Media", fmt::format("图片识别完成 | succeeded={}/{}", succeededCount, images->size()));
        }
        co_return message;
    }

    auto injectMemories(json message, const u64 sessionId) -> drogon::Task<json> {
        const std::string query = MessageRecord::extractRecallText(message);
        if (utf8Length(query) <= kMinRecallChars) {
            co_return message;
        }

        const auto embedding = co_await LlmClient::requestEmbedding(query, sessionId);
        if (!embedding) {
            co_return message;
        }

        const f32 threshold = static_cast<f32>(Config::instance().longTermInjectThreshold);
        json memories = json::array();
        for (const auto &[id, content, similarity]:
          LongTermMemoryStore::searchSimilar(sessionId, *embedding, kRecallTopK)) {
            if (similarity >= threshold) {
                memories.push_back({{"id", id}, {"content", content}, {"similarity", similarity}});
            }
        }
        if (!memories.empty()) {
            Logger::info(sessionId, "Memory", fmt::format("召回长期记忆: {} 条", memories.size()));
            message["memories"] = std::move(memories);
        }
        co_return message;
    }
} // namespace insoulforge::MessageContentEnricher
