/// @file ConfigStore.hpp
/// @brief 全局配置文件存储
/// @details 配置统一保存在运行目录的 data/config.json。文件缺失时自动创建，
///          缺失或类型不匹配的字段会按默认值修复后写回。

#pragma once
#include <functional>
#include <string>
#include <string_view>

#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/config/ConfigError.hpp>


/// @brief 配置系统内部的文件读写接口，业务代码通过 Config 访问
namespace insoulforge::ConfigStore {
    /// @brief 在文件提交前完成运行配置转换，失败时不写入或替换存储状态。
    using ConfigValidator = std::function<std::expected<void, ConfigError>(const json &)>;

    /// @brief 初始化配置文件。
    /// @param path 由调用方明确指定的配置文件路径。
    /// @return 文件操作失败时返回具体错误，不替换已有内存状态。
    [[nodiscard]] auto initialize(std::string_view path, const ConfigValidator &validate)
      -> std::expected<void, ConfigError>;

    // 下列读写接口均要求 initialize 已成功；读取返回独立副本，保存失败不修改内存状态。

    /// @brief 获取指定角色的模型配置；角色不存在时返回 null JSON
    [[nodiscard]] auto getLLMConfig(const std::string &name) -> json;

    /// @brief 保存指定角色的模型配置，并兼容旧字段 top_P
    [[nodiscard]] auto saveLLMConfig(const std::string &name, const json &config, const ConfigValidator &validate)
      -> std::expected<void, ConfigError>;

    /// @brief 获取全部模型配置的独立副本
    [[nodiscard]] auto getAllLLMConfigs() -> json;

    /// @brief 获取 OneBot 与机器人身份配置
    [[nodiscard]] auto getQQConfig() -> json;

    /// @brief 保存 OneBot 与机器人身份配置
    [[nodiscard]] auto saveQQConfig(const json &config, const ConfigValidator &validate)
      -> std::expected<void, ConfigError>;

    /// @brief 获取会话窗口和记忆处理配置
    [[nodiscard]] auto getMemoryConfig() -> json;

    /// @brief 保存会话窗口和记忆处理配置
    [[nodiscard]] auto saveMemoryConfig(const json &config, const ConfigValidator &validate)
      -> std::expected<void, ConfigError>;

    /// @brief 获取独立的执行流程配置。
    [[nodiscard]] auto getExecutionConfig() -> json;

    /// @brief 保存执行配置；运行时更新由 Config 完成。
    /// @return 参数无效或文件写入失败时返回具体错误。
    [[nodiscard]] auto saveExecutionConfig(const json &config, const ConfigValidator &validate)
      -> std::expected<void, ConfigError>;

} // namespace insoulforge::ConfigStore
