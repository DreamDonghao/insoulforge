/// @file OneBotClient.cpp
/// @brief OneBot API 客户端实现

#include <infrastructure/NumericTypes.hpp>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/OneBotWebSocketClient.hpp>

namespace insoulforge::OneBotClient {
    namespace {
        /// @brief 通用 API 调用
        /// @param tag 日志前缀，如 "[Msg]"、"[Sticker]"
        /// @param api API 名称，如 "send_group_msg"（内部补 "/" 前缀）
        /// @param params 请求参数（JSON body）
        /// @param sessionId 会话 ID（用于会话日志，可空）
        /// @param timeout 超时秒数
        /// @return 响应 JSON（含 status/retcode/data）；请求失败或 status != ok 时返回 nullopt（已记日志）
        [[nodiscard]] auto callApi(std::string_view tag, std::string api, json params,
          std::optional<u64> sessionId = std::nullopt, f64 timeout = 30.0) -> drogon::Task<std::optional<json>> {
            json body;
            const auto &config = Config::instance();
            if (config.oneBotTransport == "websocket") {
                const auto response =
                  co_await OneBotWebSocketClient::instance().callApi(api, std::move(params), timeout);
                if (!response) {
                    co_return std::nullopt;
                }
                body = std::move(*response);
            } else {
                const auto response = co_await HttpUtil::send(tag, config.qqHttpHost, "/" + api, drogon::Post,
                  std::move(params), config.accessToken, timeout, sessionId);
                if (!response) {
                    co_return std::nullopt;
                }
                if ((*response)->getStatusCode() != drogon::k200OK || !tryParseJson((*response)->body(), body)) {
                    Logger::error(sessionId.value_or(0), "OneBot",
                      fmt::format("{} API {} 请求失败: http_status={}", tag, api,
                        static_cast<i32>((*response)->getStatusCode())));
                    co_return std::nullopt;
                }
            }
            if (!body.is_object()) {
                Logger::error(sessionId.value_or(0), "OneBot", fmt::format("{} API {} 响应不是 JSON 对象", tag, api));
                co_return std::nullopt;
            }
            if (getStr(body, "status", "failed") != "ok") {
                Logger::error(sessionId.value_or(0), "OneBot",
                  fmt::format("{} API {} 失败: status={}, retcode={}", tag, api, getStr(body, "status"),
                    getInt(body, "retcode", -1)));
                co_return std::nullopt;
            }
            co_return body;
        }

        /// @brief 发送消息的公共实现（群聊/私聊共用，仅 API 名与目标字段不同）
        auto sendMessage(std::string api, std::string targetKey, const u64 targetId, std::string message,
          const std::optional<u64> sessionId) -> drogon::Task<std::optional<u64>> {
            json params;
            params[targetKey] = targetId;
            params["message"] = message;
            params["auto_escape"] = false;

            const auto resp = co_await callApi("[Msg]", std::move(api), std::move(params), sessionId);
            if (!resp) {
                Logger::error(sessionId.value_or(0), "OneBot",
                  fmt::format("发送消息错误: msgLen={}, preview={}", message.size(), message.substr(0, 200)));
                co_return std::nullopt;
            }
            co_return jsonToUInt64(atOrNull(atOrNull(*resp, "data"), "message_id"));
        }
    } // namespace

    auto sendGroupMsg(const u64 groupId, std::string message) -> drogon::Task<std::optional<u64>> {
        co_return co_await sendMessage("send_group_msg", "group_id", groupId, std::move(message), groupId);
    }

    auto sendPrivateMsg(const u64 userId, std::string message, const std::optional<u64> sessionId)
      -> drogon::Task<std::optional<u64>> {
        co_return co_await sendMessage("send_private_msg", "user_id", userId, std::move(message), sessionId);
    }

    auto setGroupBan(const u64 groupId, const u64 userId, const u64 duration) -> drogon::Task<bool> {
        json params;
        params["group_id"] = groupId;
        params["user_id"] = userId;
        params["duration"] = duration;

        const auto resp = co_await callApi("[Ban]", "set_group_ban", params, groupId);
        if (!resp) {
            co_return false;
        }
        Logger::info(groupId, "OneBot", fmt::format("禁言成功: 用户{} 时长{}秒", userId, duration));
        co_return true;
    }

