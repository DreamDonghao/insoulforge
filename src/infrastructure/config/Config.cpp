/// @file Config.cpp
/// @brief 全局配置管理 - 实现

#include <cmath>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {
        /// @brief 从配置文件加载单个 LLM 配置
        /// @param name 配置名（router/executor/executorThinking/image/memory 等）
        /// @param apiConfig 输出的 API 配置
        /// @param modelParams 模型参数（可为 nullptr，表示不加载）
        void loadLLMConfig(
          const std::string_view name, LLMApiConfig &apiConfig, LLMModelParams *modelParams = nullptr) {
            const auto cfg = ConfigStore::getLLMConfig(std::string(name));
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


    void Config::loadFromStorage() {
        loadLLMConfig("router", router, &routerParams);
        loadLLMConfig("executor", executor, &executorParams);
        loadLLMConfig("executorThinking", executorThinking, &executorThinkingParams);
        loadLLMConfig("image", image, &imageParams);
        loadLLMConfig("imageGeneration", imageGeneration);
        loadLLMConfig("memory", memory, &memoryParams);
        loadLLMConfig("embedding", embedding);
        loadLLMConfig("jev", jev);
        const f64 configuredJevConfidence = getDouble(ConfigStore::getLLMConfig("jev"), "minConfidence", 0.6);
        jevMinConfidence =
          std::isfinite(configuredJevConfidence) && configuredJevConfidence >= 0.0 && configuredJevConfidence <= 1.0
            ? configuredJevConfidence
            : 0.6;

        execution.maxToolRounds.store(
          getInt(ConfigStore::getExecutionConfig(), "maxToolRounds", ExecutionSettings::kDefaultMaxToolRounds),
          std::memory_order_relaxed);

        // 加载记忆配置
        if (const auto memCfg = ConfigStore::getMemoryConfig(); !memCfg.is_null()) {
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
            Logger::info(0, "Config", fmt::format("记忆配置已从配置文件加载"));
        }

        // 加载 QQ Bot 配置
        if (auto qqCfg = ConfigStore::getQQConfig(); !qqCfg.is_null()) {
            accessToken = trim(getStr(qqCfg, "accessToken"));
            selfQQNumber = getInt64(qqCfg, "selfQQNumber");
            oneBotTransport = getStr(qqCfg, "oneBotTransport", "http");
            qqHttpHost = trim(getStr(qqCfg, "qqHttpHost"));
            qqWebSocketHost = trim(getStr(qqCfg, "qqWebSocketHost"));
            if (qqCfg.contains("botName")) {
                botName = getStr(qqCfg, "botName");
            }
            if (oneBotTransport != "http" && oneBotTransport != "websocket") {
                oneBotTransport = "http";
            }
            Logger::info(0, "Config", fmt::format("QQ Bot 配置已从配置文件加载"));
        }

        Logger::info(0, "Config", fmt::format("所有配置已从配置文件加载"));
    }
} // namespace insoulforge
