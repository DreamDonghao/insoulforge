/// @file AgentSystem.hpp
/// @brief Agent 系统运行状态与公共依赖初始化

#pragma once
#include <atomic>
#include <string>
#include <string_view>

#include <expected>

namespace insoulforge {
    class Database;
    /// @brief Agent 系统单例类
    /// @details 消息处理流程由 EventWorkflow 协调；本类负责公共依赖初始化与运行开关。
    class AgentSystem {
    public:
        static auto instance() -> AgentSystem &;

        [[nodiscard]] auto isRunning() const noexcept -> bool;

        /// @brief 设置 Agent 的运行开关
        void setRunning(bool running) noexcept;

        /// @brief 初始化工具运行时与提示词服务
        /// @return 依赖初始化失败时返回原因。
        /// @param database 已成功打开并完成迁移的数据库。
        /// @param botName 已加载的机器人名称，用于生成工具参数说明。
        [[nodiscard]] auto initialize(const Database &database, std::string_view botName)
          -> std::expected<void, std::string>;

    private:
        AgentSystem() = default;

        std::atomic_bool m_running{true};
    };
} // namespace insoulforge
