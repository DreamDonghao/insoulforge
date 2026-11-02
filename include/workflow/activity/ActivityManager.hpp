/// @file ActivityManager.hpp
/// @brief 全局单例活动的反向 WebSocket 协议、状态快照及恢复。
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <trantor/net/EventLoop.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>

namespace insoulforge {
    class Config;

    /// @brief 管理独立服务中的活动。互斥锁不跨网络等待；同名操作与同步互斥。
    /// @note 不执行具体业务，不随主程序退出停止远端活动。调用 stop 后仍须等待在途协程结束才能析构。
    class ActivityManager {
    public:
        struct Event {
            SessionId sessionId;
            std::string name;
            std::string text;
            std::string mode; ///< silent / judge / force
            json origin; ///< 最初启动的会话和用户；不替代内部系统发送者。
        };
        using EventSink = std::function<void(Event)>;

        /// @brief 独立路径和事件出口便于离线协议测试；生产使用 instance()。
        explicit ActivityManager(std::string statePath = "data/activity-state.json", EventSink sink = {});
        static auto instance() -> ActivityManager &;
        /// @brief 校验服务配置、加载状态。仅在接受服务连接前调用；失败不替换现有状态。
        auto initialize(json config) -> std::expected<void, std::string>;
        /// @brief 包含共享密钥的配置及运行状态，仅供已认证管理接口使用，不得提供给模型。
        [[nodiscard]] auto configuration() const -> json;
        /// @brief 保存统一接入 Token，变更后关闭所有旧连接，空值表示关闭接入。
        auto saveToken(std::string token, Config &store) -> std::expected<void, std::string>;
        /// @brief 仅更新指定服务，避免管理后台的旧列表覆盖自动注册结果。
        auto setEnabled(const std::string &name, bool enabled, Config &store) -> std::expected<void, std::string>;
        /// @brief 清理服务记录；force 允许清理未结束活动并断开连接，但不停止独立服务。
        /// @note 服务重新连接时仍会自动登记。
        auto removeService(const std::string &name, Config &store, bool force = false)
          -> std::expected<void, std::string>;
        /// @brief 模型可用活动目录，仅包含启用服务的名称和用途，不包含密钥或地址。
        [[nodiscard]] auto directory() const -> json;
        /// @brief 所有会话共用的运行快照；失联保留记录并在状态文字中标注，不等待网络。
        [[nodiscard]] auto runningActivities() const -> json;

        /// @brief 发起一次工具操作；origin 必须由主程序构造，不信任模型提供的身份。
        auto invoke(std::string operation, std::string name, json arguments, json origin) -> drogon::Task<json>;
        /// @brief 主动核对实际状态，与工具操作互斥，不触发回复。
        auto refresh(std::string name) -> drogon::Task<std::expected<void, std::string>>;
        /// @brief 升级前验证统一接入 Token，不使用管理员 Cookie。
        [[nodiscard]] auto authorized(const std::string &token) const -> bool;
        /// @brief 认证连接后等待 register；未在 10 秒内注册就关闭。
        auto connect(const std::string &token, drogon::WebSocketConnectionPtr connection) -> bool;
        /// @brief 处理当前连接的响应、状态事件和心跳，旧连接不具有写入权限。
        void receive(const drogon::WebSocketConnectionPtr &connection, const std::string &message,
          drogon::WebSocketMessageType type);
        /// @brief 使该连接的等待请求失败；只有当前连接断开才改变服务状态。
        void disconnect(const drogon::WebSocketConnectionPtr &connection);
        /// @brief 检查连接存活并低频核对状态；默认 60 秒，较短间隔用于协议测试。
        void start(trantor::EventLoop &loop, std::chrono::seconds checkInterval = std::chrono::seconds{60});
        /// @brief 关闭连接并拒绝新操作、事件，不停止独立服务中的活动。
        void stop();

    private:
        struct Service {
            json config;
            json runtime = {{"running", false}, {"run_id", ""}, {"sequence", -1}};
            bool busy = false;
            bool synchronized = false;
            drogon::WebSocketConnectionPtr connection;
            std::chrono::steady_clock::time_point lastSeen;
            std::chrono::steady_clock::time_point lastCheck;
        };
        using Services = std::map<std::string, std::shared_ptr<Service>>;
        struct Registration {
            std::string name;
            u64 generation;
            trantor::EventLoop *loop;
            trantor::TimerId timer;
        };
        struct Pending {
            drogon::WebSocketConnectionPtr connection;
            trantor::EventLoop *loop;
            trantor::TimerId timer;
            std::function<void(json)> callback;
            std::string name;
            json message;
        };
        class ResponseAwaiter : public drogon::CallbackAwaiter<json> {
        public:
            ResponseAwaiter(ActivityManager &manager, std::string name, drogon::WebSocketConnectionPtr connection,
              std::string operation, json body);
            auto await_suspend(std::coroutine_handle<> continuation) -> bool;

        private:
            ActivityManager &m_manager;
            std::string m_name;
            drogon::WebSocketConnectionPtr m_connection;
            std::string m_operation;
            json m_body;
        };

        static auto validateConfiguration(const json &config) -> std::expected<void, std::string>;
        auto configurationLocked() const -> json;
        auto registerService(const drogon::WebSocketConnectionPtr &connection, const json &message)
          -> std::expected<void, std::string>;
        auto persistLocked() const -> std::expected<void, std::string>;
        auto applySnapshotLocked(Service &service, const json &snapshot, bool fromEvent)
          -> std::expected<void, std::string>;
        auto request(std::string name, drogon::WebSocketConnectionPtr connection, std::string operation, json body)
          -> drogon::Task<json>;
        void sendRequest(std::string name, drogon::WebSocketConnectionPtr connection, std::string operation, json body,
          trantor::EventLoop *loop, std::function<void(json)> callback);
        void completeRequest(const std::string &id, const drogon::WebSocketConnectionPtr &connection, json response);
        enum class SyncMode { Recovery, Check, AfterOperation };
        auto synchronize(std::string name, drogon::WebSocketConnectionPtr connection, SyncMode mode)
          -> drogon::Task<std::expected<void, std::string>>;
        auto receiveEvent(const std::string &name, const drogon::WebSocketConnectionPtr &connection, const json &event)
          -> std::expected<void, std::string>;
        void tick();

        mutable std::mutex m_mutex;
        Services m_services;
        std::string m_token;
        u64 m_authGeneration = 0;
        std::map<drogon::WebSocketConnectionPtr, Registration> m_connections;
        std::map<std::string, Pending> m_pending;
        std::string m_statePath;
        EventSink m_sink;
        trantor::EventLoop *m_loop = nullptr;
        trantor::TimerId m_timer;
        std::chrono::seconds m_checkInterval{60};
        bool m_stopping = false;
    };
} // namespace insoulforge
