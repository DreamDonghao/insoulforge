/// @file ImageReferences.cpp
/// @brief 从冻结快照或当前会话数据库解析图片来源
#include <limits>
#include <set>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/image/Image.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/image/ImageReferences.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/MessageRecord.hpp>

namespace insoulforge::ImageReferences {
    auto schema() -> json {
        const json object = {{"type", "object"},
          {"properties", {{"image_id", {{"type", "string"}, {"description", "当前可见会话中的图片 ID，优先使用"}}},
                           {"message_id", {{"type", "string"}, {"description", "旧消息 ID"}}},
                           {"image_index", {{"type", "integer"}, {"minimum", 0}}}}},
          {"anyOf",
            json::array({json{{"required", {"image_id"}}}, json{{"required", {"message_id", "image_index"}}}})}};
        return {{"type", "array"}, {"description", "按顺序引用当前会话图片，可传图片 ID 或引用对象"},
          {"items", {{"anyOf", json::array({json{{"type", "string"}}, object})}}}};
    }

    auto resolve(const json &images, const ToolCallContext &ctx)
      -> std::expected<std::vector<std::string>, std::string> {
        if (!images.is_array()) return std::unexpected("图片引用必须是数组。");
        std::vector<std::string> urls;
        std::set<std::pair<u64, size_t>> seen;
        std::set<std::string> seenIds;
        for (const auto &image : images) {
            const auto imageId = image.is_string() ? image.get<std::string>() : getStr(image, "image_id");
            if (image.is_string() || image.contains("image_id")) {
                bool visible = false;
                for (const auto &record: ctx.messageSnapshot) {
                    if (const auto session = getStr(record, "session_id");
                      !session.empty() && SessionId::parse(session) != ctx.sessionId)
                        continue;
                    const auto &assets = atOrNull(atOrNull(record, "assets"), "images");
                    if (assets.is_array())
                        for (const auto &asset: assets)
                            if (!imageId.empty() && getStr(asset, "image_id") == imageId)
                                visible = true;
                    const auto &segments = atOrNull(record, "segments");
                    if (segments.is_array())
                        for (const auto &segment: segments)
                            if (getStr(segment, "type") == "image" && !imageId.empty() &&
                                getStr(segment, "image_id") == imageId)
                                visible = true;
                }
                if (!visible)
                    return std::unexpected("图片 ID 不在当前可见会话中。");
                if (const auto cached = Image::find(imageId); !cached)
                    return std::unexpected(cached.error());
                if (seenIds.insert(imageId).second)
                    urls.push_back(imageId);
                continue;
            }
            const auto &id = atOrNull(image, "message_id");
            const auto &index = atOrNull(image, "image_index");
            const u64 messageId = id.is_string() ? parseUInt64(id.get<std::string>()) : 0;
            if (messageId == 0 || !index.is_number_integer() || index < 0 ||
                index > std::numeric_limits<i32>::max()) {
                return std::unexpected("请提供有效的图片消息 ID 和从 0 开始的整数图片索引。");
            }
            const auto imageIndex = index.get<size_t>();
            if (!seen.emplace(messageId, imageIndex).second) continue;
            json record;
            for (const auto &message : ctx.messageSnapshot) {
                if (const auto session = getStr(message, "session_id");
                  !session.empty() && SessionId::parse(session) != ctx.sessionId)
                    continue;
                if (parseUInt64(getStr(message, "message_id")) == messageId) {
                    record = message;
                    break;
                }
            }
            if (record.is_null()) {
                const auto content = ChatRecordStore::findContentByMessageId(ctx.requireSession(), messageId);
                if (!content || !tryParseJson(*content, record))
                    return std::unexpected("未找到该会话中的图片消息。");
            }
            const auto source = MessageRecord::findImageSource(record, imageIndex);
            if (!source || (source->imageId.empty() && source->url.empty()))
                return std::unexpected("该消息没有可下载的原图，请让用户重新发送图片。");
            if (!source->imageId.empty() && Image::find(source->imageId)) {
                if (seenIds.insert(source->imageId).second)
                    urls.push_back(source->imageId);
            } else if (!source->url.empty())
                urls.push_back(source->url);
            else
                return std::unexpected("图片缓存已过期，请重新发送图片。");
        }
        return urls;
    }
}
