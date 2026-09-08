/// @file MessageRecord.cpp
/// @brief 聊天记录富内容的构造、兼容与投影实现

#include "conversation/message/MessageRecord.hpp"


#include <conversation/session/QQNameDirectory.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <ranges>
#include <regex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "conversation/message/SessionId.hpp"

namespace insoulforge::MessageRecord {
    namespace {
        /// @brief 向段数组追加文本，并合并相邻文本段以减小记录体积
        void appendText(json &segments, const std::string_view text) {
            if (text.empty()) {
                return;
            }
            if (!segments.empty() && getStr(segments.back(), "type") == "text") {
                segments.back()["text"] = getStr(segments.back(), "text") + std::string(text);
                return;
            }
            segments.push_back({{"type", "text"}, {"text", text}});
        }

        /// @brief 解析 CQ 参数列表；参数值中的逗号应由 CQ 编码转义，解析失败的片段直接忽略
        [[nodiscard]] std::unordered_map<std::string, std::string> parseCqParams(const std::string_view input) {
            std::unordered_map<std::string, std::string> params;
            for (size_t begin = 0; begin < input.size();) {
                const size_t end = input.find(',', begin);
                const std::string_view entry =
                  input.substr(begin, end == std::string_view::npos ? input.size() - begin : end - begin);
                if (const size_t separator = entry.find('='); separator != std::string_view::npos) {
                    params.emplace(std::string(entry.substr(0, separator)), std::string(entry.substr(separator + 1)));
                }
                if (end == std::string_view::npos) {
                    break;
                }
                begin = end + 1;
            }
            return params;
        }

        /// @brief 从新旧图片条目中提取模型可见的识别摘要
        [[nodiscard]] json imageSummary(const json &image, const size_t imageIndex) {
            json summary;
            summary["type"] = "image";
            summary["image_index"] = imageIndex;
            if (const std::string status = getStr(image, "recognition_status"); !status.empty()) {
                summary["recognition_status"] = status;
            }
            if (const std::string description = getStr(image, "description"); !description.empty()) {
                summary["description"] = description;
            }
            return summary;
        }

        /// @brief 将旧版或新版 @ 段投影为统一结构
        [[nodiscard]] json projectMention(const json &segment) {
            json mention;
            mention["type"] = "at";
            if (const json &target = atOrNull(segment, "target"); target.is_object()) {
                mention["target"] = target;
                return mention;
            }
            const std::string qq = getStr(segment, "qq");
            if (qq == "0") {
                mention["target"] = {{"kind", "all"}};
            } else {
                json target;
                target["qq"] = qq;
                if (const std::string name = getStr(segment, "name"); !name.empty() && name != "未知") {
                    target["name"] = name;
                }
                mention["target"] = std::move(target);
            }
            return mention;
        }

        /// @brief 读取新版记录的图片资产
        [[nodiscard]] const json &assetImages(const json &record) {
            const json &assets = atOrNull(record, "assets");
            const json &images = atOrNull(assets, "images");
            return images.is_array() ? images : atOrNull(record, "images");
        }

