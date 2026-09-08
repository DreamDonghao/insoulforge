/// @file ChatRecordManager.hpp
/// @brief Agent 会话记录快照适配器

#pragma once
#include <deque>
#include <infrastructure/JsonUtil.hpp>

namespace insoulforge {
    /// @brief 提示词中完整保留的最近记录条数（更早记录简化处理；召回缓存按此长度对齐淘汰）
    inline constexpr size_t kRecentRecordCount = 12;

    /// @brief Agent 会话记录快照
    /// @details 封装某次 Agent 请求可见的完整消息快照，避免后到消息改变正在执行的上下文。
    class ChatRecordManager {
    public:
        /// @brief 使用已冻结的会话记录创建快照
        /// @param sessionId 会话 ID
        /// @param records 当前 Agent 任务可见的记录快照（旧→新）
        ChatRecordManager(uint64_t sessionId, std::deque<json> records);

        /// @brief 获取会话 ID
        [[nodiscard]] uint64_t getSessionId() const;

        /// @brief 获取冻结的聊天记录（旧→新）
        /// @return 聊天记录队列
        [[nodiscard]] std::deque<json> getRecords() const;

    private:
        uint64_t m_sessionId; ///< 所属会话 ID
        std::deque<json> m_records; ///< Agent 上下文快照
    };
} // namespace insoulforge
