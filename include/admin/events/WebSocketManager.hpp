/// @file WebSocketManager.hpp
/// @brief 管理后台消息推送连接
/// @details 管理后台连接可按会话订阅聊天消息；全局事件向所有连接广播。

#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <drogon/WebSocketConnection.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 管理后台 WebSocket 连接与会话订阅
    class WebSocketManager {
    public:
        /// @brief 获取单例实例
        /// @return WebSocketManager 实例引用
        static auto instance() -> WebSocketManager &;

        /// @brief 添加连接
        /// @param conn WebSocket 连接指针
        void addConnection(const drogon::WebSocketConnectionPtr &conn);

        /// @brief 移除连接
        /// @param conn WebSocket 连接指针
        void removeConnection(const drogon::WebSocketConnectionPtr &conn);

        /// @brief 订阅指定会话的消息
        /// @param conn WebSocket 连接指针
        /// @param sessionId 会话 ID（私聊会话带标志位）
        void subscribeSession(const drogon::WebSocketConnectionPtr &conn, u64 sessionId);

        /// @brief 取消订阅指定会话
        /// @param conn WebSocket 连接指针
        /// @param sessionId 会话 ID（私聊会话带标志位）
        void unsubscribeSession(const drogon::WebSocketConnectionPtr &conn, u64 sessionId);

        /// @brief 向订阅该会话或未设置订阅的连接推送新消息
        /// @param sessionId 会话 ID（私聊会话带标志位）
        /// @param role 角色（user/assistant）
        /// @param content 消息内容
        void pushMessage(u64 sessionId, const std::string &role, const std::string &content);

        /// @brief 广播事件到所有连接
        /// @param type 事件类型
        /// @param data 事件数据
        void broadcastEvent(const std::string &type, const json &data) const;

    private:
        WebSocketManager() = default;

        std::unordered_set<drogon::WebSocketConnectionPtr> m_connections; ///< 所有连接
        std::unordered_map<u64, std::unordered_set<drogon::WebSocketConnectionPtr>> m_subscriptions; ///< 会话订阅映射
        mutable std::mutex m_mutex; ///< 线程安全锁
    };
} // namespace insoulforge