        /// @brief 将单个段投影为不含图片来源的 Agent 输入
        void projectSegment(json &result, const json &segment, const json &images, size_t &legacyImageIndex) {
            const std::string type = getStr(segment, "type");
            if (type == "text") {
                appendText(result, getStr(segment, "text"));
                return;
            }
            if (type == "at") {
                result.push_back(projectMention(segment));
                return;
            }
            if (type == "image") {
                const int explicitIndex = getInt(segment, "image_index", -1);
                const size_t imageIndex = explicitIndex >= 0 ? static_cast<size_t>(explicitIndex) : legacyImageIndex++;
                const json &image = imageIndex < images.size() ? images[imageIndex] : segment;
                result.push_back(imageSummary(image, imageIndex));
                return;
            }
            if (type == "face") {
                json face;
                face["type"] = "face";
                face["id"] = getStr(segment, "id");
                if (const std::string label = getStr(segment, "label"); !label.empty()) {
                    face["label"] = label;
                }
                result.push_back(std::move(face));
                return;
            }
            if (type == "poke") {
                json poke;
                poke["type"] = "poke";
                if (const json &target = atOrNull(segment, "target"); target.is_object()) {
                    poke["target"] = target;
                } else {
                    poke["target"] = {{"qq", getStr(segment, "target_id")}, {"name", getStr(segment, "target_name")}};
                }
                poke["direction"] = getStr(segment, "direction", "inbound");
                result.push_back(std::move(poke));
                return;
            }
            if (type == "notification") {
                json event;
                event["type"] = "member_event";
                std::string action = getStr(segment, "action");
                if (action.empty()) {
                    action = getStr(segment, "kind");
                    if (action.starts_with("member_")) {
                        action.erase(0, std::string_view("member_").size());
                    }
                }
                event["action"] = action;
                if (const std::string reason = getStr(segment, "reason", getStr(segment, "sub_type"));
                  !reason.empty()) {
                    event["reason"] = reason;
                }
                if (const uint64_t operatorId = getUInt(segment, "operator_id"); operatorId > 0) {
                    event["operator"] = {{"qq", std::to_string(operatorId)}};
                }
                result.push_back(std::move(event));
                return;
            }
            if (type == "member_event" || type == "sticker" || type == "unsupported") {
                result.push_back(segment);
            }
        }
    } // namespace

    uint64_t getSessionId(const json &record) { return getUInt(record, "session_id"); }

    json createAssistantRecord(std::string senderName, const uint64_t messageId, const std::string &content) {
        json segments = json::array();
        std::optional<std::string> replyTo;
        static const std::regex cqPattern(R"(\[CQ:([^,\]]+)(?:,([^\]]*))?\])");

        size_t cursor = 0;
        for (std::sregex_iterator it(content.begin(), content.end(), cqPattern), end; it != end; ++it) {
            const std::smatch &match = *it;
            const auto matchStart = static_cast<size_t>(match.position());
            appendText(segments, std::string_view(content).substr(cursor, matchStart - cursor));
            cursor = matchStart + static_cast<size_t>(match.length());

            const std::string type = match[1].str();
            const auto params = parseCqParams(match[2].str());
            const auto param = [&params](const std::string_view name) -> std::string {
                const auto it = params.find(std::string(name));
                return it != params.end() ? it->second : "";
            };
            if (type == "at") {
                const std::string qq = param("qq");
                if (qq == "all" || qq == "0") {
                    segments.push_back({{"type", "at"}, {"target", {{"kind", "all"}}}});
                } else {
                    json target{{"qq", qq}};
                    if (const std::string name = QQNameDirectory::getName(parseUInt64(qq)); name != "未知") {
                        target["name"] = name;
                    }
                    segments.push_back({{"type", "at"}, {"target", std::move(target)}});
                }
            } else if (type == "face") {
                segments.push_back({{"type", "face"}, {"id", param("id")}});
            } else if (type == "image" || type == "mface") {
                const std::string name = param("summary");
                if (type == "mface" || (param("sub_type") == "1" && !name.empty())) {
                    segments.push_back({{"type", "sticker"}, {"name", name.empty() ? "未命名表情" : name}});
                } else {
                    segments.push_back({{"type", "image"}});
                }
            } else if (type == "reply") {
                if (const std::string id = param("id"); !id.empty()) {
                    replyTo = id;
                }
            } else {
                segments.push_back({{"type", "unsupported"}, {"segment_type", type}});
            }
        }
        appendText(segments, std::string_view(content).substr(cursor));

        json record;
        record["time"] = currentDateTime();
        record["sender"] = {{"name", std::move(senderName)}, {"qq", "self"}};
        record["message_id"] = std::to_string(messageId);
        record["segments"] = std::move(segments);
        if (replyTo) {
            record["reply_to"] = std::move(*replyTo);
        }
        return record;
    }

    std::string extractText(const json &record) {
        std::string text;
        const json &segments = atOrNull(record, "segments");
        if (!segments.is_array()) {
            return getStr(record, "text");
        }
        for (const json &segment: segments) {
            if (getStr(segment, "type") == "text") {
                text += getStr(segment, "text");
            }
        }
        return text;
    }

