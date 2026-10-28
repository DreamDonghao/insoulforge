/// @file SessionWorkflowState.hpp
/// @brief 单个会话的消息工作流并发状态

#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <queue>

#include <workflow/MessageList.hpp>
#include <workflow/agent/runtime/AgentTypes.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 下一轮的触发消息及可复用的路由结果；没有结果时在执行前正常路由。
    struct ReplyRequest {
        std::string messageId;
        std::optional<RouterDecision> decision;
    };

    /// @brief 管理单个会话的工作流队列及其消费者状态
    /// @details 预处理使用 FIFO 队列；回复处理中只保存最新一条必须继续处理的消息 ID。
    ///          队列和运行状态由同一把互斥锁保护，不跨协程等待持锁。
    class SessionWorkflowState {
    public:
        /// @brief 创建会话工作流状态
        /// @param sessionId 会话 ID
        /// @param database 传递给消息列表的数据库，必须覆盖会话状态的生命周期。
        /// @param config 传递给消息列表的运行时配置。
        SessionWorkflowState(u64 sessionId, const Database &database, const Config &config);

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

        /// @brief 当前是否已有串行回复任务。结果仅用于决定是否提前路由，任务所有权仍由请求方法判定。
        [[nodiscard]] auto isReplyProcessing() -> bool;

        /// @brief 请求会话回复处理
        /// @param triggerMessageId 本轮由其触发的入站消息 ID
        /// @param mayWaitForCurrentReply 已有回复任务时，是否保留该消息供下一轮处理
        /// @param decision 可选的提前路由结果，随触发消息一起保存，避免下一轮重复判断
        /// @return 当前无回复任务时返回应立即处理的请求；其他情况返回空值
        /// @details 已有回复任务时，允许等待的消息覆盖待处理触发 ID，连续多条合并为一轮。
        ///          调用方允许私聊、群内 @机器人、系统消息及提前路由判定需回复的消息等待。
        ///          下一轮从 MessageList 重新取得快照，因此不会丢失期间记录的消息。
        /// @note 线程安全。
        [[nodiscard]] auto requestReplyProcessing(std::string triggerMessageId, bool mayWaitForCurrentReply,
          std::optional<RouterDecision> decision = std::nullopt) -> std::optional<ReplyRequest>;

        /// @brief 完成当前一轮回复处理并决定是否开始下一轮
        /// @return 等待期间最新的待处理请求；没有时返回空值并结束回复处理状态
        /// @note 线程安全。仅应由该会话唯一的回复处理协程调用。
        [[nodiscard]] auto completeReplyProcessing() -> std::optional<ReplyRequest>;

    private:
        std::mutex m_mutex;
        std::shared_ptr<MessageList> m_messageList;
        std::queue<json> m_pendingPreparationMessages;
        bool m_isPreparationRunning{false};
        bool m_isReplyProcessing{false};
        std::optional<ReplyRequest> m_pendingReply;
    };
} // namespace insoulforge
