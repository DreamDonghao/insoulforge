/// @file AgentSystem.cpp
/// @brief Agent 系统运行状态与公共依赖初始化实现

#include <agent/runtime/AgentSystem.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <llm/prompts/PromptService.hpp>

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

    auto AgentSystem::initialize(const Database &database, const std::string_view botName)
      -> std::expected<void, std::string> {
        m_initialized.store(false, std::memory_order_release);
        if (!database.handle()) {
            return std::unexpected("Agent 初始化要求数据库已成功初始化");
        }
        try {
            return ToolRuntime::registerBuiltinTools(botName)
              .and_then(
                [&database] -> std::expected<void, std::string> { return ToolRuntime::reloadCustomTools(database); })
              .transform([this, &database] -> void {
                  PromptService::initialize(database);
                  m_initialized.store(true, std::memory_order_release);
              });
        } catch (const DbError &error) {
            return std::unexpected(std::string("Agent 初始化失败: ") + error.what());
        }
    }
} // namespace insoulforge
