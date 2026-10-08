/// @file OneBotWebSocketClient.hpp
/// @brief OneBot 正向 WebSocket 连接管理器
#pragma once

#include <coroutine>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <drogon/WebSocketClient.h>
#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 管理到 OneBot 实现的唯一 WebSocket 连接
    /// @details 连接用于双向 OneBot 11 通信：接收事件上报，并用 echo 关联动作请求与响应。
    class OneBotWebSocketClient {
    public:
        /// @brief 获取全局 WebSocket 连接管理器
        static auto instance() -> OneBotWebSocketClient &;

        /// @brief 按当前配置建立 WebSocket 连接
        /// @details 仅当 oneBotTransport 为 websocket 时生效；重复启动不建立额外连接。
        /// 连接在事件循环中异步建立，失败或断开后等待 3 秒再重连；地址无效时不重试。
        void start();

        /// @brief 停止连接、取消重连并使等待中的动作请求失败
        /// @details 通过更新连接代次使已排队的连接和重连操作失效，不直接删除重连定时器。
        void stop();

        /// @brief 在 OneBot 配置保存后切换连接状态
        /// @details 先停止旧连接，再按当前配置启动；切换为 HTTP 时不再建立 WebSocket 连接。
        void reconfigure();

        /// @brief 判断正向 WebSocket 是否已建立连接
        /// @return 当前连接存在且仍处于连接状态时返回 true；不表示 OneBot 动作一定执行成功。
        [[nodiscard]] auto isConnected() const -> bool;

        /// @brief 通过 WebSocket 调用 OneBot 11 动作 API
        /// @param action OneBot action，例如 send_group_msg
        /// @param params 动作参数
        /// @param timeout 等待响应的超时秒数，调用方应传入正数
        /// @return OneBot 完整响应，包括 status 为 failed 的响应；连接不可用、发送失败、
        /// 连接断开、主动停止或等待超时时返回 nullopt。
        /// @note 每个请求通过独立 echo 匹配响应，超时或断开后不自动重发动作。
        [[nodiscard]] auto callApi(std::string action, json params, f64 timeout) -> drogon::Task<std::optional<json>>;

    private:
        /// @brief 动作请求的完成回调；nullopt 表示未取得响应，而不是 OneBot 业务失败。
        using ResponseCallback = std::function<void(std::optional<json>)>;

        /// @brief 将动作请求的回调转换为协程等待结果
        /// @details 等待器由 callApi 的协程帧持有，直到请求完成后恢复协程。
        class ApiResponseAwaiter : public drogon::CallbackAwaiter<std::optional<json>> {
        public:
            /// @brief 保存请求参数，在协程挂起时发起请求
            ApiResponseAwaiter(OneBotWebSocketClient &client, std::string action, json params, f64 timeout);

            /// @brief 发起请求，并在响应或失败回调中恢复协程
            /// @return 请求已提交时返回 true；无法提交时设置空结果并返回 false，不挂起协程。
            auto await_suspend(std::coroutine_handle<> continuation) -> bool;

        private:
            OneBotWebSocketClient &m_client;
            std::string m_action;
            json m_params;
            f64 m_timeout;
        };

        OneBotWebSocketClient() = default;

        /// @brief 为指定连接代次建立连接；已经停止或代次过期时不再连接。
        /// @param generation 启动时分配的连接代次，用于防止旧操作覆盖新连接
        void connect(u64 generation);

        /// @brief 延迟 3 秒尝试重连，执行时由 connect 检查代次是否仍有效。
        void scheduleReconnect(u64 generation);

        /// @brief 登记请求回调、发送带 echo 的动作，并安排超时处理
        /// @return 请求已提交时返回 true，由回调交付结果；无法提交时返回 false，不调用回调。
        auto sendApiRequest(std::string action, json params, f64 timeout, ResponseCallback callback) -> bool;

        /// @brief 分发文本 JSON：带 echo 的消息用于完成请求，带 post_type 的消息进入事件工作流。
        /// @details 忽略心跳帧；关闭帧仅记录原因，实际断开由连接关闭回调处理。
        /// 其他非文本消息及无效 JSON 不进入业务处理。
        void handleMessage(const std::string &message, drogon::WebSocketMessageType type);

        /// @brief 清理当前连接并使待处理请求失败，需要时安排重连；忽略旧客户端的关闭通知。
        void handleConnectionClosed(const drogon::WebSocketClientPtr &client);

        /// @brief 移除 echo 对应的请求，在释放锁后调用其完成回调。
        /// @details 已完成或未知的 echo 会被忽略，避免迟到响应或超时回调重复完成请求。
        void resolveRequest(const std::string &echo, std::optional<json> response);

        /// @brief 将 ws/wss 地址拆成连接地址和握手路径，不接受带片段标识 # 的地址。
        /// @param origin 输出协议、主机及可选端口
        /// @param path 输出路径及查询参数；没有路径时使用 /
        /// @return 通过基本格式检查时返回 true；不负责完整 URL 校验或确认主机可达。
        static auto splitWebSocketUrl(const std::string &url, std::string &origin, std::string &path) -> bool;

        /// @brief 取走并清空所有待处理回调，不在此处调用回调。
        /// @pre 调用方已经持有 m_mutex。
        [[nodiscard]] auto takePendingCallbacksLocked() -> std::vector<ResponseCallback>;

        /// @brief 用空结果完成取出的请求，使等待协程获知连接不可用。
        /// @pre 调用方不持有 m_mutex，避免恢复的协程再次访问客户端时死锁。
        static void failRequests(const std::vector<ResponseCallback> &callbacks);

        mutable std::mutex m_mutex; ///< 保护下列连接状态和请求表；请求完成回调在锁外执行。
        bool m_running = false; ///< 是否允许建立连接和重连，不等同于当前已连接。
        u64 m_generation = 0; ///< 启动或停止时递增，使旧连接操作和延迟重连失效。
        u64 m_nextEcho = 0; ///< 动作请求编号，重连时不重置，避免迟到响应匹配新请求。
        drogon::WebSocketClientPtr m_client;
        drogon::WebSocketConnectionPtr m_connection;
        std::unordered_map<std::string, ResponseCallback> m_pendingRequests; ///< 按 echo 等待响应的请求回调。
    };
} // namespace insoulforge
