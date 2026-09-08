/// @file Logger.cpp
/// @brief 日志系统生命周期 - 实现
/// @author donghao
/// @date 2026-08-22

#include <filesystem>
#include <memory>
#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>
#include <string_view>
#include <infrastructure/logging/LogBuffer.hpp>
#include <infrastructure/logging/LogSink.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <vector>

namespace insoulforge {
    namespace {
        constexpr std::string_view kLogDir = "logs";
        constexpr std::string_view kLogFile = "logs/bot.log";
        constexpr size_t kLogFileMaxSize = 10 * 1024 * 1024; // 单文件 10MB
        constexpr size_t kLogFileMaxCount = 5; // 保留 5 个历史文件
    } // namespace

    void Logger::init() {
        const auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
        consoleSink->set_pattern("[%H:%M:%S] [%^%l%$] %v");

        const auto logSink = std::make_shared<LogSink>();
        std::vector<spdlog::sink_ptr> sinks{consoleSink, logSink};
        try {
            std::filesystem::create_directories(kLogDir);
            const auto fileSink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
              kLogFile.data(), kLogFileMaxSize, kLogFileMaxCount);
            fileSink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
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
        spdlog::info("日志系统初始化完成 (文件={})", sinks.size() > 2);
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

    SessionLogger Logger::session(const uint64_t sessionId) { return SessionLogger(sessionId); }

    void Logger::shutdown() { spdlog::shutdown(); }
} // namespace insoulforge