    auto getGroupInfo(const u64 groupId) -> drogon::Task<json> {
        json params;
        params["group_id"] = groupId;

        const auto resp = co_await callApi("[GroupInfo]", "get_group_info", params, groupId);
        co_return resp.value_or(json{});
    }

    auto getStrangerInfo(const u64 userId, const std::optional<u64> sessionId) -> drogon::Task<json> {
        json params;
        params["user_id"] = userId;

        const auto resp = co_await callApi("[StrangerInfo]", "get_stranger_info", params, sessionId);
        co_return resp.value_or(json{});
    }

    auto sendPoke(const u64 groupId, const u64 userId) -> drogon::Task<bool> {
        json params;
        params["group_id"] = groupId;
        params["user_id"] = userId;

        const auto resp = co_await callApi("[Poke]", "send_poke", params, groupId);
        if (!resp) {
            co_return false;
        }
        Logger::info(groupId, "OneBot", fmt::format("拍一拍成功: 用户{}", userId));
        co_return true;
    }

    auto deleteMsg(const u64 messageId, const std::optional<u64> sessionId) -> drogon::Task<bool> {
        json params;
        params["message_id"] = messageId;

        const auto resp = co_await callApi("[Recall]", "delete_msg", params, sessionId);
        if (!resp) {
            co_return false;
        }
        Logger::info(sessionId.value_or(0), "OneBot", fmt::format("撤回消息成功: message_id={}", messageId));
        co_return true;
    }

    auto getImage(std::string file, const std::optional<u64> sessionId) -> drogon::Task<std::optional<std::string>> {
        json params;
        params["file"] = std::move(file);

        const auto resp = co_await callApi("[Sticker]", "get_image", params, sessionId, 15.0);
        if (!resp) {
            co_return std::nullopt;
        }
        co_return jsonToString(atOrNull(atOrNull(*resp, "data"), "file"));
    }

    auto downloadFile(std::string url, const std::optional<u64> sessionId) -> drogon::Task<std::optional<std::string>> {
        json params;
        params["url"] = std::move(url);

        const auto resp = co_await callApi("[Sticker]", "download_file", params, sessionId);
        if (!resp) {
            co_return std::nullopt;
        }
        co_return jsonToString(atOrNull(atOrNull(*resp, "data"), "file"));
    }

    auto addCustomFace(std::string file, const std::optional<u64> sessionId) -> drogon::Task<bool> {
        json params;
        params["file"] = file;

        const auto resp = co_await callApi("[Sticker]", "add_custom_face", params, sessionId);
        if (!resp) {
            Logger::error(0, "Sticker", fmt::format("保存收藏表情失败: {}", file));
            co_return false;
        }
        co_return true;
    }

    auto setCustomFaceDesc(std::string emojiId, std::string resId, std::string md5, std::string desc,
      const std::optional<u64> sessionId) -> drogon::Task<bool> {
        json params;
        params["emoji_id"] = std::move(emojiId);
        params["res_id"] = std::move(resId);
        params["md5"] = std::move(md5);
        params["desc"] = std::move(desc);

        const auto resp = co_await callApi("[Sticker]", "set_custom_face_desc", params, sessionId);
        co_return resp.has_value();
    }

    auto deleteCustomFace(std::string resId, const std::optional<u64> sessionId) -> drogon::Task<bool> {
        json params;
        params["res_id"] = std::move(resId);

        const auto resp = co_await callApi("[Sticker]", "delete_custom_face", params, sessionId);
        co_return resp.has_value();
    }

    auto fetchCustomFaceDetail(const std::optional<u64> sessionId) -> drogon::Task<json> {
        json params;
        params["count"] = 200;

        const auto resp = co_await callApi("[Sticker]", "fetch_custom_face_detail", params, sessionId);
        if (!resp) {
            co_return json::array();
        }
        co_return atOrNull(*resp, "data");
    }
} // namespace insoulforge::OneBotClient
