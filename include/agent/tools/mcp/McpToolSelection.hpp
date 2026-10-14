#pragma once

#include <map>
#include <string>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 一次回复固定的 MCP 数量限制；不读取运行中变化的配置。
    struct McpToolLimits {
        i32 pageSize = 10;
        i32 maxPerEnable = 4;
        i32 maxPerReply = 12;
    };

    /// @brief 仅属于一次回复的选择状态，由 Executor 持有，不写入会话或全局缓存。
    /// @note 选择操作串行执行；服务和目录快照不可变，enabled 仅由启用工具更新。
    struct McpToolSelection {
        McpToolLimits limits;
        json directory = json::array();
        std::map<std::string, u64> services; ///< 服务名称到连接配置版本，防止复用已删除服务的名称。
        std::map<std::string, u64> enabled; ///< 本地工具名称到工具定义版本。
    };
} // namespace insoulforge
