/// @file ActivityManager.hpp
/// @brief Activity 服务接入、状态缓存与请求转发。
#pragma once
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <drogon/WebSocketConnection.h>
#include <drogon/utils/coroutine.h>
#include <expected>
#include <trantor/net/EventLoop.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/image/Image.hpp>

namespace insoulforge {
    class Config;
    /// @brief 管理 Activity 通信与状态缓存，不解释或持久化服务内部的业务状态。
    /// @details 同连接请求串行，不同连接可以并行；共享数据由互斥锁保护，锁不跨协程挂起。
    /// @note 析构前必须调用 stop，并等待所有在途协程和已排入事件循环的回调完成。
    class ActivityManager {
    public:
        /// @brief 已校验的会话通知，不代表状态更新或业务完成。
        struct Event {
            SessionId sessionId;
            std::string name;
            std::string text;
            std::string mode; ///< silent 仅记录，judge 交给 Router，force 直接进入回复流程。
            std::vector<Image> attachments;
        };
        /// @brief 通知出口；在接收消息的线程中于管理器锁外调用，不提供确认或自动重试。
        using EventSink = std::function<void(Event)>;

        /// @brief 创建独立接入管理器，便于隔离协议测试。
        /// @param sink 通知出口；为空时不投递会话通知。生产单例连接到 EventWorkflow。
        explicit ActivityManager(EventSink sink = {});

        /// @brief 获取进程级管理器，其通知出口接入现有会话处理队列。
        static auto instance() -> ActivityManager &;

        /// @brief 校验并载入 Token 与服务登记，清空内存状态和上传归属。
        /// @pre 尚未接受连接，或已有连接已全部清理；不从磁盘恢复业务状态。
        /// @return 配置非法或仍有连接时返回错误，保留当前配置。
        auto initialize(json config) -> std::expected<void, std::string>;

        /// @brief 返回登记配置、连接情况及最近上报状态的独立副本。
        /// @warning 包含认证 Token，仅供已认证管理接口使用，不得提供给模型。
        [[nodiscard]] auto configuration() const -> json;

        /// @brief 持久化共享 Token；实际变更后撤销旧连接并完成其等待请求。
        /// @param token 空字符串关闭接入；非空至少 16 字节且不能含换行。
        /// @return 校验或保存失败时返回错误，保留原 Token 和连接。
        auto saveToken(std::string token, Config &store) -> std::expected<void, std::string>;

        /// @brief 持久化服务启用设置，不受 active 或在途业务操作限制。
        /// @note 禁用后不进入模型目录和状态上下文，不投递通知；连接仍可上报状态。
        /// @return 服务不存在或保存失败时返回错误，不改变已确认的启用设置。
        auto setEnabled(const std::string &name, bool enabled, Config &store) -> std::expected<void, std::string>;

        /// @brief 移除登记、上传归属与连接，完成等待请求，不停止远端业务。
        /// @note 服务重连会重新登记并默认启用；保存失败保留现有接入。
        auto removeService(const std::string &name, Config &store) -> std::expected<void, std::string>;

        /// @brief 返回已启用且在线服务的 name、description 和 instructions，不要求 active。
        [[nodiscard]] auto directory() const -> json;

        /// @brief 返回已启用、在线且 active 的 name/status 数组，仅读取本地缓存。
        /// @return 无符合条件的服务时返回空数组，由上下文构建方省略对应字段。
        [[nodiscard]] auto activeActivities() const -> json;

        /// @brief 排队转发自定义参数，不注入身份、不要求 active、不自动重试。
        /// @param parameters 必须为 JSON 对象，字段及业务含义由服务约定。
        /// @return 响应包含 success 及 result/error；附件转换为可引用的 image_id。
        /// @note 请求绑定调用时的连接。发送后 10 秒超时会断开连接；已发送项结果未确认，
        ///       未发送项明确未执行，不将队列转交新连接。
        auto invoke(std::string name, json parameters) -> drogon::Task<json>;