    bool hasSegmentType(const json &record, const std::string_view type) {
        return std::ranges::any_of(
          atOrNull(record, "segments"), [type](const json &segment) { return getStr(segment, "type") == type; });
    }

    bool mentions(const json &record, const uint64_t qqNumber) {
        return std::ranges::any_of(atOrNull(record, "segments"), [qqNumber](const json &segment) {
            return getStr(segment, "type") == "at" &&
                   getUInt(atOrNull(segment, "target"), "qq", getUInt(segment, "qq")) == qqNumber;
        });
    }

    bool isAssistant(const json &record) { return getStr(atOrNull(record, "sender"), "qq") == "self"; }

    bool isSystem(const json &record) {
        return getStr(atOrNull(record, "sender"), "qq") == std::to_string(SessionId::kSystemAccountId);
    }

    json projectForAgent(const json &record) {
        if (!record.is_object()) {
            return record;
        }
        json projected;
        if (record.contains("time")) {
            projected["time"] = record["time"];
        }
        if (const json &sender = atOrNull(record, "sender"); sender.is_object()) {
            projected["sender"] = sender;
        }
        if (record.contains("message_id")) {
            projected["message_id"] = record["message_id"];
        }
        if (const json &replyTo = atOrNull(record, "reply_to"); !replyTo.is_null() && !replyTo.empty()) {
            projected["reply_to"] = replyTo;
        }

        json segments = json::array();
        const json &images = assetImages(record);
        size_t legacyImageIndex = 0;
        if (const json &sourceSegments = atOrNull(record, "segments"); sourceSegments.is_array()) {
            for (const auto &segment: sourceSegments) {
                projectSegment(segments, segment, images, legacyImageIndex);
            }
        } else if (const std::string text = getStr(record, "text"); !text.empty()) {
            appendText(segments, text);
        }

        if (segments.empty()) {
            for (const auto &face: atOrNull(record, "faces")) {
                projectSegment(segments,
                  json{{"type", "face"}, {"id", getStr(face, "id")}, {"label", getStr(face, "label")}}, images,
                  legacyImageIndex);
            }
            for (const auto &notification: atOrNull(record, "notifications")) {
                projectSegment(segments, notification, images, legacyImageIndex);
            }
        }
        projected["segments"] = std::move(segments);
        if (const json &sourceMemories = atOrNull(record, "memories"); sourceMemories.is_array()) {
            json memories = json::array();
            for (const json &memory: sourceMemories) {
                const std::string content = memory.is_string() ? jsonToString(memory) : getStr(memory, "content");
                if (!content.empty()) {
                    memories.push_back(content);
                }
            }
            if (!memories.empty()) {
                projected["memories"] = std::move(memories);
            }
        }
        return projected;
    }

    std::string extractRecallText(const json &record) {
        std::string text;
        const json projected = projectForAgent(record);
        for (const auto &segment: atOrNull(projected, "segments")) {
            const std::string type = getStr(segment, "type");
            if (type == "text") {
                text += getStr(segment, "text");
            } else if (type == "image" && getStr(segment, "recognition_status") == "succeeded") {
                const std::string description = getStr(segment, "description");
                if (!description.empty()) {
                    if (!text.empty()) {
                        text += '\n';
                    }
                    text += "图片：" + description;
                }
            }
        }
        return text;
    }

    std::optional<ImageSource> findImageSource(const json &record, const size_t imageIndex) {
        const json &images = assetImages(record);
        if (!images.is_array() || imageIndex >= images.size()) {
            return std::nullopt;
        }
        const json &image = images[imageIndex];
        const json &source = atOrNull(image, "source");
        ImageSource result{
          .file = getStr(source.is_object() ? source : image, "file"),
          .url = getStr(source.is_object() ? source : image, "url"),
        };
        return result.file.empty() && result.url.empty() ? std::nullopt : std::optional{std::move(result)};
    }
} // namespace insoulforge::MessageRecord
