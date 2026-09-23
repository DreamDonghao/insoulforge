/// @file OneBotEventNormalizer.cpp
/// @brief OneBot 上报事件到统一消息记录的转换实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/message/SessionId.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/workflow/OneBotEventNormalizer.hpp>

namespace insoulforge::OneBotEventNormalizer {
    namespace {
        std::atomic nextSyntheticMessageId{9'100'000'000LL};

        /// @brief 为缺少消息 ID 的通知事件生成进程内唯一 ID
        [[nodiscard]] i64 makeSyntheticMessageId() { return nextSyntheticMessageId.fetch_add(1); }

        /// @brief 将 OneBot Unix 时间戳转换为统一记录的本地时间字符串
        [[nodiscard]] std::string normalizeTime(const json &body) {
            const i64 timestamp = getInt64(body, "time");
            return timestamp > 0 ? formatUnixTime(timestamp) : currentDateTime();
        }

        /// @brief 读取昵称，缺失时使用统一占位值
        [[nodiscard]] std::string displayName(const json &source) {
            const std::string name = getStr(source, "nickname", getStr(source, "name"));
            return name.empty() ? "未知" : name;
        }

        /// @brief 从 @ 段生成统一目标对象
        [[nodiscard]] json normalizeAtTarget(const json &data) {
            const std::string qq = jsonToString(atOrNull(data, "qq"));
            if (parseUInt64(qq) == 0) {
                return {{"kind", "all"}};
            }

            json target{{"qq", qq}};
            if (const std::string name = getStr(data, "name"); !name.empty()) {
                target["name"] = name;
            }
            return target;
        }

        /// @brief 追加一个普通 OneBot 消息段，并返回其引用回复 ID（若存在）
        [[nodiscard]] std::optional<std::string> appendMessageSegment(
          const json &source, json &segments, json &images, const u64 selfId) {
            const std::string type = getStr(source, "type");
            const json &data = atOrNull(source, "data");
            if (type == "text") {
                segments.push_back({{"type", "text"}, {"text", getStr(data, "text")}});
            } else if (type == "at") {
                segments.push_back({{"type", "at"}, {"target", normalizeAtTarget(data)}});
            } else if (type == "face") {
                json face{{"type", "face"}, {"id", getStr(data, "id")}};
                if (const std::string label = getStr(atOrNull(data, "raw"), "faceText", getStr(data, "label"));
                  !label.empty()) {
                    face["label"] = label;
                }
                segments.push_back(std::move(face));
            } else if (type == "image") {
                const size_t imageIndex = images.size();
                images.push_back({{"source", {{"file", getStr(data, "file")}, {"url", getStr(data, "url")}}}});
                segments.push_back({{"type", "image"}, {"image_index", imageIndex}});
            } else if (type == "poke") {
                const u64 actorId = getUInt(data, "actor_id");
                json target{{"qq", jsonToString(atOrNull(data, "target_id"))}};
                if (const std::string name = getStr(data, "target_name"); !name.empty()) {
                    target["name"] = name;
                }
                segments.push_back({{"type", "poke"}, {"target", std::move(target)},
                  {"direction", actorId == selfId ? "outbound" : "inbound"}});
            } else if (type == "notification") {
                std::string action = getStr(data, "kind");
                if (action.starts_with("member_")) {
                    action.erase(0, std::string_view("member_").size());
                }
                json event{{"type", "member_event"}, {"action", std::move(action)}};
                if (const std::string reason = getStr(data, "sub_type"); !reason.empty()) {
                    event["reason"] = reason;
                }
                if (const std::string operatorId = jsonToString(atOrNull(data, "operator_id")); !operatorId.empty()) {
                    event["operator"] = {{"qq", operatorId}};
                }
                segments.push_back(std::move(event));
            } else if (type == "reply") {
                if (const std::string replyId = jsonToString(atOrNull(data, "id")); !replyId.empty()) {
                    return replyId;
                }
            } else if (!type.empty()) {
                segments.push_back({{"type", "unsupported"}, {"segment_type", type}});
            }
            return std::nullopt;
        }

        /// @brief 初始化统一记录的公共元数据字段
        [[nodiscard]] json createRecord(
          const json &body, const u64 senderId, const std::string &senderName, const i64 messageId) {
            return {{"time", normalizeTime(body)}, {"sender", {{"name", senderName}, {"qq", std::to_string(senderId)}}},
              {"message_id", std::to_string(messageId)}, {"segments", json::array()}};
        }

        /// @brief 从原始上报提取统一会话 ID
        /// @details 群聊以群号为 ID；私聊以用户 QQ 号加私聊标志位，避免与群号冲突。
        [[nodiscard]] u64 extractSessionId(const json &body, const u64 senderId) {
            if (const u64 groupId = getUInt(body, "group_id"); groupId != 0) {
                return groupId;
            }
            if (getStr(body, "post_type") == "message" && getStr(body, "message_type") != "private") {
                return 0;
            }
            const u64 userId = getUInt(body, "user_id", senderId);
            return userId == 0 ? 0 : SessionId::fromPrivateUser(userId);
        }

        /// @brief 归一化普通消息上报
        [[nodiscard]] std::optional<json> normalizeMessage(const json &body) {
            const u64 senderId = getUInt(atOrNull(body, "sender"), "user_id", getUInt(body, "user_id"));
            const i64 messageId = getInt64(body, "message_id");
            if (senderId == 0 || messageId == 0) {
                return std::nullopt;
            }

            const std::string senderName = displayName(atOrNull(body, "sender"));
            QQNameDirectory::recordName(senderId, senderName);
            json record = createRecord(body, senderId, senderName, messageId);
            json images = json::array();
            std::optional<std::string> replyTo;
            const u64 selfId = getUInt(body, "self_id");
            if (senderId == selfId && selfId != 0) {
                record["sender"]["qq"] = "self";
            }
            for (const json &segment: atOrNull(body, "message")) {
                if (const auto replyId = appendMessageSegment(segment, record["segments"], images, selfId)) {
                    replyTo = *replyId;
                }
            }
            if (!images.empty()) {
                record["assets"] = {{"images", std::move(images)}};
            }
            if (replyTo) {
                record["reply_to"] = std::move(*replyTo);
            }
            return record;
        }

        /// @brief 归一化拍一拍通知
        [[nodiscard]] std::optional<json> normalizePokeNotice(const json &body) {
            const u64 actorId = getUInt(body, "user_id");
            const u64 targetId = getUInt(body, "target_id");
            if (actorId == 0 || targetId == 0) {
                return std::nullopt;
            }

            const std::string senderName = displayName(atOrNull(body, "sender"));
            QQNameDirectory::recordName(actorId, senderName);
            json record = createRecord(body, actorId, senderName, makeSyntheticMessageId());
            if (actorId == getUInt(body, "self_id")) {
                record["sender"]["qq"] = "self";
            }
            json target{{"qq", std::to_string(targetId)}};
            record["segments"].push_back({{"type", "poke"}, {"target", std::move(target)},
              {"direction", actorId == getUInt(body, "self_id") ? "outbound" : "inbound"}});
            return record;
        }

        /// @brief 归一化群成员变动通知
        [[nodiscard]] std::optional<json> normalizeMembershipNotice(const json &body) {
            const u64 memberId = getUInt(body, "user_id");
            if (memberId == 0 || getUInt(body, "group_id") == 0) {
                return std::nullopt;
            }

            const std::string noticeType = getStr(body, "notice_type");
            const std::string senderName = displayName(atOrNull(body, "sender"));
            QQNameDirectory::recordName(memberId, senderName);
            json record = createRecord(body, memberId, senderName, makeSyntheticMessageId());
            json event{{"type", "member_event"}, {"action", noticeType == "group_increase" ? "join" : "leave"}};
            if (const std::string reason = getStr(body, "sub_type"); !reason.empty()) {
                event["reason"] = reason;
            }
            if (const std::string operatorId = jsonToString(atOrNull(body, "operator_id")); !operatorId.empty()) {
                event["operator"] = {{"qq", operatorId}};
            }
            record["segments"].push_back(std::move(event));
            return record;
        }
    } // namespace

    std::optional<json> normalize(json body) {
        std::optional<json> record;
        if (getStr(body, "post_type") == "message") {
            record = normalizeMessage(body);
        } else if (getStr(body, "notice_type") == "notify" && getStr(body, "sub_type") == "poke") {
            record = normalizePokeNotice(body);
        } else {
            const std::string noticeType = getStr(body, "notice_type");
            if (noticeType == "group_increase" || noticeType == "group_decrease") {
                record = normalizeMembershipNotice(body);
            }
        }
        if (!record) {
            return std::nullopt;
        }

        const u64 senderId = getUInt(atOrNull(body, "sender"), "user_id", getUInt(body, "user_id"));
        const u64 sessionId = extractSessionId(body, senderId);
        if (sessionId == 0) {
            return std::nullopt;
        }
        (*record)["session_id"] = sessionId;
        return record;
    }
} // namespace insoulforge::OneBotEventNormalizer
