/// @file OneBotWebSocketClient.hpp
/// @brief OneBot 正向 WebSocket 连接管理器
#pragma once

#include <coroutine>
#include <cstdint>
#include <drogon/WebSocketClient.h>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge {
    /// @brief 管理到 OneBot 实现的唯一 WebSocket 连接
    /// @details 连接用于双向 OneBot 11 通信：接收事件上报，并用 echo 关联动作请求与响应。
    class OneBotWebSocketClient {
    public:
        /// @brief 获取全局 WebSocket 连接管理器
        static OneBotWebSocketClient &instance();

        /// @brief 按当前配置建立 WebSocket 连接
        /// @details 仅当 oneBotTransport 为 websocket 时生效；连接断开后会延迟重连。
        void start();

        /// @brief 停止连接、取消重连并使等待中的动作请求失败
        void stop();

        /// @brief 在 OneBot 配置保存后切换连接状态
        void reconfigure();

        /// @brief 判断正向 WebSocket 是否已建立连接
        [[nodiscard]] bool isConnected() const;

        /// @brief 通过 WebSocket 调用 OneBot 11 动作 API
        /// @param action OneBot action，例如 send_group_msg
        /// @param params 动作参数
        /// @param timeout 超时秒数
        /// @return OneBot 完整响应；连接不可用、断开或超时时返回 nullopt
        [[nodiscard]] drogon::Task<std::optional<json>> callApi(std::string action, json params, double timeout);

    private:
        using ResponseCallback = std::function<void(std::optional<json>)>;

        class ApiResponseAwaiter : public drogon::CallbackAwaiter<std::optional<json>> {
        public:
            ApiResponseAwaiter(OneBotWebSocketClient &client, std::string action, json params, double timeout);

            bool await_suspend(std::coroutine_handle<> continuation);

        private:
            OneBotWebSocketClient &m_client;
            std::string m_action;
            json m_params;
            double m_timeout;
        };

        OneBotWebSocketClient() = default;

        void connect(std::uint64_t generation);

        void scheduleReconnect(std::uint64_t generation);

        bool sendApiRequest(std::string action, json params, double timeout, ResponseCallback callback);

        void handleMessage(const std::string &message, drogon::WebSocketMessageType type);

        void handleConnectionClosed(const drogon::WebSocketClientPtr &client);

        void resolveRequest(const std::string &echo, std::optional<json> response);

        static bool splitWebSocketUrl(const std::string &url, std::string &origin, std::string &path);

        [[nodiscard]] std::vector<ResponseCallback> takePendingCallbacksLocked();

        static void failRequests(const std::vector<ResponseCallback> &callbacks);

        mutable std::mutex m_mutex;
        bool m_running = false;
        std::uint64_t m_generation = 0;
        std::uint64_t m_nextEcho = 0;
        drogon::WebSocketClientPtr m_client;
        drogon::WebSocketConnectionPtr m_connection;
        std::unordered_map<std::string, ResponseCallback> m_pendingRequests;
    };
} // namespace insoulforge
