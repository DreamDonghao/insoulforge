/// @file SharedConversationContext.cpp
/// @brief 跨会话近期见闻的投影与组装。

#include <algorithm>

#include <ctime>

#include <infrastructure/CommonUtil.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/history/SharedConversationContext.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge::SharedConversationContext {
    namespace {
        // 同一消息的所有可读字段共用额度，类型、账号等结构性字段不参与截断。
        void limitText(json &value, size_t &remaining, bool &truncated) {
            if (value.is_string()) {
                auto &text = value.get_ref<std::string &>();
                size_t end = 0;
                while (end < text.size() && remaining > 0) {
                    ++end;
                    while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
                        ++end;
                    }
                    --remaining;
                }
                if (end < text.size()) {
                    text.resize(end);
                    truncated = true;
                }
            } else if (value.is_object()) {
                for (auto &[key, child]: value.items()) {
                    if (key != "type" && key != "id" && key != "qq" && key != "recognition_status" && key != "kind") {
                        limitText(child, remaining, truncated);
                    }
                }
            } else if (value.is_array()) {
                for (auto &child: value) {
                    limitText(child, remaining, truncated);
                }
            }
        }
    } // namespace

    auto projectMessage(const json &record) -> std::optional<json> {
        if (!record.is_object() || getStr(record, "record_type") == "tool_execution" || record.contains("send_error") ||
            MessageRecord::isSystem(record)) {
            return std::nullopt;
        }
        const auto projected = MessageRecord::projectForAgent(record, false, false);
        json result = {
          {"time", getStr(projected, "time")}, {"sender", atOrNull(projected, "sender")}, {"segments", json::array()}};
        size_t remaining = 200;
        bool truncated = false;
        for (auto segment: atOrNull(projected, "segments")) {
            if (remaining == 0) {
                truncated = true;
                break;
            }
            segment.erase("image_index");
            limitText(segment, remaining, truncated);
            result["segments"].push_back(std::move(segment));
        }
        if (result["segments"].empty()) {
            return std::nullopt;
        }
        if (truncated) {
            result["truncated"] = true;
        }
        return result;
    }

    auto build(const u64 receiverId, const i32 messagesPerSession) -> json {
        json result = json::array();
        const auto now = std::time(nullptr);
        const auto earliest = formatUnixTime(now - 3 * 24 * 60 * 60);
        const auto latest = formatUnixTime(now);
        for (const auto &[sessionId, name]: SessionStore::getSharedSessions(receiverId)) {
            auto messages = OneBotEventWorkflow::instance().getSharedSessionMessages(
              sessionId, static_cast<size_t>(std::max(messagesPerSession, 1)), earliest, latest);
            if (messages.empty()) {
                continue;
            }
            json session;
            if (SessionId::isPrivate(sessionId)) {
                session = {
                  {"type", "private"}, {"qq", std::to_string(SessionId::privateUserId(sessionId))}, {"name", name}};
            } else {
                session = {{"type", "group"}, {"group_id", sessionId}, {"group_name", name}};
            }
            result.push_back({{"session", std::move(session)}, {"messages", std::move(messages)}});
        }
        return result;
    }
} // namespace insoulforge::SharedConversationContext