        /// @brief 校验共享 Token；空 Token 或管理器已停止时拒绝接入。
        [[nodiscard]] auto authorized(const std::string &token) const -> bool;

        /// @brief 校验并缓存 PNG/JPEG，建立服务级附件引用，不触发状态或通知。
        /// @param bytes 原始图片字节，仅在本次调用期间借用；最大 24 MiB、16 MiPixels。
        /// @return success、file_id、media_type，或未连接、格式无效、缓存超限等错误。
        /// @note 不要求 active；引用有效期 10 分钟，同服务重复上传续期，不跨进程重启保留。
        auto uploadFile(const std::string &name, std::string_view bytes) -> std::expected<json, std::string>;

        /// @brief 复核 Token 后接管握手连接，等待首条 register，不立即开放模型调用。
        /// @return false 表示已停止或认证失败，由调用方关闭连接；true 表示开始等待注册。
        /// @note 10 秒内未完成注册会断开；调用方仍须转交消息与关闭事件。
        auto connect(const std::string &token, drogon::WebSocketConnectionPtr connection) -> bool;

        /// @brief 处理注册、响应、状态、通知与心跳；旧连接或旧认证代次不能写入新状态。
        /// @note 非法协议消息关闭连接。通知在锁外交给出口，业务失败响应不导致断线。
        void receive(const drogon::WebSocketConnectionPtr &connection, const std::string &message,
          drogon::WebSocketMessageType type);

        /// @brief 撤销连接并完成其等待项；仅清理该连接所属状态，不影响同名接管后的服务。
        /// @note 可重复调用；这里只清理本地接入，不主动关闭底层 WebSocket。
        void disconnect(const drogon::WebSocketConnectionPtr &connection);

        /// @brief 启动连接存活与上传过期维护，不启动远端业务；重复调用不创建额外定时器。
        /// @param loop 维护定时器所属事件循环，必须存活到 stop 及相关回调完成。
        void start(trantor::EventLoop &loop);

        /// @brief 停止接入与维护，断开连接、清理上传并完成等待请求，不停止远端业务。
        /// @note 不阻塞等待调用协程恢复；调用方负责在析构前排空在途协程及回调。
        void stop();

    private:
        /// @brief 队列中的一次请求；只有队首可发送，发送后才启动超时计时。
        struct Pending {
            trantor::EventLoop *loop = nullptr; ///< 调用协程恢复及请求定时器所属循环，非拥有指针。
            trantor::TimerId timer;
            std::function<void(json)> callback;
            json message;
            bool sent = false; ///< 区分结果未确认与明确未执行，也决定是否存在请求定时器。
        };

        /// @brief 一个服务接入的登记、状态缓存及当前连接请求队列，不表示业务实例。
        struct Service {
            json config; ///< 仅持久化 name、description、enabled。
            std::string instructions;
            bool active = false; ///< 服务自报的工作标记，只控制模型状态上下文。
            std::string status;
            i64 updatedAt = 0; ///< 最近注册或状态上报的 Unix 秒时间，0 表示尚未上报。
            drogon::WebSocketConnectionPtr connection;
            std::chrono::steady_clock::time_point lastSeen;
            std::deque<std::shared_ptr<Pending>> requests;
        };

        /// @brief 连接级登记；接管后仍持有旧 Service，以便正确清理旧连接的等待项。
        struct Registration {
            std::string name; ///< 注册完成前为空。
            std::shared_ptr<Service> service;
            u64 generation; ///< 握手时的认证代次，Token 变更后旧代次失效。
            trantor::EventLoop *loop;
            trantor::TimerId timer; ///< 注册超时定时器，成功注册后撤销。
        };

        /// @brief 将排队请求适配为协程等待，延后入队以避免挂起前同步恢复。
        class ResponseAwaiter : public drogon::CallbackAwaiter<json> {
        public:
            /// @brief 持有请求参数与目标连接，管理器必须存活到响应完成。
            ResponseAwaiter(
              ActivityManager &manager, std::string name, drogon::WebSocketConnectionPtr connection, json parameters);
            /// @brief 将入队及最终恢复安排到调用线程的事件循环，无当前循环时使用应用主循环。
            auto await_suspend(std::coroutine_handle<> continuation) -> bool;

