/// @file MessageContentEnricher.cpp
/// @brief 统一消息的媒体与长期记忆富化实现


#include <algorithm>
#include <array>
#include <format>
#include <string_view>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/image/Image.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/memory/LongTermMemoryStore.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/image/ImageDescriptionService.hpp>

namespace insoulforge::MessageContentEnricher {
    namespace {
        constexpr i32 kRecallTopK = 3;
        constexpr size_t kMinRecallChars = 3;

        /// @brief 计算 UTF-8 代码点数量
        [[nodiscard]] auto utf8Length(const std::string &text) -> size_t {
            return static_cast<size_t>(std::ranges::count_if(text,
              [](const char character) -> bool { return (static_cast<unsigned char>(character) & 0xC0U) != 0x80U; }));
        }

        auto littleEndian(std::string_view bytes, const size_t offset, const size_t count) -> u32 {
            u32 value = 0;
            for (size_t index = 0; index < count; ++index) {
                value |= static_cast<u32>(static_cast<u8>(bytes[offset + index])) << (8 * index);
            }
            return value;
        }

        auto isSupportedWav(std::string_view wav) -> bool {
            if (wav.size() < 12 || wav.substr(0, 4) != "RIFF" || wav.substr(8, 4) != "WAVE" ||
                littleEndian(wav, 4, 4) != wav.size() - 8) {
                return false;
            }
            u32 byteRate = 0;
            u32 blockSize = 0;
            size_t audioBytes = 0;
            for (size_t offset = 12; offset < wav.size();) {
                if (wav.size() - offset < 8) {
                    return false;
                }
                const auto type = wav.substr(offset, 4);
                const auto size = static_cast<size_t>(littleEndian(wav, offset + 4, 4));
                offset += 8;
                if (size > wav.size() - offset || size % 2 > wav.size() - offset - size) {
                    return false;
                }
                if (type == "fmt ") {
                    if (size < 16 || littleEndian(wav, offset, 2) != 1 || littleEndian(wav, offset + 14, 2) != 16) {
                        return false;
                    }
                    const auto channels = littleEndian(wav, offset + 2, 2);
                    const auto rate = littleEndian(wav, offset + 4, 4);
                    constexpr std::array<u32, 7> kSampleRates{8000, 16000, 22050, 24000, 32000, 44100, 48000};
                    blockSize = channels * 2;
                    byteRate = rate * blockSize;
                    if ((channels != 1 && channels != 2) ||
                        std::ranges::find(kSampleRates, rate) == kSampleRates.end() ||
                        littleEndian(wav, offset + 8, 4) != byteRate ||
                        littleEndian(wav, offset + 12, 2) != blockSize) {
                        return false;
                    }
                } else if (type == "data") {
                    audioBytes += size;
                }
                offset += size + (size % 2);
            }
            return blockSize > 0 && audioBytes > 0 && audioBytes % blockSize == 0 && audioBytes <= byteRate * 120U;
        }
    } // namespace

    auto enrichImages(json message, const SessionId sessionId) -> drogon::Task<json> {
        if (!message.is_object()) {
            co_return message;
        }
        const auto assets = message.find("assets");
        if (assets == message.end() || !assets->is_object()) {
            co_return message;
        }
        const auto images = assets->find("images");
        if (images == assets->end() || !images->is_array()) {
            co_return message;
        }

        size_t succeededCount = 0;
        for (json &image: *images) {
            // 活动附件已经在接入处验证；保留资产 ID，按需由 ask_images 读取。
            if (!getStr(image, "image_id").empty() && getStr(image, "recognition_status") == "available") {
                continue;
            }
            const std::string sourceUrl = getStr(atOrNull(image, "source"), "url");
            if (sourceUrl.empty()) {
                image["recognition_status"] = "failed";
                continue;
            }
            const auto cached = co_await Image::load(sourceUrl);
            if (!cached) {
                image["recognition_status"] = "failed";
                continue;
            }
            image["image_id"] = cached->id();
            const auto description = co_await ImageDescriptionService::describe(cached->id(), sessionId);
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
              sessionId, "Media", std::format("图片识别完成 | succeeded={}/{}", succeededCount, images->size()));
        }
        co_return message;
    }

    auto transcribeAudio(json message, const SessionId sessionId) -> drogon::Task<json> {
        if (!MessageRecord::hasSegmentType(message, "audio")) {
            co_return message;
        }
        const auto config = Config::instance().getLLMConfig("asr");
        const auto apiKey = trim(getStr(config, "apiKey"));
        const auto baseUrl = trim(getStr(config, "baseUrl"));
        const auto path = trim(getStr(config, "path"));
        const auto timeout = getInt(config, "timeoutSeconds", 90);
        for (auto &segment: message["segments"]) {
            if (getStr(segment, "type") != "audio") {
                continue;
            }
            auto file = getStr(segment, "onebot_file");
            segment.erase("onebot_file");
            segment["recognition_status"] = "skipped";
            if (apiKey.empty()) {
                continue;
            }
            segment["recognition_status"] = "failed";
            if (file.empty() || baseUrl.empty() || path.empty()) {
                continue;
            }
            auto wav = co_await OneBotClient::getRecordWav(std::move(file), sessionId);
            if (!wav || !isSupportedWav(*wav)) {
                Logger::warn(sessionId, "ASR", "未取得符合要求的 WAV 语音");
                continue;
            }
            const auto response = co_await HttpUtil::uploadWav(baseUrl, path, std::move(*wav), apiKey, timeout);
            json result;
            if (!response || response->getStatusCode() != drogon::k200OK || !tryParseJson(response->body(), result)) {
                Logger::warn(sessionId, "ASR", "语音识别请求失败");
                continue;
            }
            const auto transcript = atOrNull(result, "text");
            if (transcript.is_string()) {
                if (auto content = trim(transcript.get_ref<const std::string &>()); !content.empty()) {
                    segment["transcript"] = std::move(content);
                    segment["recognition_status"] = "succeeded";
                    continue;
                }
            }
            Logger::warn(sessionId, "ASR", "语音识别未返回文字");
        }
        co_return message;
    }

    auto injectMemories(json message, const SessionId sessionId) -> drogon::Task<json> {
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
            Logger::info(sessionId, "Memory", std::format("召回长期记忆: {} 条", memories.size()));
            message["memories"] = std::move(memories);
        }
        co_return message;
    }
} // namespace insoulforge::MessageContentEnricher
