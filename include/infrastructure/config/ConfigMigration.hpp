/// @file ConfigMigration.hpp
/// @brief 旧配置文件到当前结构的启动期转换

#pragma once

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::ConfigStore {
    /// @brief 转换旧字段；返回是否需要持久化。仅在配置文件加载时调用。
    auto migrateConfigFile(json &config, const json &defaults) -> bool;
} // namespace insoulforge::ConfigStore
