/// @file MemoryManager.hpp
/// @brief 读取单个会话的外号与待迁移历史内容
/// @details Agent 构建上下文时只投影 aliases，历史内容留待维护任务迁移。

#pragma once

#include <string>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 指向单个会话外号存储的读取适配器
    class MemoryManager {
    public:
        /// @brief 绑定会话 ID
        /// @param sessionId 会话 ID（私聊会话带标志位）
        explicit MemoryManager(u64 sessionId);

        /// @brief 获取原始会话记忆
        /// @return 外号 JSON 字符串，可能含尚未迁移的旧字段或旧纯文本
        [[nodiscard]] auto getMemory() const -> std::string;

    private:
        u64 m_sessionId; ///< 群号或带私聊标志位的用户 QQ 号
    };
} // namespace insoulforge
