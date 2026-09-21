/// @file Logger.cpp
/// @brief 统一运行日志入口 - 实现

#include <admin/realtime/LogWebSocketManager.hpp>
#include <algorithm>
#include <filesystem>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <infrastructure/logging/LogBuffer.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace insoulforge {
    namespace {
        constexpr std::string_view kLogDir = "logs";
        constexpr std::string_view kLogFile = "logs/bot.log";
        constexpr size_t kLogFileMaxSize = 10 * 1024 * 1024;
        constexpr size_t kLogFileMaxCount = 5;

        [[nodiscard]] spdlog::level::level_enum toSpdlogLevel(const Logger::Level level) {
            switch (level) {
                case Logger::Level::Trace:
                    return spdlog::level::trace;
                case Logger::Level::Debug:
                    return spdlog::level::debug;
                case Logger::Level::Info:
                    return spdlog::level::info;
                case Logger::Level::Warn:
                    return spdlog::level::warn;
                case Logger::Level::Error:
                    return spdlog::level::err;
                case Logger::Level::Critical:
                    return spdlog::level::critical;
            }
            return spdlog::level::info;
        }

        [[nodiscard]] std::string_view toLevelName(const Logger::Level level) {
            switch (level) {
                case Logger::Level::Trace:
                    return "trace";
                case Logger::Level::Debug:
                    return "debug";
                case Logger::Level::Info:
                    return "info";
                case Logger::Level::Warn:
                    return "warn";
                case Logger::Level::Error:
                    return "error";
                case Logger::Level::Critical:
                    return "critical";
            }
            return "info";
        }

        [[nodiscard]] std::string formatTimestamp(const spdlog::log_clock::time_point timestamp) {
            const auto time = spdlog::log_clock::to_time_t(timestamp);
            std::tm localTime{};
#ifdef _WIN32
            localtime_s(&localTime, &time);
#else
            localtime_r(&time, &localTime);
#endif
            const auto milliseconds =
              std::chrono::duration_cast<std::chrono::milliseconds>(timestamp.time_since_epoch()).count() % 1000;
            return fmt::format("{:%Y-%m-%d %H:%M:%S}.{:03d}", localTime, milliseconds);
        }
    } // namespace

    void Logger::init() {
        const auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        // 等级已包含在结构化正文中；只用颜色标记为控制台整条日志着色，文件和 Web 保持纯文本。
        consoleSink->set_pattern("%^[%H:%M:%S] %v%$");

        std::vector<spdlog::sink_ptr> sinks{consoleSink};
        try {
            std::filesystem::create_directories(kLogDir);
            const auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
              kLogFile.data(), kLogFileMaxSize, kLogFileMaxCount);
            fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] %v");
            sinks.push_back(fileSink);
        } catch (const std::exception &e) {
            fmt::print(stderr, "警告: 文件日志不可用 ({})，仅输出到控制台\n", e.what());
        }

        spdlog::init_thread_pool(8192, 1);
        const auto logger = std::make_shared<spdlog::async_logger>(
          "main", sinks.begin(), sinks.end(), spdlog::thread_pool(), spdlog::async_overflow_policy::block);
        logger->set_level(spdlog::level::trace);
        logger->flush_on(spdlog::level::warn);
        spdlog::set_default_logger(logger);

        LogBuffer::instance().loadFromDirectory(kLogDir.data());
        info(0, "Logger", fmt::format("日志系统初始化完成 | file={}", sinks.size() > 1));
    }

    void Logger::trace(const uint64_t sessionId, const std::string_view source, std::string content) {
        write(Level::Trace, sessionId, source, std::move(content));
    }

    void Logger::debug(const uint64_t sessionId, const std::string_view source, std::string content) {
        write(Level::Debug, sessionId, source, std::move(content));
    }

    void Logger::info(const uint64_t sessionId, const std::string_view source, std::string content) {
        write(Level::Info, sessionId, source, std::move(content));
    }

    void Logger::warn(const uint64_t sessionId, const std::string_view source, std::string content) {
        write(Level::Warn, sessionId, source, std::move(content));
    }

    void Logger::error(const uint64_t sessionId, const std::string_view source, std::string content) {
        write(Level::Error, sessionId, source, std::move(content));
    }

    void Logger::critical(const uint64_t sessionId, const std::string_view source, std::string content) {
        write(Level::Critical, sessionId, source, std::move(content));
    }

    bool Logger::setLevel(const std::string_view levelName) {
        const auto level = spdlog::level::from_str(std::string(levelName));
        if (level == spdlog::level::off && levelName != "off") {
            return false;
        }
        spdlog::default_logger()->set_level(level);
        return true;
    }

    std::string Logger::level() {
        const auto level = spdlog::level::to_string_view(spdlog::default_logger()->level());
        return {level.data(), level.size()};
    }

    void Logger::write(
      const Level level, const uint64_t sessionId, const std::string_view source, std::string content) {
        const auto spdlogLevel = toSpdlogLevel(level);
        if (!spdlog::default_logger()->should_log(spdlogLevel)) {
            return;
        }
        std::string sourceName(source);
        if (sourceName.size() >= 2 && sourceName.front() == '[' && sourceName.back() == ']') {
            sourceName = sourceName.substr(1, sourceName.size() - 2);
        }
        std::ranges::replace(content, '\r', ' ');
        size_t newline = 0;
        while ((newline = content.find('\n', newline)) != std::string::npos) {
            content.replace(newline, 1, "\\n");
            newline += 2;
        }
        const auto timestamp = spdlog::log_clock::now();
        LogEntry entry{.timestamp = formatTimestamp(timestamp),
          .level = std::string(toLevelName(level)),
          .sessionId = sessionId,
          .source = std::move(sourceName),
          .content = std::move(content)};
        const std::string line =
          fmt::format("[{}][{}][{}][{}]", entry.level, entry.sessionId, entry.source, entry.content);

        spdlog::log(spdlogLevel, "{}", line);
        const LogEntry stored = LogBuffer::instance().append(std::move(entry));
        LogWebSocketManager::instance().pushLog(stored);
    }

    void Logger::shutdown() { spdlog::shutdown(); }
} // namespace insoulforge
