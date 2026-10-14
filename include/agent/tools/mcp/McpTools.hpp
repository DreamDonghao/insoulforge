#pragma once

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <drogon/utils/coroutine.h>
#include <expected>

#include <agent/tools/mcp/McpToolSelection.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/config/ConfigError.hpp>
#include <infrastructure/mcp/Client.hpp>

namespace insoulforge {
    class ToolRegistry;
    class Config;

    /// @brief 将远程 MCP 工具接入本地工具注册表。
    /// @note 启动时配置，执行阶段按服务异步发现并缓存；不跨事件循环共享 Client。
    class McpTools {
    public:
        static auto instance() -> McpTools &;
        /// @brief 校验服务配置并保存；没有服务时不影响原有工具。
        /// @pre 仅在启动期调用，registry 必须存活到全部工具调用结束。
        [[nodiscard]] auto initialize(json config, ToolRegistry &registry) -> std::expected<void, std::string>;
        /// @brief 先校验并持久化，再动态替换服务；文件保存失败保留现有工具。
        [[nodiscard]] auto saveConfig(json config, Config &store) -> std::expected<void, ConfigError>;
        /// @brief 清除发现缓存并重新查询；保留旧工具直到刷新成功。
        auto refreshTools() -> drogon::Task<>;
        /// @brief 返回加载状态、工具数量和最近错误，不包含认证令牌。
        [[nodiscard]] auto status() -> json;
        /// @brief 固定本次回复的目录、连接身份与数量限制，不进行网络请求。
        [[nodiscard]] auto createSelection(const json &executionConfig) -> std::shared_ptr<McpToolSelection>;
        [[nodiscard]] auto directory() -> json;
        /// @brief 仅查询选定服务，返回分页摘要，不包含参数 Schema。
        auto listTools(json arguments, std::shared_ptr<McpToolSelection> selection) -> drogon::Task<std::string>;
        /// @brief 整批校验后更新当前回复的选择状态，未知名称或超限不部分启用。
        auto enableTools(const json &arguments, McpToolSelection &selection) -> std::string;
        /// @brief 首次使用时发现工具；失败服务最多每 60 秒尝试一次。
        /// @note 同一服务正在加载时其他会话不等待，本轮使用已有工具。
        auto ensureTools() -> drogon::Task<>;

    private:
        struct Catalog {
            u64 version;
            json tools;
        };
        struct Server {
            std::string name;
            std::string description;
            drogonmcp::ClientOptions options;
            bool loaded = false;
            bool loading = false;
            std::chrono::steady_clock::time_point retryAfter{};
            u64 generation = 0;
            std::shared_ptr<const Catalog> catalog;
            std::string error;
        };
        static auto parseConfig(const json &config) -> std::expected<std::vector<std::shared_ptr<Server>>, std::string>;
        void applyServers(std::vector<std::shared_ptr<Server>> servers);
        auto discover(std::shared_ptr<Server> server) -> drogon::Task<std::expected<void, std::string>>;
        auto loadServer(std::shared_ptr<Server> server) -> drogon::Task<std::expected<void, std::string>>;
        auto registerManagementTools() -> bool;
        auto directoryLocked() const -> json;
        std::vector<std::shared_ptr<Server>> m_servers;
        std::optional<std::reference_wrapper<ToolRegistry>> m_registry; ///< 非拥有依赖，启动时显式传入。
        std::mutex m_mutex; ///< 只保护加载状态，不持锁等待网络。
        std::mutex m_configurationMutex; ///< 串行化保存与生效，不持锁等待网络。
        u64 m_nextVersion = 1; ///< 连接和工具缓存共用递增标识，不持久化。
    };
} // namespace insoulforge
