/// @file AgentSystem.cpp
/// @brief Agent 系统运行状态与公共依赖初始化实现

#include <agent/runtime/AgentSystem.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <llm/PromptService.hpp>

namespace insoulforge {
    AgentSystem &AgentSystem::instance() {
        static AgentSystem system;
        return system;
    }

    bool AgentSystem::isRunning() const noexcept { return m_running.load(std::memory_order_acquire); }

    bool AgentSystem::isReady() const noexcept { return m_initialized.load(std::memory_order_acquire) && isRunning(); }

    void AgentSystem::setRunning(const bool running) noexcept { m_running.store(running, std::memory_order_release); }

    void AgentSystem::initialize() {
        ToolRuntime::registerBuiltinTools();
        ToolRuntime::reloadCustomTools();
        PromptService::initialize();
        m_initialized.store(true, std::memory_order_release);
    }
} // namespace insoulforge
