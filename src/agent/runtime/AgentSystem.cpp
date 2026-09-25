/// @file AgentSystem.cpp
/// @brief Agent 系统运行状态与公共依赖初始化实现

#include <agent/runtime/AgentSystem.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <llm/PromptService.hpp>

namespace insoulforge {
    auto AgentSystem::instance() -> AgentSystem & {
        static AgentSystem system;
        return system;
    }

    auto AgentSystem::isRunning() const noexcept -> bool { return m_running.load(std::memory_order_acquire); }

    auto AgentSystem::isReady() const noexcept -> bool {
        return m_initialized.load(std::memory_order_acquire) && isRunning();
    }

    void AgentSystem::setRunning(const bool running) noexcept { m_running.store(running, std::memory_order_release); }

    void AgentSystem::initialize() {
        ToolRuntime::registerBuiltinTools();
        ToolRuntime::reloadCustomTools();
        PromptService::initialize();
        m_initialized.store(true, std::memory_order_release);
    }
} // namespace insoulforge
