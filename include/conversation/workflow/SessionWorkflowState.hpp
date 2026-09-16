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
        /// @brief 将回复快照加入队列后的调度结果
        enum class ReplyEnqueueResult {
            StartConsumer, ///< 队列此前空闲，调用者应启动回复消费者
            Queued, ///< 已有回复消费者，消息已加入队列
            Skipped, ///< 已有回复消费者，当前消息不应排队
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

        /// @brief 按当前回复处理状态决定是否加入回复快照队列
        /// @param snapshot 固定的消息列表快照
        /// @param mayQueueWhileProcessing 当前已有回复任务时是否仍应排队
        /// @return 入队或跳过结果
        /// @note 线程安全。
        [[nodiscard]] ReplyEnqueueResult enqueueReplySnapshot(json snapshot, bool mayQueueWhileProcessing);

        /// @brief 取出一份等待回复的消息快照
        /// @return 队首快照；队列为空时返回空值并结束回复消费者状态
        /// @note 线程安全。仅应由该会话唯一的回复消费者调用。
        [[nodiscard]] std::optional<json> takeReplySnapshot();

    private:
        std::mutex m_mutex;
        std::shared_ptr<MessageList> m_messageList;
        std::queue<json> m_pendingPreparationMessages;
        bool m_isPreparationRunning{false};
        std::queue<json> m_pendingReplySnapshots;
        bool m_isReplyProcessing{false};
    };
} // namespace insoulforge
