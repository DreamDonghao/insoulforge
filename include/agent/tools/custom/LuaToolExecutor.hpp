/// @file LuaToolExecutor.hpp
/// @brief 自定义 Lua 工具执行与宿主操作桥接

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <drogon/utils/coroutine.h>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::LuaToolExecutor {
    /// @brief 在受限运行环境中检查脚本，并确认存在 run 入口
    [[nodiscard]] auto validate(std::string_view source) -> std::optional<std::string>;

    /// @brief 执行 run 或 background；测试模式模拟有副作用的宿主操作
    /// @throws std::runtime_error 脚本错误、超限或返回值无效
    [[nodiscard]] auto execute(std::string source, json args, u64 sessionId, bool background = false,
      bool testMode = false) -> drogon::Task<std::string>;
} // namespace insoulforge::LuaToolExecutor
