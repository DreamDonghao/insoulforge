/// @file LogWebSocket.hpp
/// @brief 运行日志 WebSocket 控制器

#pragma once

#include <drogon/WebSocketController.h>

namespace insoulforge {
    /// @brief 接收后台日志订阅条件并维护 WebSocket 连接
    /// @details 客户端连接后发送 subscribe 消息，选择会话、等级和关键词过滤条件。
    class LogWebSocket : public drogon::WebSocketController<LogWebSocket> {
    public:
        WS_PATH_LIST_BEGIN
        WS_PATH_ADD("/admin/logs/ws");

        WS_PATH_LIST_END

        /// @brief 注册连接并发送已连接确认
        void handleNewConnection(
          const drogon::HttpRequestPtr &req, const drogon::WebSocketConnectionPtr &conn) override;

        /// @brief 处理文本格式的订阅请求；其他消息类型忽略
        void handleNewMessage(const drogon::WebSocketConnectionPtr &conn, std::string &&message,
          const drogon::WebSocketMessageType &type) override;

        /// @brief 移除连接及其订阅条件
        void handleConnectionClosed(const drogon::WebSocketConnectionPtr &conn) override;
    };
} // namespace insoulforge
