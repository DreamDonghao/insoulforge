/// @file ConfigStore.hpp
/// @brief 全局配置文件存储
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

    /// @brief 获取指定角色的模型配置；角色不存在时返回 null JSON
    [[nodiscard]] auto getLLMConfig(const std::string &name) -> json;

    /// @brief 保存指定角色的模型配置，并兼容旧字段 top_P
    void saveLLMConfig(const std::string &name, const json &config);

    /// @brief 获取全部模型配置的独立副本
    [[nodiscard]] auto getAllLLMConfigs() -> json;

    /// @brief 获取 OneBot 与机器人身份配置
    [[nodiscard]] auto getQQConfig() -> json;

    /// @brief 保存 OneBot 与机器人身份配置
    void saveQQConfig(const json &config);

    /// @brief 获取会话窗口和记忆处理配置
    [[nodiscard]] auto getMemoryConfig() -> json;

    /// @brief 保存会话窗口和记忆处理配置
    void saveMemoryConfig(const json &config);

} // namespace insoulforge::ConfigStore
