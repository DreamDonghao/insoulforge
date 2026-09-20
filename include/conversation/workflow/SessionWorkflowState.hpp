/// @file SessionWorkflowState.hpp
/// @brief 单个会话的消息工作流并发状态

#pragma once

#include <conversation/workflow/MessageList.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>

namespace insoulforge {
    /// @brief 管理单个会话的工作流队列及其消费者状态
    /// @details 每个队列的入队、出队及消费者状态变更由同一把互斥锁保护。
    ///          调用者不应跨协程等待持有内部锁。
    class SessionWorkflowState {
    public:
        /// @brief 请求回复处理后的调度结果
        enum class ReplyRequestResult {
            StartProcessor, ///< 当前没有回复任务，调用者应启动回复处理协程
            Pending, ///< 当前回复任务结束后，需要基于最新上下文再处理一次
            Skipped, ///< 当前已有回复任务，普通消息不触发额外回复
        };

        /// @brief 创建会话工作流状态
        /// @param sessionId 会话 ID
        explicit SessionWorkflowState(uint64_t sessionId);

        /// @brief 取得当前会话的完整消息列表
        /// @return 仅在构造时创建的消息列表实例
        /// @note 线程安全。返回的 MessageList 自行保证其内容访问的线程安全。
        [[nodiscard]] const std::shared_ptr<MessageList> &messageList() const noexcept;

        /// @brief 加入等待预处理的消息
        /// @param message 已归一化的入站消息
        /// @return 是否需要由调用者启动预处理消费者
        /// @note 线程安全。
        [[nodiscard]] bool enqueuePreparation(json message);

        /// @brief 取出一条等待预处理的消息
        /// @return 队首消息；队列为空时返回空值并结束预处理消费者状态
        /// @note 线程安全。仅应由该会话唯一的预处理消费者调用。
        [[nodiscard]] std::optional<json> takePreparationMessage();

        /// @brief 请求会话回复处理
        /// @param mayWaitForCurrentReply 当前已有回复任务时是否仍应等待下一轮回复
        /// @return 应立即启动、等待下一轮或跳过的结果
        /// @details 不保存消息快照。回复处理协程开始每一轮时从 MessageList 获取最新上下文，
        ///          以包含等待期间已发送的助手消息和新到的强制回复消息。
        /// @note 线程安全。
        [[nodiscard]] ReplyRequestResult requestReplyProcessing(bool mayWaitForCurrentReply);

        /// @brief 完成当前一轮回复处理并决定是否开始下一轮
        /// @return 等待期间是否收到了需要额外回复的消息
        /// @note 线程安全。仅应由该会话唯一的回复处理协程调用。
        [[nodiscard]] bool completeReplyProcessing();

    private:
        std::mutex m_mutex;
        std::shared_ptr<MessageList> m_messageList;
        std::queue<json> m_pendingPreparationMessages;
        bool m_isPreparationRunning{false};
        bool m_isReplyProcessing{false};
        bool m_hasPendingReply{false};
    };
} // namespace insoulforge
