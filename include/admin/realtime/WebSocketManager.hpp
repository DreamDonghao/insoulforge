/// @file WebSocketManager.hpp
/// @brief WebSocket 连接管理器
/// @author donghao
/// @date 2026-04-02
/// @details 管理 Web 管理后台的 WebSocket 连接：
///          - 连接管理：添加、移除连接
///          - 群订阅：支持按群订阅消息推送
///          - 消息推送：实时推送新消息

#pragma once
#include <drogon/WebSocketConnection.h>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge {
    /// @brief WebSocket连接管理器（单例模式）
    /// @details 管理所有WebSocket连接，支持按群订阅消息
    class WebSocketManager {
    public:
        /// @brief 获取单例实例
        /// @return WebSocketManager 实例引用
        static WebSocketManager &instance();

        /// @brief 添加连接
        /// @param conn WebSocket 连接指针
        void addConnection(const drogon::WebSocketConnectionPtr &conn);

        /// @brief 移除连接
        /// @param conn WebSocket 连接指针
        void removeConnection(const drogon::WebSocketConnectionPtr &conn);

        /// @brief 订阅特定群的消息
        /// @param conn WebSocket 连接指针
        /// @param sessionId 会话 ID（私聊会话带标志位）
        void subscribeSession(const drogon::WebSocketConnectionPtr &conn, uint64_t sessionId);

        /// @brief 取消订阅群
        /// @param conn WebSocket 连接指针
        /// @param sessionId 会话 ID（私聊会话带标志位）
        void unsubscribeSession(const drogon::WebSocketConnectionPtr &conn, uint64_t sessionId);

        /// @brief 推送新消息到订阅该群的连接
        /// @param sessionId 会话 ID（私聊会话带标志位）
        /// @param role 角色（user/assistant）
        /// @param content 消息内容
        void pushMessage(uint64_t sessionId, const std::string &role, const std::string &content);

        /// @brief 广播事件到所有连接
        /// @param type 事件类型
        /// @param data 事件数据
        void broadcastEvent(const std::string &type, const json &data) const;

    private:
        WebSocketManager() = default;

        std::unordered_set<drogon::WebSocketConnectionPtr> m_connections; ///< 所有连接
        std::unordered_map<uint64_t, std::unordered_set<drogon::WebSocketConnectionPtr>>
          m_subscriptions; ///< 群订阅映射
        mutable std::mutex m_mutex; ///< 线程安全锁
    };
} // namespace insoulforge
