/// @file AdminWebSocket.hpp
/// @brief 管理后台 WebSocket 控制器
/// @author donghao
/// @date 2026-04-02

#pragma once

#include <drogon/WebSocketController.h>
#include <admin/realtime/WebSocketManager.hpp>

namespace insoulforge {
    /// @brief 管理后台 WebSocket 控制器
    /// @details 处理 Web 管理界面的 WebSocket 连接，支持：
    ///          - 实时推送聊天记录
    ///          - 群订阅/取消订阅
    ///          - 连接状态管理
    class AdminWebSocket : public drogon::WebSocketController<AdminWebSocket> {
    public:
        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/admin/ws");

        WS_PATH_LIST_END

        /// @brief 新连接建立时的处理
        /// @param req HTTP 请求
        /// @param conn WebSocket 连接
        void handleNewConnection(
          const drogon::HttpRequestPtr &req, const drogon::WebSocketConnectionPtr &conn) override;

        /// @brief 收到 WebSocket 消息时的处理
        /// @param conn WebSocket 连接
        /// @param message 消息内容
        /// @param type 消息类型
        void handleNewMessage(const drogon::WebSocketConnectionPtr &conn, std::string &&message,
          const drogon::WebSocketMessageType &type) override;

        /// @brief 连接关闭时的处理
        /// @param conn WebSocket 连接
        void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override;
    };
} // namespace insoulforge
