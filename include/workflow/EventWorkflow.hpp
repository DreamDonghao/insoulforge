/// @file EventWorkflow.hpp
/// @brief 统一事件处理工作流

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <drogon/utils/coroutine.h>
#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/image/Image.hpp>
#include <workflow/SessionWorkflowState.hpp>

namespace insoulforge {
    class Database;
    class Config;
    class AgentSystem;
    /// @brief 统一事件处理工作流
    /// @details 接收 OneBot 事件、系统任务和活动通知。
    ///          每个会话串行预处理入站消息；回复处理同一时间只运行一轮。预处理完成后，
    ///          回复阶段从 MessageList 读取快照并调用 Router、Executor 和发送服务。
    ///          空闲时协程退出，不轮询。数据库中的聊天记录用于启动恢复和正常退出时保存消息列表。
    /// @note 预处理与回复可并行，不同会话独立处理；使用写入、入队接口前必须成功初始化。
    ///       退出时应先停止事件入口并等待在途任务结束，再保存消息及释放依赖。
    class EventWorkflow {
    public:
        /// @brief 获取进程内唯一的工作流
        /// @return 单例工作流
        /// @note 线程安全。C++ 保证函数内静态对象只初始化一次；取得实例后仍应遵循各公开成员函数的并发约束。
        [[nodiscard]] static auto instance() -> EventWorkflow &;

        /// @brief 显式恢复会话消息列表；获取单例不会触发恢复。
        /// @param database 已成功打开并完成迁移的数据库。
        /// @param config 已成功加载的运行时配置。
        /// @param agent 已成功初始化的 Agent，用于检查是否允许处理消息。
        /// @return 初始化成功时为空；数据库或 Agent 未就绪、依赖被替换或恢复失败时返回错误说明。
        /// @note 仅在启动期、接收事件前调用；依赖对象必须比工作流存活更久。
        /// 同一组依赖重复初始化不会重复恢复消息。
        [[nodiscard]] auto initialize(const Database &database, const Config &config, const AgentSystem &agent)
          -> std::expected<void, std::string>;

        /// @brief 将所有会话的当前完整消息列表写入数据库恢复副本
        /// @note 线程安全。可与消息处理并发执行，写入的是调用期间取得的各会话消息列表快照。
        void flushMessageListsToStorage();

        /// @brief 接收发送服务已确认投递的机器人消息
        /// @param sessionId 所属会话 ID
        /// @param message 完整助手消息 JSON
        /// @details OneBot 确认发送成功后调用；消息会进入内存列表并推送给管理后台。
        /// @note 线程安全。函数完成时消息已写入内存列表；追加本身不触发记忆维护。
        void appendDeliveredAssistantMessage(const SessionId &sessionId, json message);

        /// @brief 将后台任务系统状态写入完整消息列表，不触发 Router 或 Executor
        /// @param sessionId 所属会话 ID
        /// @param message 已构造好的完整系统消息 JSON
        /// @note 线程安全。会推送管理后台，并按普通消息参与后续快照与记忆维护。
        void appendSystemStatusMessage(const SessionId &sessionId, json message);

        /// @brief 保存内部助手执行记录，不发送 QQ 消息，也不投递新回复任务。
        /// @param sessionId 记录所属会话。
        /// @param message 已构造好的助手执行记录，包含工具历史及执行结果。
        /// @note 线程安全。写入内存消息列表并推送管理后台；追加本身不触发记忆维护。
        void appendAssistantExecutionRecord(const SessionId &sessionId, json message);

        /// @brief 获取运行中会话的完整消息列表快照
        /// @param sessionId 所属会话 ID
        /// @return 会话已在工作流中初始化时返回完整消息快照，否则返回空值
        /// @note 线程安全。供后台及工具读取；结果不受模型上下文窗口长度限制，工具须自行校验权限和投影。
        [[nodiscard]] auto getSessionMessages(const SessionId &sessionId) -> std::optional<json>;

        /// @brief 读取指定会话有限条数的共享消息，不创建或改变会话状态。
        /// @param sessionId 消息来源会话。
        /// @param limit 最多返回的消息数；为零时返回空数组。
        /// @param earliest 时间下界（含），格式为本地时间 YYYY-MM-DD HH:MM:SS。
        /// @param latest 时间上界（含），格式与 earliest 相同。
        /// @return 时间范围内最近的可共享消息投影，按时间顺序排列；会话不存在时返回空数组。
        /// @note 线程安全。不校验共享开关或调用权限，由调用方限制来源和可见范围。
        [[nodiscard]] auto getSharedSessionMessages(
          const SessionId &sessionId, size_t limit, const std::string &earliest, const std::string &latest) -> json;

