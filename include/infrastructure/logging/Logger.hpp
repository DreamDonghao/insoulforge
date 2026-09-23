/// @file Logger.hpp
/// @brief 统一运行日志入口

#pragma once

#include <infrastructure/NumericTypes.hpp>
#include <string>
#include <string_view>

namespace insoulforge {
    /// @brief 负责控制台、滚动文件、内存查询和后台实时推送的全局日志系统
    /// @details 每条日志均由等级、会话 ID、来源和内容组成。系统级日志的会话 ID 固定为 0。
    class Logger {
    public:
        enum class Level { Trace, Debug, Info, Warn, Error, Critical };

        /// @brief 初始化日志输出与历史日志缓冲区
        static void init();

        /// @brief 写入 trace 级别日志
        static void trace(u64 sessionId, std::string_view source, std::string content);

        /// @brief 写入 debug 级别日志
        static void debug(u64 sessionId, std::string_view source, std::string content);

        /// @brief 写入 info 级别日志
        static void info(u64 sessionId, std::string_view source, std::string content);

        /// @brief 写入 warn 级别日志
        static void warn(u64 sessionId, std::string_view source, std::string content);

        /// @brief 写入 error 级别日志
        static void error(u64 sessionId, std::string_view source, std::string content);

        /// @brief 写入 critical 级别日志
        static void critical(u64 sessionId, std::string_view source, std::string content);

        /// @brief 设置运行时日志等级
        /// @return 等级名称有效时返回 true
        static bool setLevel(std::string_view levelName);

        /// @brief 获取当前运行时日志等级
        static std::string level();

        /// @brief 刷新并关闭日志系统，应在程序退出前调用
        static void shutdown();

    private:
        static void write(Level level, u64 sessionId, std::string_view source, std::string content);
    };
} // namespace insoulforge
