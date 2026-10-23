/// @file ImageReferences.cpp
/// @brief 从冻结快照或当前会话数据库解析图片来源
#include <limits>
#include <set>

#include <infrastructure/CommonUtil.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/image/ImageReferences.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/MessageRecord.hpp>

namespace insoulforge::ImageReferences {
    auto schema() -> json {
        return {{"type", "array"},
          {"description", "当前会话中的图片引用，按数组顺序读取，重复引用只保留一次"},
          {"items", {{"type", "object"},
            {"properties", {
              {"message_id", {{"type", "string"}, {"description", "原图所在消息的 message_id"}}},
              {"image_index", {{"type", "integer"}, {"minimum", 0},
                {"description", "原图在消息中的索引，从 0 开始"}}}}},
            {"required", {"message_id", "image_index"}}}}};
    }

    auto resolve(const json &images, const ToolCallContext &ctx)
      -> std::expected<std::vector<std::string>, std::string> {
        if (!images.is_array()) return std::unexpected("图片引用必须是数组。");
        std::vector<std::string> urls;
        std::set<std::pair<u64, size_t>> seen;
        for (const auto &image : images) {
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
                if (parseUInt64(getStr(message, "message_id")) == messageId) {
                    record = message;
                    break;
                }
            }
            if (record.is_null()) {
                const auto content = ChatRecordStore::findContentByMessageId(ctx.sessionId, messageId);
                if (!content || !tryParseJson(*content, record))
                    return std::unexpected("未找到该会话中的图片消息。");
            }
            const auto source = MessageRecord::findImageSource(record, imageIndex);
            if (!source || source->url.empty())
                return std::unexpected("该消息没有可下载的原图，请让用户重新发送图片。");
            urls.push_back(source->url);
        }
        return urls;
    }
}