        /// @brief 清空会话的内存消息与持久化恢复副本，保留启用状态与已提取记忆。
        /// @param sessionId 要清空消息的会话；未加载的会话只清除数据库记录。
        /// @note 线程安全，但不取消已排队或正在处理的任务；这些任务仍可能追加新消息。
        void clearSessionMessages(const SessionId &sessionId);

        /// @brief 将 OneBot 上报事件加入处理流程
        /// @param body 由 HTTP 或 WebSocket 入口解析得到的 OneBot 事件对象。
        /// @details 归一化并检查机器人状态、发送者后入队；会话尚无预处理任务时启动协程。
        /// @note 线程安全。函数返回仅表示事件已被接受或丢弃，不表示图片识别、回复或记忆维护已经完成。
        void enqueueOneBotEvent(const json &body);

        /// @brief 内部定时任务直接入队，不经过 OneBot 协议字段转换。
        /// @param sessionId 任务所属会话。
        /// @param text 系统消息正文。
        /// @param senderName 系统发送者的显示名称；内部身份固定为 system。
        /// @note 线程安全。沿用普通入队检查和会话启用设置，返回时不保证处理完成。
        void enqueueSystemMessage(const SessionId &sessionId, std::string text, std::string senderName = "系统任务");

        /// @brief 活动事件使用会话串行队列；silent 仅记录，judge 路由判断，force 跳过回复判断。
        /// @param sessionId 接收通知的会话。
        /// @param text 活动通知正文，可与图片附件一起提供。
        /// @param name 活动名称，用于发送者显示及活动元数据。
        /// @param mode 经活动管理器校验的 silent、judge 或 force；此处不重复校验。
        /// @param origin 活动最初的启动来源，含 session_id 和 user_id，不替代系统发送者身份。
        /// @param attachments 已缓存的图片；仅在本次调用期间借用，消息中保存图片 ID。
        /// @note 线程安全。仍受机器人就绪状态和会话启用设置限制，force 不绕过这些检查。
        ///       函数返回不表示通知已处理或回复已发送，也不接管图片句柄的生命周期。
        void enqueueActivityMessage(const SessionId &sessionId, std::string text, std::string name, std::string mode,
          json origin = json::object(), const std::vector<Image> &attachments = {});

    private:
        /// @brief 检查归一化消息的发送者与 Agent 状态，并在需要时启动会话预处理协程。
        /// @param message 含 session_id 的完整内部消息；助手消息和无效或被拉黑的发送者会被丢弃。
        /// @note 取得消息所有权；未初始化或 Agent 不可用时直接丢弃。
        void enqueueNormalizedMessage(json message);

        /// @brief 启动时绑定的非拥有依赖；初始化后不再替换。
        struct Dependencies {
            const Database &database;
            const Config &config;
            const AgentSystem &agent;
        };
        std::optional<Dependencies> m_dependencies;
        std::atomic_bool m_initialized{false};
        std::mutex m_sessionsMutex; ///< 保护会话状态索引
        std::unordered_map<SessionId, std::shared_ptr<SessionWorkflowState>> m_sessions; ///< 已恢复或已激活的会话状态

        EventWorkflow() = default;

        /// @brief 保存命令消息并向命令来源发送执行结果
        /// @param message 已识别的内部命令消息。
        /// @note 调用方必须 co_await 至完成，并保证 message 在整个协程期间有效。
        auto executeCommand(const json &message) -> drogon::Task<>;

        /// @brief 获取或创建会话工作流状态
        /// @param sessionId 状态所属会话。
        /// @return 由工作流和在途任务共同持有的会话状态。
        /// @pre initialize 已成功完成。
        /// @note 线程安全；索引锁内完成查找与首次创建。
        [[nodiscard]] auto getOrCreateSessionState(const SessionId &sessionId) -> std::shared_ptr<SessionWorkflowState>;

        /// @brief 处理一个会话的消息预处理队列，空队列时立即退出
        /// @param sessionId 待处理队列所属会话，按值保存在协程帧中。
        /// @note 仅在取得该会话预处理执行权后启动；单条消息失败不阻塞后续消息。
        auto processPreparationQueue(SessionId sessionId) -> drogon::Task<>;

        /// @brief 处理一个会话的串行回复工作流，无待处理回复时立即退出
        /// @param sessionId 回复任务所属会话，按值保存在协程帧中。
        /// @param request 首轮触发消息及可选的提前路由结果
        /// @note 仅在取得该会话回复执行权后启动；每轮读取最新消息快照，私聊跳过 Router。
        ///       当前轮结束后接续最新待处理请求，无请求时释放执行权并退出。
        auto processReplyWorkflow(SessionId sessionId, ReplyRequest request) -> drogon::Task<>;
    };
} // namespace insoulforge
