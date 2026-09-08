/// @file OneBotClient.cpp
/// @brief OneBot HTTP API 客户端 - 实现

#include <infrastructure/config/Config.hpp>
#include <onebot/OneBotClient.hpp>
#include <spdlog/spdlog.h>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge::OneBotClient {
    namespace {
        /// @brief 通用 API 调用
        /// @param tag 日志前缀，如 "[Msg]"、"[Sticker]"
        /// @param api API 名称，如 "send_group_msg"（内部补 "/" 前缀）
        /// @param params 请求参数（JSON body）
        /// @param sessionId 会话 ID（用于会话日志，可空）
        /// @param timeout 超时秒数
        /// @return 响应 JSON（含 status/retcode/data）；HTTP 非 200 或 status != ok 时返回 nullopt（已记日志）
        [[nodiscard]] drogon::Task<std::optional<json>> callApi(std::string_view tag, std::string api, json params,
          std::optional<uint64_t> sessionId = std::nullopt, double timeout = 30.0) {
            const auto &config = Config::instance();
            const auto resp = co_await HttpUtil::send(tag, config.qqHttpHost, "/" + api, drogon::Post,
              std::move(params), config.accessToken, timeout, sessionId);
            if (!resp) {
                co_return std::nullopt;
            }
            json body;
            if ((*resp)->getStatusCode() != drogon::k200OK || !tryParseJson((*resp)->body(), body)) {
                Logger::session(sessionId.value_or(0))
                  .error(
                    "{} OneBot API {} 请求失败: http_status={}", tag, api, static_cast<int>((*resp)->getStatusCode()));
                co_return std::nullopt;
            }
            if (getStr(body, "status", "failed") != "ok") {
                Logger::session(sessionId.value_or(0))
                  .error("{} OneBot API {} 失败: status={}, retcode={}", tag, api, getStr(body, "status"),
                    getInt(body, "retcode", -1));
                co_return std::nullopt;
            }
            co_return body;
        }

        /// @brief 发送消息的公共实现（群聊/私聊共用，仅 API 名与目标字段不同）
        drogon::Task<std::optional<uint64_t>> sendMessage(std::string api, std::string targetKey,
          const uint64_t targetId, std::string message, const std::optional<uint64_t> sessionId) {
            json params;
            params[targetKey] = targetId;
            params["message"] = message;
            params["auto_escape"] = false;

            const auto resp = co_await callApi("[Msg]", std::move(api), std::move(params), sessionId);
            if (!resp) {
                Logger::session(sessionId.value_or(0))
                  .error("发送消息错误: msgLen={}, preview={}", message.size(), message.substr(0, 200));
                co_return std::nullopt;
            }
            co_return jsonToUInt64(atOrNull(atOrNull(*resp, "data"), "message_id"));
        }
    } // namespace

    drogon::Task<std::optional<uint64_t>> sendGroupMsg(const uint64_t groupId, std::string message) {
        co_return co_await sendMessage("send_group_msg", "group_id", groupId, std::move(message), groupId);
    }

    drogon::Task<std::optional<uint64_t>> sendPrivateMsg(
      const uint64_t userId, std::string message, const std::optional<uint64_t> sessionId) {
        co_return co_await sendMessage("send_private_msg", "user_id", userId, std::move(message), sessionId);
    }

    drogon::Task<bool> setGroupBan(const uint64_t groupId, const uint64_t userId, const uint64_t duration) {
        json params;
        params["group_id"] = groupId;
        params["user_id"] = userId;
        params["duration"] = duration;

        const auto resp = co_await callApi("[Ban]", "set_group_ban", params, groupId);
        if (!resp) {
            co_return false;
        }
        Logger::session(groupId).info("禁言成功: 用户{} 时长{}秒", userId, duration);
        co_return true;
    }

    drogon::Task<json> getGroupInfo(const uint64_t groupId) {
        json params;
        params["group_id"] = groupId;

        const auto resp = co_await callApi("[GroupInfo]", "get_group_info", params, groupId);
        co_return resp.value_or(json{});
    }

    drogon::Task<json> getStrangerInfo(const uint64_t userId, const std::optional<uint64_t> sessionId) {
        json params;
        params["user_id"] = userId;

        const auto resp = co_await callApi("[StrangerInfo]", "get_stranger_info", params, sessionId);
        co_return resp.value_or(json{});
    }

    drogon::Task<bool> sendPoke(const uint64_t groupId, const uint64_t userId) {
        json params;
        params["group_id"] = groupId;
        params["user_id"] = userId;

        const auto resp = co_await callApi("[Poke]", "send_poke", params, groupId);
        if (!resp) {
            co_return false;
        }
        Logger::session(groupId).info("拍一拍成功: 用户{}", userId);
        co_return true;
    }

    drogon::Task<bool> deleteMsg(const uint64_t messageId, const std::optional<uint64_t> sessionId) {
        json params;
        params["message_id"] = messageId;

        const auto resp = co_await callApi("[Recall]", "delete_msg", params, sessionId);
        if (!resp) {
            co_return false;
        }
        Logger::session(sessionId.value_or(0)).info("撤回消息成功: message_id={}", messageId);
        co_return true;
    }

    drogon::Task<std::optional<std::string>> getImage(std::string file, const std::optional<uint64_t> sessionId) {
        json params;
        params["file"] = std::move(file);

        const auto resp = co_await callApi("[Sticker]", "get_image", params, sessionId, 15.0);
        if (!resp) {
            co_return std::nullopt;
        }
        co_return jsonToString(atOrNull(atOrNull(*resp, "data"), "file"));
    }

    drogon::Task<std::optional<std::string>> downloadFile(std::string url, const std::optional<uint64_t> sessionId) {
        json params;
        params["url"] = std::move(url);

        const auto resp = co_await callApi("[Sticker]", "download_file", params, sessionId);
        if (!resp) {
            co_return std::nullopt;
        }
        co_return jsonToString(atOrNull(atOrNull(*resp, "data"), "file"));
    }

    drogon::Task<bool> addCustomFace(std::string file, const std::optional<uint64_t> sessionId) {
        json params;
        params["file"] = file;

        const auto resp = co_await callApi("[Sticker]", "add_custom_face", params, sessionId);
        if (!resp) {
            spdlog::error("[Sticker] 保存收藏表情失败: {}", file);
            co_return false;
        }
        co_return true;
    }

    drogon::Task<bool> setCustomFaceDesc(std::string emojiId, std::string resId, std::string md5, std::string desc,
      const std::optional<uint64_t> sessionId) {
        json params;
        params["emoji_id"] = std::move(emojiId);
        params["res_id"] = std::move(resId);
        params["md5"] = std::move(md5);
        params["desc"] = std::move(desc);

        const auto resp = co_await callApi("[Sticker]", "set_custom_face_desc", params, sessionId);
        co_return resp.has_value();
    }

    drogon::Task<bool> deleteCustomFace(std::string resId, const std::optional<uint64_t> sessionId) {
        json params;
        params["res_id"] = std::move(resId);

        const auto resp = co_await callApi("[Sticker]", "delete_custom_face", params, sessionId);
        co_return resp.has_value();
    }

    drogon::Task<json> fetchCustomFaceDetail(const std::optional<uint64_t> sessionId) {
        json params;
        params["count"] = 200;

        const auto resp = co_await callApi("[Sticker]", "fetch_custom_face_detail", params, sessionId);
        if (!resp) {
            co_return json::array();
        }
        co_return atOrNull(*resp, "data");
    }
} // namespace insoulforge::OneBotClient
