/// @file Config.cpp
/// @brief 全局配置管理 - 实现

#include <array>
#include <cmath>
#include <limits>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {

        /// @brief 检查已知字段，避免类型转换静默截断小数、溢出或使用空字符串。
        auto validateFields(const json &value) -> std::expected<void, ConfigError> {
            if (!value.is_object()) {
                return std::unexpected(ConfigError{ConfigErrorType::InvalidArgument, "配置必须是 JSON 对象"});
            }
            constexpr std::array integerFields{"maxTokens", "contextWindowLimit", "memorySummaryTriggerCount",
              "memorySummaryBatchSize", "memorySummaryContextCount", "routerWindowTriggerCount",
              "routerWindowKeepCount", "shortTermMemoryMax", "maxToolRounds"};
            constexpr std::array floatFields{
              "temperature", "topP", "top_P", "minConfidence", "longTermRecallThreshold", "longTermInjectThreshold"};
            constexpr std::array stringFields{"apiKey", "baseUrl", "path", "model", "reasoningEffort", "accessToken",
              "oneBotTransport", "qqHttpHost", "qqWebSocketHost", "botName"};
            for (const auto &[key, field]: value.items()) {
                if (std::ranges::find(integerFields, key) != integerFields.end()) {
                    if (!field.is_number_integer() ||
                        (field.is_number_unsigned()
                            ? field.get<u64>() > static_cast<u64>(std::numeric_limits<i32>::max())
                            : field.get<i64>() < std::numeric_limits<i32>::min() ||
                                field.get<i64>() > std::numeric_limits<i32>::max())) {
                        return std::unexpected(
                          ConfigError{ConfigErrorType::InvalidArgument, key + " 必须是 i32 范围内的整数"});
                    }
                } else if (key == "selfQQNumber") {
                    if (!field.is_number_integer() || (!field.is_number_unsigned() && field.get<i64>() < 0)) {
                        return std::unexpected(
                          ConfigError{ConfigErrorType::InvalidArgument, "selfQQNumber 必须是非负整数"});
                    }
                } else if (std::ranges::find(floatFields, key) != floatFields.end()) {
                    if (!field.is_number() || !std::isfinite(field.get<f64>())) {
                        return std::unexpected(ConfigError{ConfigErrorType::InvalidArgument, key + " 必须是有限数值"});
                    }
                    const auto number = field.get<f64>();
                    if ((key == "temperature" && (number < 0.0 || number > 2.0)) ||
                        ((key == "topP" || key == "top_P" || key == "minConfidence") &&
                          (number < 0.0 || number > 1.0))) {
                        return std::unexpected(ConfigError{ConfigErrorType::InvalidArgument, key + " 超出允许范围"});
                    }
                } else if (std::ranges::find(stringFields, key) != stringFields.end()) {
                    if (!field.is_string()) {
                        return std::unexpected(ConfigError{ConfigErrorType::InvalidArgument, key + " 必须是字符串"});
                    }
                }
            }
            return {};
        }
        /// @brief 从已经校验的模型配置 JSON 加载单个角色。
        /// @param name 配置名（router/executor/executorThinking/image/memory 等）
        /// @param apiConfig 输出的 API 配置
        /// @param modelParams 模型参数（可为 nullptr，表示不加载）
        void loadLLMConfig(const json &llm, const std::string_view name, LLMApiConfig &apiConfig,
          LLMModelParams *modelParams = nullptr) {
            const auto &cfg = llm.at(std::string(name));
            if (cfg.is_null())
                return;

            apiConfig.apiKey = trim(getStr(cfg, "apiKey"));
            apiConfig.baseUrl = trim(getStr(cfg, "baseUrl"));
            apiConfig.path = trim(getStr(cfg, "path"));
            apiConfig.model = trim(getStr(cfg, "model"));
            if (cfg.contains("reasoningEffort")) {
                apiConfig.reasoningEffort = getStr(cfg, "reasoningEffort");
            }

            if (modelParams) {
                modelParams->maxTokens = getInt(cfg, "maxTokens");
                modelParams->temperature = getDouble(cfg, "temperature");
                modelParams->topP = getDouble(cfg, "topP", getDouble(cfg, "top_P"));
            }
        }
    } // namespace

    auto Config::instance() -> Config & {
        static Config config{};
        return config;
    }


    auto Config::loadValues(const json &value) -> std::expected<void, ConfigError> {
        const auto &llm = value.at("llm");
        for (const auto *role:
          {"router", "executor", "executorThinking", "image", "imageGeneration", "memory", "embedding", "jev"}) {
            if (auto result = validateFields(llm.at(role)); !result) {
                return result;
            }
        }
        for (const auto *section: {"qq", "memory", "execution"}) {
            if (auto result = validateFields(value.at(section)); !result) {
                return result;
            }
        }
        loadLLMConfig(llm, "router", router, &routerParams);
        loadLLMConfig(llm, "executor", executor, &executorParams);
        loadLLMConfig(llm, "executorThinking", executorThinking, &executorThinkingParams);
        loadLLMConfig(llm, "image", image, &imageParams);
        loadLLMConfig(llm, "imageGeneration", imageGeneration);
        loadLLMConfig(llm, "memory", memory, &memoryParams);
        loadLLMConfig(llm, "embedding", embedding);
        loadLLMConfig(llm, "jev", jev);
        const f64 configuredJevConfidence = getDouble(llm.at("jev"), "minConfidence", 0.6);
        jevMinConfidence =
          std::isfinite(configuredJevConfidence) && configuredJevConfidence >= 0.0 && configuredJevConfidence <= 1.0
            ? configuredJevConfidence
            : 0.6;

        execution.maxToolRounds.store(
          getInt(value.at("execution"), "maxToolRounds", ExecutionSettings::kDefaultMaxToolRounds),
          std::memory_order_relaxed);

        // 加载记忆配置
        if (const auto memCfg = value.at("memory"); !memCfg.is_null()) {
            contextWindowLimit = getInt(memCfg, "contextWindowLimit");
            memorySummaryTriggerCount = getInt(memCfg, "memorySummaryTriggerCount");
            memorySummaryBatchSize = getInt(memCfg, "memorySummaryBatchSize");
            memorySummaryContextCount = getInt(memCfg, "memorySummaryContextCount");
            routerWindowTriggerCount = getInt(memCfg, "routerWindowTriggerCount");
            routerWindowKeepCount = getInt(memCfg, "routerWindowKeepCount");
            shortTermMemoryMax = getInt(memCfg, "shortTermMemoryMax");
            longTermRecallThreshold = getDouble(memCfg, "longTermRecallThreshold");
            longTermInjectThreshold = getDouble(memCfg, "longTermInjectThreshold");
            if (contextWindowLimit <= 0)
                contextWindowLimit = 100;
            if (memorySummaryTriggerCount <= 0)
                memorySummaryTriggerCount = 100;
            if (memorySummaryBatchSize <= 0 || memorySummaryBatchSize > memorySummaryTriggerCount)
                memorySummaryBatchSize = std::max(memorySummaryTriggerCount / 2, 1);
            if (memorySummaryContextCount < 0)
                memorySummaryContextCount = 0;
            if (routerWindowTriggerCount <= 0)
                routerWindowTriggerCount = 20;
            if (routerWindowKeepCount <= 0 || routerWindowKeepCount >= routerWindowTriggerCount) {
                routerWindowKeepCount = routerWindowTriggerCount / 2;
            }
            if (longTermRecallThreshold <= 0.0 || longTermRecallThreshold >= 1.0)
                longTermRecallThreshold = 0.65;
            if (longTermInjectThreshold <= 0.0 || longTermInjectThreshold >= 1.0)
                longTermInjectThreshold = 0.45;
        }

        // 加载 QQ Bot 配置
        if (const auto qqCfg = value.at("qq"); !qqCfg.is_null()) {
            accessToken = trim(getStr(qqCfg, "accessToken"));
            selfQQNumber = qqCfg.at("selfQQNumber").get<u64>();
            oneBotTransport = getStr(qqCfg, "oneBotTransport", "http");
            qqHttpHost = trim(getStr(qqCfg, "qqHttpHost"));
            qqWebSocketHost = trim(getStr(qqCfg, "qqWebSocketHost"));
            if (qqCfg.contains("botName")) {
                botName = getStr(qqCfg, "botName");
            }
            if (oneBotTransport != "http" && oneBotTransport != "websocket") {
                oneBotTransport = "http";
            }
        }

        return {};
    }


    void Config::applyValues(Config &&value) {
        for (const auto *role:
          {"router", "executor", "executorThinking", "image", "imageGeneration", "memory", "embedding", "jev"}) {
            applyLLMValues(role, std::move(value));
        }
        applyQQValues(std::move(value));
        applyMemoryValues(value);
        execution.maxToolRounds.store(
          value.execution.maxToolRounds.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }

    void Config::applyLLMValues(const std::string_view name, Config &&value) {
        if (name == "router") {
            router = std::move(value.router);
            routerParams = std::move(value.routerParams);
        } else if (name == "executor") {
            executor = std::move(value.executor);
            executorParams = std::move(value.executorParams);
        } else if (name == "executorThinking") {
            executorThinking = std::move(value.executorThinking);
            executorThinkingParams = std::move(value.executorThinkingParams);
        } else if (name == "image") {
            image = std::move(value.image);
            imageParams = std::move(value.imageParams);
        } else if (name == "imageGeneration") {
            imageGeneration = std::move(value.imageGeneration);
        } else if (name == "memory") {
            memory = std::move(value.memory);
            memoryParams = std::move(value.memoryParams);
        } else if (name == "embedding") {
            embedding = std::move(value.embedding);
        } else if (name == "jev") {
            jev = std::move(value.jev);
            jevMinConfidence = value.jevMinConfidence;
        }
    }

    void Config::applyQQValues(Config &&value) {
        accessToken = std::move(value.accessToken);
        selfQQNumber = value.selfQQNumber;
        oneBotTransport = std::move(value.oneBotTransport);
        qqHttpHost = std::move(value.qqHttpHost);
        qqWebSocketHost = std::move(value.qqWebSocketHost);
        botName = std::move(value.botName);
    }

    void Config::applyMemoryValues(const Config &value) {
        contextWindowLimit = value.contextWindowLimit;
        memorySummaryTriggerCount = value.memorySummaryTriggerCount;
        memorySummaryBatchSize = value.memorySummaryBatchSize;
        memorySummaryContextCount = value.memorySummaryContextCount;
        routerWindowTriggerCount = value.routerWindowTriggerCount;
        routerWindowKeepCount = value.routerWindowKeepCount;
        shortTermMemoryMax = value.shortTermMemoryMax;
        longTermRecallThreshold = value.longTermRecallThreshold;
        longTermInjectThreshold = value.longTermInjectThreshold;
    }

    auto Config::initialize(const std::string_view path) -> std::expected<void, ConfigError> {
        std::scoped_lock lock(m_updateMutex);
        Config next;
        return ConfigStore::initialize(path, [&next](const json &value) {
            return next.loadValues(value);
        }).transform([this, &next] {
            applyValues(std::move(next));
            Logger::info(0, "Config", "配置读取和运行时加载完成");
        });
    }

    auto Config::getLLMConfig(const std::string &name) const -> json { return ConfigStore::getLLMConfig(name); }
    auto Config::getAllLLMConfigs() const -> json { return ConfigStore::getAllLLMConfigs(); }
    auto Config::getQQConfig() const -> json { return ConfigStore::getQQConfig(); }
    auto Config::getMemoryConfig() const -> json { return ConfigStore::getMemoryConfig(); }
    auto Config::getExecutionConfig() const -> json { return ConfigStore::getExecutionConfig(); }

    auto Config::saveLLMConfig(const std::string &name, const json &value) -> std::expected<void, ConfigError> {
        std::scoped_lock lock(m_updateMutex);
        if (auto result = validateFields(value); !result) {
            return result;
        }
        Config next;
        return ConfigStore::saveLLMConfig(name, value, [&next](const json &stored) {
            return next.loadValues(stored);
        }).transform([this, &next, &name] { applyLLMValues(name, std::move(next)); });
    }

    auto Config::saveQQConfig(const json &value) -> std::expected<void, ConfigError> {
        std::scoped_lock lock(m_updateMutex);
        if (auto result = validateFields(value); !result) {
            return result;
        }
        json normalized = value;
        auto transport = getStr(value, "oneBotTransport", "http");
        normalized["oneBotTransport"] = transport == "websocket" ? "websocket" : "http";
        Config next;
        return ConfigStore::saveQQConfig(normalized, [&next](const json &stored) {
            return next.loadValues(stored);
        }).transform([this, &next] { applyQQValues(std::move(next)); });
    }

    auto Config::saveMemoryConfig(const json &value) -> std::expected<void, ConfigError> {
        std::scoped_lock lock(m_updateMutex);
        if (auto result = validateFields(value); !result) {
            return result;
        }
        json normalized = value;
        if (getInt(normalized, "contextWindowLimit") <= 0) {
            (normalized)["contextWindowLimit"] = contextWindowLimit;
        }
        if (getInt(normalized, "memorySummaryTriggerCount") <= 0) {
            (normalized)["memorySummaryTriggerCount"] = memorySummaryTriggerCount;
        }
        if (getInt(normalized, "memorySummaryBatchSize") <= 0 ||
            getInt(normalized, "memorySummaryBatchSize") > getInt(normalized, "memorySummaryTriggerCount")) {
            (normalized)["memorySummaryBatchSize"] = getInt(normalized, "memorySummaryTriggerCount") / 2;
        }
        if (getInt(normalized, "memorySummaryContextCount") < 0) {
            (normalized)["memorySummaryContextCount"] = memorySummaryContextCount;
        }
        // Router 子窗口校验: 保留条数必须小于触发条数
        if (getInt(normalized, "routerWindowTriggerCount") <= 0) {
            (normalized)["routerWindowTriggerCount"] = routerWindowTriggerCount;
        }
        if (getInt(normalized, "routerWindowKeepCount") <= 0 ||
            getInt(normalized, "routerWindowKeepCount") >= getInt(normalized, "routerWindowTriggerCount")) {
            (normalized)["routerWindowKeepCount"] = getInt(normalized, "routerWindowTriggerCount") / 2;
        }
        // 召回阈值: 必须在 (0,1) 开区间内
        if (getDouble(normalized, "longTermRecallThreshold") <= 0.0 ||
            getDouble(normalized, "longTermRecallThreshold") >= 1.0) {
            (normalized)["longTermRecallThreshold"] = longTermRecallThreshold;
        }
        // 注入阈值: 必须在 (0,1) 开区间内
        if (getDouble(normalized, "longTermInjectThreshold") <= 0.0 ||
            getDouble(normalized, "longTermInjectThreshold") >= 1.0) {
            (normalized)["longTermInjectThreshold"] = longTermInjectThreshold;
        }

        Config next;
        return ConfigStore::saveMemoryConfig(normalized, [&next](const json &stored) {
            return next.loadValues(stored);
        }).transform([this, &next] { applyMemoryValues(next); });
    }

    auto Config::saveExecutionConfig(const json &value) -> std::expected<void, ConfigError> {
        std::scoped_lock lock(m_updateMutex);
        if (auto result = validateFields(value); !result) {
            return result;
        }
        Config next;
        return ConfigStore::saveExecutionConfig(value, [&next](const json &stored) {
            return next.loadValues(stored);
        }).transform([this, &next] {
            execution.maxToolRounds.store(
              next.execution.maxToolRounds.load(std::memory_order_relaxed), std::memory_order_relaxed);
        });
    }
} // namespace insoulforge
