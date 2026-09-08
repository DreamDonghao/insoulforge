/// @file OneBotEventWorkflow.hpp
/// @brief OneBot 上报事件处理工作流

#pragma once
#include <conversation/workflow/MessageList.hpp>
#include <drogon/utils/coroutine.h>
#include <infrastructure/JsonUtil.hpp>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <unordered_map>

namespace insoulforge {
    /// @brief OneBot 入站事件处理工作流
    /// @details 单例按会话维护两级事件驱动队列：预处理阶段完成图片/GIF 识别、记忆召回、入库与快照；
    ///          回复阶段消费快照并调用 Router、Executor 和发送服务。队列为空时对应 drain 协程立即退出，
    ///          不使用轮询或空转等待。运行时消息上下文仅来自 MessageList，数据库只承担启动恢复与正常退出
    ///          时的持久化职责。
    class OneBotEventWorkflow {
        /// @brief 单个会话的工作流运行状态
        /// @details 队列与消费者标志均由 mutex 保护；消息列表自行保护其内容与快照。
        struct SessionWorkflowState {
            explicit SessionWorkflowState(uint64_t sessionId);

            std::mutex queueMutex; ///< 保护队列与消费者状态，绝不跨协程等待持有
            std::shared_ptr<MessageList> messageList; ///< 当前会话的完整消息列表
            std::queue<json> pendingPreparationMessages; ///< 等待媒体、召回、入库与快照的消息
            bool isPreparationRunning{false}; ///< 是否已有预处理协程
            std::queue<json> pendingReplySnapshots; ///< 等待 Router、Agent 与发送的消息快照
            bool isReplyProcessing{false}; ///< 是否已有回复协程
        };

    public:
        /// @brief 获取进程内唯一的工作流
        /// @return 单例工作流
        /// @note 线程安全。C++ 保证函数内静态对象只初始化一次；取得实例后仍应遵循各公开成员函数的并发约束。
        [[nodiscard]] static OneBotEventWorkflow &instance();

        /// @brief 将所有会话的当前完整消息列表写入数据库恢复副本
        /// @note 线程安全。可与消息处理并发执行，写入的是调用期间取得的各会话消息列表快照。
        void flushMessageListsToStorage();

        /// @brief 接收发送服务已确认投递的机器人消息
        /// @param sessionId 所属会话 ID
        /// @param message 完整助手消息 JSON
        /// @param displayContent 管理后台展示的已发送消息文本
        /// @details 发送服务完成写入后调用；消息会进入所属会话的内存列表并发布已记录事件。
        /// @note 线程安全。函数完成时消息已写入内存列表；触发的记忆总结任务已持久化并异步执行。
        void appendDeliveredAssistantMessage(uint64_t sessionId, json message, const std::string &displayContent = {});

        /// @brief 将 OneBot 上报事件加入处理流程
        /// @param body 已通过 HTTP JSON 校验的 OneBot 事件对象
        /// @details 归一化后仅执行入队操作；队列从空闲变为非空时启动对应会话的预处理协程。
        /// @note 线程安全。函数返回仅表示事件已被接受或丢弃，不表示图片识别、回复或记忆维护已经完成。
        void enqueueOneBotEvent(json body);

    private:
        std::mutex m_sessionsMutex; ///< 保护会话状态索引
        std::unordered_map<uint64_t, std::shared_ptr<SessionWorkflowState>> m_sessions; ///< 已恢复或已激活的会话状态

        OneBotEventWorkflow();

        /// @brief 保存命令消息并向命令来源发送执行结果
        drogon::Task<> executeCommand(const json &message);

        /// @brief 获取或创建会话工作流状态
        [[nodiscard]] std::shared_ptr<SessionWorkflowState> getOrCreateSessionState(uint64_t sessionId);

        /// @brief 处理一个会话的消息预处理队列，空队列时立即退出
        drogon::Task<> processPreparationQueue(uint64_t sessionId);

        /// @brief 处理一个会话的回复队列，空队列时立即退出
        drogon::Task<> processReplyQueue(uint64_t sessionId);
    };
} // namespace insoulforge