        private:
            ActivityManager &m_manager;
            std::string m_name;
            drogon::WebSocketConnectionPtr m_connection;
            json m_parameters;
        };
        /// @brief 校验共享 Token、登记名称唯一性、描述与启用字段，不校验业务状态。
        static auto validateConfiguration(const json &config) -> std::expected<void, std::string>;

        /// @brief 构建持久化配置，仅含 Token 与服务登记，不含状态和连接。
        /// @pre 已持有 m_mutex。
        auto configurationLocked() const -> json;

        /// @brief 校验首条注册，保存登记并应用初始状态，发送 ready 后清理被接管连接。
        auto registerService(const drogon::WebSocketConnectionPtr &connection, const json &message)
          -> std::expected<void, std::string>;

        /// @brief 重新核对目标连接及启用设置后入队，不可用时异步返回未执行。
        /// @param loop 回调与协程恢复所属循环，必须存活到请求完成。
        void enqueueRequest(std::string name, drogon::WebSocketConnectionPtr connection, json parameters,
          trantor::EventLoop *loop, std::function<void(json)> callback);

        /// @brief 将队首发送任务排入其事件循环，实际发送前再次检查连接和队首身份。
        /// @pre 已持有 m_mutex，service.requests 非空。
        void dispatchLocked(const std::string &name, Service &service);

        /// @brief 从匹配连接取出队首并推进队列；过期连接不能完成新连接的请求。
        void completeRequest(const std::string &name, const drogon::WebSocketConnectionPtr &connection, json response);

        /// @brief 撤销请求计时、记录调试结果，并将完成回调排入原调用循环。
        /// @pre pending 已从队列移除，调用方保证每项只完成一次。
        void finish(const std::shared_ptr<Pending> &pending, const std::string &name, json response);

        /// @brief 校验可选附件的数量、服务归属和有效期，返回保持图片缓存的句柄。
        /// @pre 已持有 m_mutex。
        auto attachmentsLocked(const std::string &name, const json &message)
          -> std::expected<std::vector<Image>, std::string>;

        /// @brief 原子校验等待项并撤销连接，避免超时回调误取消已推进的下一项。
        /// @param expected 非空时仅撤销仍以该项为队首的连接；为空时无条件清理登记连接。
        /// @return 是否实际清理了连接；不主动关闭底层 WebSocket。
        auto disconnectCurrent(
          const drogon::WebSocketConnectionPtr &connection, const std::shared_ptr<Pending> &expected) -> bool;

        /// @brief 清理过期上传并断开超过 45 秒无入站帧的连接，不查询业务状态。
        void tick();

        /// @brief 有时限的上传归属及图片保护句柄，释放句柄不直接删除共享缓存文件。
        struct Upload {
            Image image;
            size_t size{}; ///< 原始图片字节数，用于全局容量核算。
            std::chrono::steady_clock::time_point expires;
        };

        /// @brief 释放到期或已移除服务的上传归属，不依赖服务工作标记。
        /// @pre 已持有 m_mutex。
        void expireUploadsLocked();
        std::map<std::pair<std::string, std::string>, Upload> m_uploads; ///< 键为服务名称与文件引用。
        mutable std::mutex m_mutex;
        std::map<std::string, std::shared_ptr<Service>> m_services;
        std::string m_token;
        u64 m_authGeneration = 0; ///< Token 实际变更时递增，用于立即拒绝旧连接消息。
        std::map<drogon::WebSocketConnectionPtr, Registration> m_connections;
        EventSink m_sink;
        trantor::EventLoop *m_loop = nullptr;
        trantor::TimerId m_timer{};
        bool m_stopping = false;
    };
} // namespace insoulforge
