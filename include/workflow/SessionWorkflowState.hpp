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
    /// @brief 管理单个会话的消息列表与预处理、处理两条消费链。
    /// @details 预处理链按 FIFO 处理入站消息并写入 MessageList；处理链按需启动，
    ///          先取窗口做 Router 判断，需要回复时再冻结一份交给 Executor。
    ///          队列与运行标记由同一把互斥锁保护，临界区内不跨协程等待。
    class SessionWorkflowState {
    public:
        /// @brief 创建会话状态，并从数据库恢复该会话的消息。
        /// @param sessionId 会话身份。
        /// @param database 数据库连接，生命周期须覆盖本对象。
        /// @param config 运行时配置，生命周期须覆盖本对象。
        SessionWorkflowState(const SessionId &sessionId, const Database &database, const Config &config);

        /// @brief 获取该会话的消息列表。
        /// @return 构造时创建的唯一实例。
        /// @note 线程安全；消息内容的并发访问由 MessageList 负责。
        [[nodiscard]] auto messageList() const noexcept -> const std::shared_ptr<MessageList> &;

        /// @brief 将入站消息加入预处理队列尾部。
        /// @param message 已归一化的入站消息。
        /// @return 调用方是否需要启动预处理消费者；已有消费者时返回 false，由它继续取空队列。
        /// @note 线程安全。
        [[nodiscard]] auto enqueuePreparation(json message) -> bool;

        /// @brief 取出队首的待预处理消息。
        /// @return 队首消息；队列为空时返回空值，并将预处理运行标记置为 false，表示消费者可退出。
        /// @note 线程安全；每个会话只应有一个预处理消费者调用。
        [[nodiscard]] auto takePreparationMessage() -> std::optional<json>;

        /// @brief 保存预处理完成的消息，并置待处理标记。
        /// @return 调用方是否取得处理消费者启动权；已有消费者时返回 false，消息仍会写入。
        /// @note 待处理标记不携带消息内容，多条消息合并为一轮判断。
        /// @note 线程安全；写入消息、置标记与抢占启动权在同一临界区完成。
        [[nodiscard]] auto recordIncoming(json message) -> bool;

        /// @brief 取出本轮 Router 判断用的消息窗口。
        /// @return 冻结的窗口快照；没有待处理消息时返回空值并释放消费者所有权。
        /// @note 仅由该会话唯一的处理消费者调用，不改写消息处理标记。
        [[nodiscard]] auto takeProcessingRequest() -> std::optional<json>;

        /// @brief 取出本轮 Executor 执行用的消息窗口。
        /// @return 窗口的独立副本，其中记录被标记为已交给 Executor，待处理标记置为 false。
        /// @note 与 recordIncoming 共用同一把锁，避免同一条消息进入两轮执行。
        [[nodiscard]] auto takeExecutorRequest() -> json;

    private:
        std::mutex m_mutex; ///< 保护以下全部可变状态。
        std::shared_ptr<MessageList> m_messageList; ///< 该会话唯一的消息来源。
        std::queue<json> m_pendingPreparationMessages; ///< 待预处理消息，先进先出。
        bool m_isPreparationRunning{false}; ///< 预处理消费者已在运行中；队列取空时置为 false。
        bool m_isProcessingRunning{false}; ///< 处理消费者已在运行中；没有待处理消息时置为 false。
        bool m_pendingProcessing{false}; ///< 有消息尚未交给 Router 判断；取走输入时置为 false。
    };
} // namespace insoulforge
