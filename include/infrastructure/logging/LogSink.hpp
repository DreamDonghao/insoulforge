/// @file LogSink.hpp
/// @brief 将 spdlog 日志转发到内存查询服务

#pragma once

#include <mutex>
#include <spdlog/sinks/base_sink.h>

namespace insoulforge {
    class LogSink final : public spdlog::sinks::base_sink<std::mutex> {
    protected:
        void sink_it_(const spdlog::details::log_msg &message) override;

        void flush_() override;
    };
}