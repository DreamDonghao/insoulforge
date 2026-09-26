/// @file ChatRecordManager.hpp
/// @brief Agent 会话记录快照适配器

#pragma once

#include <deque>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief Agent 上下文中完整保留的最近记录条数；更早的记录会截短文本
    inline constexpr size_t kRecentRecordCount = 12;

    /// @brief Agent 会话记录快照
    /// @details 封装某次 Agent 请求可见的完整消息快照，避免后到消息改变正在执行的上下文。
    class ChatRecordManager {
    public:
        /// @brief 使用已冻结的会话记录创建快照
        /// @param sessionId 会话 ID
        /// @param records 当前 Agent 任务可见的记录快照（旧→新）
        ChatRecordManager(u64 sessionId, std::deque<json> records);

        /// @brief 获取会话 ID
        [[nodiscard]] auto getSessionId() const -> u64;

        /// @brief 获取冻结的聊天记录（旧→新）
        /// @return 独立的记录副本，按时间从早到晚排列
        [[nodiscard]] auto getRecords() const -> std::deque<json>;

    private:
        u64 m_sessionId; ///< 所属会话 ID
        std::deque<json> m_records; ///< Agent 上下文快照
    };
} // namespace insoulforge
