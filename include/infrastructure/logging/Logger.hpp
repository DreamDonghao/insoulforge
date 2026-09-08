/// @file Logger.hpp
/// @brief 日志系统生命周期 - 基于 spdlog（控制台 + 滚动文件）
/// @author donghao
/// @date 2026-08-22

#pragma once

#include <cstdint>
#include <fmt/format.h>
#include <conversation/message/SessionId.hpp>
#include <spdlog/spdlog.h>
#include <string_view>
#include <utility>

namespace insoulforge {
    class SessionLogger {
    public:
        explicit SessionLogger(uint64_t sessionId) : m_sessionId(sessionId) {}

        template<typename... Args>
        void trace(fmt::format_string<Args...> format, Args &&...args) const {
            write(spdlog::level::trace, format, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void debug(fmt::format_string<Args...> format, Args &&...args) const {
            write(spdlog::level::debug, format, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void info(fmt::format_string<Args...> format, Args &&...args) const {
            write(spdlog::level::info, format, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void warn(fmt::format_string<Args...> format, Args &&...args) const {
            write(spdlog::level::warn, format, std::forward<Args>(args)...);
        }

        template<typename... Args>
        void error(fmt::format_string<Args...> format, Args &&...args) const {
            write(spdlog::level::err, format, std::forward<Args>(args)...);
        }

    private:
        template<typename... Args>
        void write(spdlog::level::level_enum level, fmt::format_string<Args...> format, Args &&...args) const {
            // 私聊会话用可读的 QQ 号展示；LogBuffer 按 private_id 前缀还原为完整会话 ID
            if (SessionId::isPrivate(m_sessionId)) {
                spdlog::log(level, "[private_id={}] {}", SessionId::privateUserId(m_sessionId),
                  fmt::format(format, std::forward<Args>(args)...));
            } else {
                spdlog::log(level, "[group_id={}] {}", m_sessionId, fmt::format(format, std::forward<Args>(args)...));
            }
        }

        uint64_t m_sessionId;
    };

    /// @brief 日志系统生命周期管理
    class Logger {
    public:
        /// @brief 初始化默认 logger，应在 main() 最开始调用
        /// @details 配置控制台（彩色）与滚动文件（10MB x 5）双 sink；
        ///          文件日志不可用（如目录创建失败）时自动降级为仅控制台
        static void init();

        /// @brief 设置运行时日志等级
        /// @return 等级名称有效时返回 true
        static bool setLevel(std::string_view levelName);

        /// @brief 获取当前运行时日志等级
        static std::string level();

        /// @brief 创建带群聊上下文的日志记录器
        static SessionLogger session(uint64_t sessionId);

        /// @brief 刷新并关闭日志系统，应在程序退出前调用
        static void shutdown();
    };
} // namespace insoulforge
