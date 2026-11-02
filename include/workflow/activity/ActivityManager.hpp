/// @file ActivityManager.hpp
/// @brief 全局单例活动的 HTTP 协议、状态快照及恢复。
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

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
        /// @brief 校验服务配置、加载状态。仅在启动后台同步前调用；失败不替换现有状态。
        auto initialize(json config) -> std::expected<void, std::string>;
        /// @brief 包含共享密钥的配置及运行状态，仅供已认证管理接口使用，不得提供给模型。
        [[nodiscard]] auto configuration() const -> json;
        /// @brief 保存配置草稿，空密钥沿用；已知活动运行中时拒绝配置变更。
        auto saveConfiguration(json config, Config &store) -> std::expected<void, std::string>;
        /// @brief 模型可用活动目录，仅包含启用服务的名称和用途，不包含密钥或地址。
        [[nodiscard]] auto directory() const -> json;
        /// @brief 所有会话共用的运行快照；失联保留记录并在状态文字中标注，不等待网络。
        [[nodiscard]] auto runningActivities() const -> json;

        /// @brief 发起一次工具操作；origin 必须由主程序构造，不信任模型提供的身份。
        auto invoke(std::string operation, std::string name, json arguments, json origin) -> drogon::Task<json>;
        /// @brief 验证回调身份、启动标识与单调序号，持久化后才投递通知。
        auto receiveEvent(const std::string &name, const std::string &token, const json &event)
          -> std::expected<void, std::string>;
        /// @brief 立即同步一个服务，复用启动恢复和失联重试逻辑。
        auto synchronize(std::string name) -> drogon::Task<>;
        /// @brief 安排首次恢复与后台重试；由进程启动线程调用，重复调用无副作用。
        void start(trantor::EventLoop &loop);
        /// @brief 停止同步并拒绝新操作、回调，不停止独立服务中的活动。
        void stop();

    private:
        struct Service {
            json config;
            json runtime = {{"running", false}, {"run_id", ""}, {"sequence", -1}};
            bool busy = false;
            bool synchronized = false;
            bool reachable = false;
            size_t failures = 0;
            i64 nextSync = 0;
        };
        using Services = std::map<std::string, std::shared_ptr<Service>>;

        static auto validateConfiguration(const json &config) -> std::expected<void, std::string>;
        auto persistLocked() const -> std::expected<void, std::string>;
        auto applySnapshotLocked(Service &service, const json &snapshot, bool callback)
          -> std::expected<void, std::string>;
        static auto request(json config, std::string operation, json body) -> drogon::Task<json>;
        void tick();

        mutable std::mutex m_mutex;
        Services m_services;
        std::string m_callbackUrl;
        std::string m_statePath;
        EventSink m_sink;
        trantor::EventLoop *m_loop = nullptr;
        trantor::TimerId m_timer;
        bool m_stopping = false;
    };
} // namespace insoulforge
