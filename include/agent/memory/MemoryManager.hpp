/// @file MemoryManager.hpp
/// @brief 读取单个会话的短期记忆
/// @details 从 MemoryStore 读取短期记忆，供 Agent 构建上下文使用。

#pragma once

#include <infrastructure/NumericTypes.hpp>
#include <string>

namespace insoulforge {
    /// @brief 指向单个会话短期记忆的读取适配器
    class MemoryManager {
    public:
        /// @brief 绑定会话 ID
        /// @param sessionId 会话 ID（私聊会话带标志位）
        explicit MemoryManager(u64 sessionId);

        /// @brief 获取短期记忆
        /// @return 记忆内容（每行一条）
        [[nodiscard]] auto getMemory() const -> std::string;

    private:
        u64 m_sessionId; ///< 群号或带私聊标志位的用户 QQ 号
    };
} // namespace insoulforge
