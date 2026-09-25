/// @file MemoryManager.hpp
/// @brief 短期记忆管理器 - 群聊记忆存储与检索
/// @author donghao
/// @date 2026-04-02
/// @details 管理单个群组的短期记忆，支持存储、更新和检索操作。
///          短期记忆以纯文本形式存储，每行一条记忆条目。

#pragma once

#include <infrastructure/NumericTypes.hpp>
#include <string>

namespace insoulforge {
    /// @brief 短期记忆管理类
    /// @details 管理单个群组的短期记忆，每行一条记忆条目。
    ///          用于 LLM 上下文中提供记忆信息。
    class MemoryManager {
    public:
        /// @brief 构造函数
        /// @param sessionId 会话 ID（私聊会话带标志位）
        explicit MemoryManager(u64 sessionId);

        /// @brief 获取短期记忆
        /// @return 记忆内容（每行一条）
        [[nodiscard]] auto getMemory() const -> std::string;

    private:
        u64 m_sessionId; ///< 群号
    };
} // namespace insoulforge
