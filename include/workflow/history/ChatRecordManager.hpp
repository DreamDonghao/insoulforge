/// @file ChatRecordManager.hpp
/// @brief Agent 会话记录快照适配器

#pragma once

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
        /// @param records 当前 Agent 任务可见的完整消息数组（旧→新），按值接收以独立持有快照。
        ChatRecordManager(u64 sessionId, json records);

        /// @brief 获取会话 ID
        [[nodiscard]] auto getSessionId() const -> u64;

        /// @brief 获取冻结的聊天记录（旧→新）
        /// @return 按时间从早到晚排列的只读引用，仅在本快照对象存活且未被移动期间有效。
        [[nodiscard]] auto getRecords() const -> const json &;

    private:
        u64 m_sessionId; ///< 所属会话 ID
        json m_records; ///< Agent 上下文快照，不包装为持久化记录的 content 字符串
    };
} // namespace insoulforge
