/// @file AgentSystem.hpp
/// @brief Agent 系统运行状态与公共依赖初始化

#pragma once
#include <atomic>

namespace insoulforge {
    /// @brief Agent 系统单例类
    /// @details 消息处理流程由 OneBotEventWorkflow 协调；本类仅管理其所依赖的全局初始化状态。
    class AgentSystem {
    public:
        static AgentSystem &instance();

        [[nodiscard]] bool isRunning() const noexcept;

        /// @brief 判断 Agent 是否已完成初始化且允许处理消息
        [[nodiscard]] bool isReady() const noexcept;

        /// @brief 设置 Agent 的运行开关
        void setRunning(bool running) noexcept;

        /// @brief 初始化工具运行时与提示词服务
        void initialize();

    private:
        AgentSystem() = default;

        std::atomic_bool m_initialized{false};
        std::atomic_bool m_running{true};
    };
} // namespace insoulforge
