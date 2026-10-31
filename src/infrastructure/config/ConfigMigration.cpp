/// @file ConfigMigration.cpp
/// @brief 配置文件升级；业务读写只使用当前字段。

#include <utility>

#include <infrastructure/config/ConfigMigration.hpp>

namespace insoulforge::ConfigStore {
    /// @brief 将早期配置文件字段迁移为当前命名与结构。
    auto migrateConfigFile(json &config, const json &defaults) -> bool {
        bool changed = config.erase("version") > 0;
        if (const auto memory = config.find("memory"); memory != config.end() && memory->is_object()) {
            changed = memory->erase("shortTermMemoryMax") > 0 || changed;
        }
        const auto llm = config.find("llm");
        if (llm == config.end() || !llm->is_object())
            return changed;

        if (!llm->contains("memory")) {
            json memoryConfig = llm->value("executor", defaults["llm"]["memory"]);
            if (!memoryConfig.is_object()) {
                memoryConfig = defaults["llm"]["memory"];
            }
            const json &legacyMemory = atOrNull(config, "memory");
            const i32 maxTokens = getInt(legacyMemory, "memoryExtractMaxTokens", 4000);
            memoryConfig["maxTokens"] = maxTokens > 0 ? maxTokens : 4000;
            memoryConfig["temperature"] = 0.4;
            memoryConfig["topP"] = 0.9;
            memoryConfig["timeoutSeconds"] = 180;
            memoryConfig["maxRetries"] = 0;
            (*llm)["memory"] = std::move(memoryConfig);
            changed = true;
        }
        if (const auto memory = config.find("memory"); memory != config.end() && memory->is_object()) {
            changed = memory->erase("memoryExtractMaxTokens") > 0 || changed;
        }

        for (auto &entry: llm->items()) {
            auto &modelConfig = entry.value();
            if (!modelConfig.is_object())
                continue;
            if (entry.key() == "imageGeneration" && !modelConfig.contains("apiType")) {
                modelConfig["apiType"] = "openrouter";
                changed = true;
            }
            if (modelConfig.contains("top_P")) {
                if (!modelConfig.contains("topP")) {
                    modelConfig["topP"] = modelConfig["top_P"];
                }
                modelConfig.erase("top_P");
                changed = true;
            }
            if (entry.key() == "embedding" || entry.key() == "jev" || entry.key() == "imageGeneration") {
                changed = modelConfig.erase("maxTokens") > 0 || changed;
                changed = modelConfig.erase("temperature") > 0 || changed;
                changed = modelConfig.erase("topP") > 0 || changed;
                changed = modelConfig.erase("reasoningEffort") > 0 || changed;
            }
        }
        return changed;
    }

} // namespace insoulforge::ConfigStore
