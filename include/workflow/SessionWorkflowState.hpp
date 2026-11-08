/// @file SessionWorkflowState.hpp
/// @brief 单个会话的消息工作流并发状态

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <queue>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/SessionId.hpp>
#include <workflow/MessageList.hpp>

namespace insoulforge {
    /// @brief 一次回复判断或执行的冻结上下文。
    struct ReplyRequest {
        json records;
    };

    /// @brief 入站消息的记录结果与回复消费者启动权。
    struct IncomingMessageUpdate {
        json records;
        bool startReply{false};
    };

    /// @brief 管理单个会话的工作流队列及其消费者状态
    /// @details 预处理使用 FIFO 队列；新增回复需求合并为一个待处理批次。
    ///          队列和运行状态由同一把互斥锁保护，不跨协程等待持锁。
    class SessionWorkflowState {
    public:
        /// @brief 创建会话工作流状态
        /// @param sessionId 会话 ID
        /// @param database 传递给消息列表的数据库，必须覆盖会话状态的生命周期。
        /// @param config 传递给消息列表的运行时配置。
        SessionWorkflowState(const SessionId &sessionId, const Database &database, const Config &config);

        /// @brief 取得当前会话的完整消息列表
        /// @return 仅在构造时创建的消息列表实例
        /// @note 线程安全。返回的 MessageList 自行保证其内容访问的线程安全。
        [[nodiscard]] auto messageList() const noexcept -> const std::shared_ptr<MessageList> &;

        /// @brief 加入等待预处理的消息
        /// @param message 已归一化的入站消息
        /// @return 是否需要由调用者启动预处理消费者
        /// @note 线程安全。
        [[nodiscard]] auto enqueuePreparation(json message) -> bool;

        /// @brief 取出一条等待预处理的消息
        /// @return 队首消息；队列为空时返回空值并结束预处理消费者状态
        /// @note 线程安全。仅应由该会话唯一的预处理消费者调用。
        [[nodiscard]] auto takePreparationMessage() -> std::optional<json>;

        /// @brief 记录已完成预处理的入站消息，并原子登记回复需求。
        /// @return 追加后的消息窗口，以及调用者是否取得消费者启动权。
        [[nodiscard]] auto recordIncoming(json message) -> IncomingMessageUpdate;

        /// @brief 取得下一批 Router 输入；没有新增需求时原子释放消费者所有权。
        /// @note 仅由该会话唯一的回复消费者调用；读取不改变消息处理标记。
        [[nodiscard]] auto takeReplyRequest() -> std::optional<ReplyRequest>;

        /// @brief 重新冻结 Executor 输入，消费冻结前所有待回复需求并标记窗口记录。
        /// @note 与 recordIncoming 使用同一把锁，避免快照和入站通知交错导致重复执行。
        [[nodiscard]] auto takeExecutorRequest() -> ReplyRequest;

    private:
        std::mutex m_mutex;
        std::shared_ptr<MessageList> m_messageList;
        std::queue<json> m_pendingPreparationMessages;
        bool m_isPreparationRunning{false}; ///< 已准备启动或正在执行预处理任务，待预处理队列取空时清除。
        bool m_isReplyProcessing{false}; ///< 已准备启动或正在执行回复任务，没有待处理的新消息时清除。
        bool m_pendingReply{false}; ///< 有新消息尚未纳入本轮 Router 判断或 Executor 执行，取得对应输入时清除。
    };
} // namespace insoulforge
