/// @file Logger.hpp
/// @brief 统一运行日志入口

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <expected>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>

namespace insoulforge {
    /// @brief 负责控制台、滚动文件、内存查询和后台实时推送的全局日志系统
    /// @details 每条日志均由等级、会话 ID、来源和内容组成。系统级日志不关联会话，以空值表示。
    class Logger {
    public:
        enum class Level { Trace, Debug, Info, Warn, Error, Critical };

        /// @brief 初始化日志输出与历史日志缓冲区
        /// @return 日志输出初始化失败时返回原因；文件日志不可用时降级为控制台输出并成功返回。
        [[nodiscard]] static auto initialize() -> std::expected<void, std::string>;

        /// @brief 写入 trace 级别日志
        static void trace(std::optional<SessionId> sessionId, std::string_view source, std::string content);

        /// @brief 写入 debug 级别日志
        static void debug(std::optional<SessionId> sessionId, std::string_view source, std::string content);

        /// @brief 写入 info 级别日志
        static void info(std::optional<SessionId> sessionId, std::string_view source, std::string content);

        /// @brief 写入 warn 级别日志
        static void warn(std::optional<SessionId> sessionId, std::string_view source, std::string content);

        /// @brief 写入 error 级别日志
        static void error(std::optional<SessionId> sessionId, std::string_view source, std::string content);

        /// @brief 写入 critical 级别日志
        static void critical(std::optional<SessionId> sessionId, std::string_view source, std::string content);

        /// @brief 设置运行时日志等级
        /// @return 等级名称有效时返回 true
        static auto setLevel(std::string_view levelName) -> bool;

        /// @brief 获取当前运行时日志等级
        static auto level() -> std::string;

        /// @brief 连接恢复或主动停止时重置底层连接拒绝警告窗口，使下次离线首次警告可见。
        static void resetConnectionRefusedWarnings();

        /// @brief 刷新并关闭日志系统，应在程序退出前调用
        static void shutdown();

    private:
        static void write(
          Level level, std::optional<SessionId> sessionId, std::string_view source, std::string content);
    };
} // namespace insoulforge
