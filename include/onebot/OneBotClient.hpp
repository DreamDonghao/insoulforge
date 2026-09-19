/// @file OneBotClient.hpp
/// @brief OneBot API 客户端 - 统一封装对 QQ 客户端(NapCat)的调用
/// @date 2026-08-31
/// @details 所有与 QQ 客户端的交互统一走这里：
///          - callApi() 通用调用：按配置使用 HTTP 或 WebSocket、携带 accessToken、校验 status=ok、记录失败日志
///          - 按操作封装的类型化方法（发消息、禁言、戳一拍、表情管理等）

#pragma once
#include <drogon/utils/coroutine.h>
#include <optional>
#include <string>

#include <infrastructure/JsonUtil.hpp>

/// @brief OneBot API 客户端
/// @details 根据 OneBot 配置，通过 HTTP 或 WebSocket 与 OneBot 实现通信。
namespace insoulforge::OneBotClient {
    /// @brief 发送群消息（群号即会话 ID，无需单独传 sessionId）
    /// @note OneBot 11: send_group_msg
    /// @return message_id；失败返回 nullopt
    [[nodiscard]] drogon::Task<std::optional<uint64_t>> sendGroupMsg(uint64_t groupId, std::string message);

    /// @brief 发送私聊消息
    /// @note OneBot 11: send_private_msg
    /// @return message_id；失败返回 nullopt
    [[nodiscard]] drogon::Task<std::optional<uint64_t>> sendPrivateMsg(
      uint64_t userId, std::string message, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 群组单人禁言
    /// @note OneBot 11: set_group_ban（duration=0 解除禁言）
    [[nodiscard]] drogon::Task<bool> setGroupBan(uint64_t groupId, uint64_t userId, uint64_t duration);

    /// @brief 获取群信息
    /// @note OneBot 11: get_group_info
    /// @return 响应 JSON（data 含 group_name 等）；失败返回 null
    [[nodiscard]] drogon::Task<json> getGroupInfo(uint64_t groupId);

    /// @brief 获取陌生人（QQ 用户）信息
    /// @note OneBot 11: get_stranger_info
    /// @return 响应 JSON（data 含 nickname 等）；失败返回 null
    [[nodiscard]] drogon::Task<json> getStrangerInfo(uint64_t userId, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 群聊戳一拍
    /// @note NapCat: send_poke（支持群聊/私聊）
    [[nodiscard]] drogon::Task<bool> sendPoke(uint64_t groupId, uint64_t userId);

    /// @brief 撤回消息
    /// @note OneBot 11: delete_msg
    [[nodiscard]] drogon::Task<bool> deleteMsg(uint64_t messageId, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 获取图片在 QQ 客户端容器内的路径
    /// @note OneBot 11: get_image
    /// @return data.file 路径；失败返回 nullopt（15 秒超时，商城表情等会失败）
    [[nodiscard]] drogon::Task<std::optional<std::string>> getImage(
      std::string file, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 下载文件到 QQ 客户端缓存目录
    /// @note go-cqhttp: download_file
    /// @return data.file 路径；失败返回 nullopt
    [[nodiscard]] drogon::Task<std::optional<std::string>> downloadFile(
      std::string url, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 将图片保存为 QQ 收藏表情
    /// @note NapCat 扩展: add_custom_face（api.md 未收录）
    [[nodiscard]] drogon::Task<bool> addCustomFace(std::string file, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 设置收藏表情描述
    /// @note NapCat 扩展: set_custom_face_desc（api.md 未收录）
    [[nodiscard]] drogon::Task<bool> setCustomFaceDesc(std::string emojiId, std::string resId, std::string md5,
      std::string desc, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 从收藏表情中删除
    /// @note NapCat 扩展: delete_custom_face（api.md 未收录）
    [[nodiscard]] drogon::Task<bool> deleteCustomFace(
      std::string resId, std::optional<uint64_t> sessionId = std::nullopt);

    /// @brief 获取收藏表情详情列表
    /// @note NapCat 扩展（与 api.md 收录的 NapCat fetch_custom_face 同族）
    /// @return data 数组；失败返回空数组
    [[nodiscard]] drogon::Task<json> fetchCustomFaceDetail(std::optional<uint64_t> sessionId = std::nullopt);
} // namespace insoulforge::OneBotClient
