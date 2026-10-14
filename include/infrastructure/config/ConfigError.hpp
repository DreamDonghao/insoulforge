/// @file ConfigError.hpp
/// @brief 配置读取、转换和保存的错误信息。
#pragma once

#include <string>

namespace insoulforge {
    /// @brief 区分配置无效、文件操作失败和并发保存冲突。
    enum class ConfigErrorType { InvalidArgument, FileOperation, Conflict };

    /// @brief 配置操作失败的类别与可供日志、管理接口使用的具体原因。
    struct ConfigError {
        ConfigErrorType type;
        std::string message;
    };
} // namespace insoulforge
