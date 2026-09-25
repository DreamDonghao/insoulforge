/// @file ConfigStore.hpp
/// @brief 全局配置文件存储
/// @author donghao
/// @date 2026-08-30
/// @details 配置统一保存在运行目录的 data/config.json。文件缺失时自动创建，
///          缺失或类型不匹配的字段会按默认值修复后写回。

#pragma once
#include <string>

#include <infrastructure/JsonUtil.hpp>


/// @brief 全局配置文件读写接口
namespace insoulforge::ConfigStore {
    /// @brief 初始化配置文件。
    /// @param path 配置文件路径，默认使用 data/config.json。
    /// @throws std::runtime_error 目录创建、读取或原子写入失败时抛出。
    void initialize(const std::string &path = "data/config.json");

    // ==================== LLM 配置 ====================

    [[nodiscard]] auto getLLMConfig(const std::string &name) -> json;

    void saveLLMConfig(const std::string &name, const json &config);

    [[nodiscard]] auto getAllLLMConfigs() -> json;

    // ==================== QQ Bot / 记忆配置 ====================

    [[nodiscard]] auto getQQConfig() -> json;

    void saveQQConfig(const json &config);

    [[nodiscard]] auto getMemoryConfig() -> json;

    void saveMemoryConfig(const json &config);

} // namespace insoulforge::ConfigStore
