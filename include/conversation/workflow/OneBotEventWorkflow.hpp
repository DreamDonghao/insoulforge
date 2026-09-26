/// @file OneBotEventWorkflow.hpp
/// @brief OneBot 上报事件处理工作流

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <drogon/utils/coroutine.h>

#include <conversation/workflow/SessionWorkflowState.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief OneBot 入站事件处理工作流
    /// @details 每个会话串行预处理入站消息；回复处理同一时间只运行一轮。预处理完成后，
    ///          回复阶段从 MessageList 读取快照并调用 Router、Executor 和发送服务。
    ///          空闲时协程退出，不轮询。数据库中的聊天记录用于启动恢复和正常退出时保存消息列表。
    class OneBotEventWorkflow {
    public:
        /// @brief 获取进程内唯一的工作流
        /// @return 单例工作流
        /// @note 线程安全。C++ 保证函数内静态对象只初始化一次；取得实例后仍应遵循各公开成员函数的并发约束。
        [[nodiscard]] static auto instance() -> OneBotEventWorkflow &;

        /// @brief 将所有会话的当前完整消息列表写入数据库恢复副本
        /// @note 线程安全。可与消息处理并发执行，写入的是调用期间取得的各会话消息列表快照。
        void flushMessageListsToStorage();

        /// @brief 接收发送服务已确认投递的机器人消息
        /// @param sessionId 所属会话 ID
        /// @param message 完整助手消息 JSON
        /// @details OneBot 确认发送成功后调用；消息会进入内存列表并推送给管理后台。
        /// @note 线程安全。函数完成时消息已写入内存列表；若触发记忆总结，任务已持久化并交给异步消费者。
        void appendDeliveredAssistantMessage(u64 sessionId, json message);

        /// @brief 获取运行中会话的完整消息列表快照
        /// @param sessionId 所属会话 ID
        /// @return 会话已在工作流中初始化时返回完整消息快照，否则返回空值
        /// @note 线程安全。用于管理后台展示；结果不受模型上下文窗口长度限制。
        [[nodiscard]] auto getSessionMessages(u64 sessionId) -> std::optional<json>;

        /// @brief 将 OneBot 上报事件加入处理流程
        /// @param body 已通过 HTTP JSON 校验的 OneBot 事件对象
        /// @details 归一化并检查机器人状态、发送者后入队；会话尚无预处理任务时启动协程。
        /// @note 线程安全。函数返回仅表示事件已被接受或丢弃，不表示图片识别、回复或记忆维护已经完成。
        void enqueueOneBotEvent(json body);

    private:
        std::mutex m_sessionsMutex; ///< 保护会话状态索引
        std::unordered_map<u64, std::shared_ptr<SessionWorkflowState>> m_sessions; ///< 已恢复或已激活的会话状态

        OneBotEventWorkflow();

        /// @brief 保存命令消息并向命令来源发送执行结果
        auto executeCommand(const json &message) -> drogon::Task<>;

        /// @brief 获取或创建会话工作流状态
        [[nodiscard]] auto getOrCreateSessionState(u64 sessionId) -> std::shared_ptr<SessionWorkflowState>;

        /// @brief 处理一个会话的消息预处理队列，空队列时立即退出
        auto processPreparationQueue(u64 sessionId) -> drogon::Task<>;

        /// @brief 处理一个会话的串行回复工作流，无待处理回复时立即退出
        /// @param triggerMessageId 首轮回复的触发入站消息 ID
        auto processReplyWorkflow(u64 sessionId, std::string triggerMessageId) -> drogon::Task<>;
    };
} // namespace insoulforge
