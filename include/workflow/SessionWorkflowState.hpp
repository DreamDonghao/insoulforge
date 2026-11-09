/// @file SessionWorkflowState.hpp
/// @brief 单个会话的消息工作流并发状态

#pragma once

#include <mutex>
#include <optional>
#include <queue>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/SessionId.hpp>
#include <workflow/MessageList.hpp>

namespace insoulforge {
    /// @brief 管理单个会话的消息列表，以及加工、处理两条消费链的调度状态。
    /// @details 加工链按 FIFO 逐条加工入站消息（会话配置、命令、图片与记忆富化），
    ///          完成后写入 MessageList 并置待处理标记；处理链按需启动，
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
        [[nodiscard]]
        auto messageList() noexcept ->MessageList &;

        /// @brief 将入站消息加入加工队列尾部。
        /// @param message 已归一化的入站消息。
        /// @note 线程安全；仅入队，不启动消费者，启动由 tryClaimCompose 的结果决定。
        void enqueueCompose(json message);

        /// @brief 尝试取得加工消费者启动权。
        /// @return 取得启动权时返回 true；加工消费者已在运行或队列为空时返回 false。
        /// @note 线程安全；返回 true 的调用方负责启动加工协程，否则由在跑的消费者继续取空队列。
        [[nodiscard("返回值为 true 时必须启动加工")]]
        auto tryClaimCompose() -> bool;

        /// @brief 保存加工完成的消息，并置待处理标记。
        /// @param message 已富化并去掉会话字段、可直接交给 Agent 的完整消息。
        /// @note 待处理标记不携带消息内容，多条消息合并为一轮判断。
        /// @note 线程安全；与 takeExecutorRequest 共用同一把锁。
        void recordIncoming(json message);

        /// @brief 尝试取得处理消费者启动权。
        /// @return 取得启动权时返回 true；处理消费者已在运行或没有待处理消息时返回 false。
        /// @note 线程安全；返回 true 的调用方负责启动处理协程。
        [[nodiscard("返回值为 true 时必须启动处理")]]
        auto tryClaimProcess() -> bool;

        /// @brief 取出队首的一条待加工消息。
        /// @return 队首消息；队列为空时返回空值，并将加工运行标记置为 false，表示消费者可退出。
        /// @note 线程安全；每个会话只应有一个加工消费者调用。
        [[nodiscard]] auto takeComposeEvent() -> std::optional<json>;

        /// @brief 取出本轮 Router 判断用的消息窗口。
        /// @return 冻结的窗口快照；没有待处理消息时返回空值，并将处理运行标记置为 false，表示消费者可退出。
        /// @note 仅由该会话唯一的处理消费者调用，不改写待处理标记。
        [[nodiscard]] auto takeProcessEvents() -> std::optional<json>;

        /// @brief 取出本轮 Executor 执行用的消息窗口。
        /// @return 窗口的独立副本，其中记录被标记为已交给 Executor，待处理标记置为 false。
        /// @note 与 recordIncoming 共用同一把锁，避免同一条消息进入两轮执行。
        [[nodiscard]] auto takeExecutorRequest() -> json;

    private:
        std::mutex m_mutex; ///< 保护以下全部可变状态。
        MessageList m_messageList; ///< 该会话唯一的消息来源。
        std::queue<json> m_pendingComposeMessages; ///< 待加工消息，先进先出。
        bool m_isComposing{false}; ///< 加工消费者已在运行中；队列取空时置为 false。
        bool m_isProcessing{false}; ///< 处理消费者已在运行中；没有待处理消息时置为 false。
        bool m_pendingProcessing{false}; ///< 已写入 MessageList 但尚未交给 Router 判断；取走输入时置为 false。
    };
} // namespace insoulforge
