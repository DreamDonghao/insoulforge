/// @file ConfigStore.cpp
/// @brief 全局配置文件存储 - 实现

#include <algorithm>
#include <cmath>
#include <fstream>
#include <stdexcept>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/logging/Logger.hpp>


namespace insoulforge::ConfigStore {
    namespace {
        struct ConfigFileState {
            std::filesystem::path path;
            json content;
            std::mutex mutex;
            bool initialized = false;
        };

        auto state() -> ConfigFileState & {
            static ConfigFileState instance;
            return instance;
        }

        auto defaultChatConfig(const i32 maxTokens, const f64 temperature, const f64 topP) -> json {
            return {{"apiKey", ""}, {"baseUrl", ""}, {"path", "/chat/completions"}, {"model", ""},
              {"maxTokens", maxTokens}, {"temperature", temperature}, {"topP", topP}, {"reasoningEffort", ""}};
        }

        auto defaultEmbeddingConfig() -> json {
            return {{"apiKey", ""}, {"baseUrl", ""}, {"path", "/embeddings"}, {"model", ""}};
        }

        auto defaultImageGenerationConfig() -> json {
            return {{"apiKey", ""}, {"baseUrl", "https://api.openai.com/v1"}, {"path", "/images/generations"},
              {"model", "gpt-image-2.5-flare"}};
        }

        /// @brief 默认使用 OpenRouter Decisions API；密钥留空时不启用 Jev。
        auto defaultJevConfig() -> json {
            return {{"apiKey", ""}, {"baseUrl", "https://openrouter.ai/api/alpha"}, {"path", "/decisions"},
              {"model", "~typesafe/jev-latest"}, {"minConfidence", 0.6}};
        }

        auto defaultConfig() -> json {
            return {
              {"llm",
                {{"router", defaultChatConfig(100, 0.3, 0.9)}, {"executor", defaultChatConfig(150, 0.7, 0.9)},
                  {"executorThinking", defaultChatConfig(512, 0.7, 0.9)}, {"image", defaultChatConfig(1024, 0.7, 0.9)},
                  {"imageGeneration", defaultImageGenerationConfig()}, {"memory", defaultChatConfig(4000, 0.4, 0.9)},
                  {"embedding", defaultEmbeddingConfig()}, {"jev", defaultJevConfig()}}},
              {"qq", {{"accessToken", ""}, {"selfQQNumber", 0}, {"oneBotTransport", "websocket"},
                       {"qqHttpHost", "http://127.0.0.1:3000"}, {"qqWebSocketHost", "ws://127.0.0.1:3001"},
                       {"botName", "机器人"}}},
              {"memory",
                {{"contextWindowLimit", 100}, {"memorySummaryTriggerCount", 100}, {"memorySummaryBatchSize", 50},
                  {"memorySummaryContextCount", 10}, {"routerWindowTriggerCount", 20}, {"routerWindowKeepCount", 10},
                  {"shortTermMemoryMax", 15}, {"longTermRecallThreshold", 0.65}, {"longTermInjectThreshold", 0.45}}},
              {"execution", {{"maxToolRounds", ExecutionSettings::kDefaultMaxToolRounds}}}};
        }

        auto validToolRounds(const json &value) -> bool {
            return value.is_number_integer() && value >= ExecutionSettings::kMinToolRounds &&
                   value <= ExecutionSettings::kMaxToolRounds;
        }

        /// @brief 启动时修复整数类型以外或越界的执行参数，不截断小数。
        auto normalizeExecutionConfig(json &config) -> bool {
            auto &rounds = config["execution"]["maxToolRounds"];
            if (validToolRounds(rounds)) {
                return false;
            }
            rounds = ExecutionSettings::kDefaultMaxToolRounds;
            return true;
        }

        auto compatibleType(const json &value, const json &defaultValue) -> bool {
            if (defaultValue.is_number())
                return value.is_number();
            return value.type() == defaultValue.type();
        }

        /// @brief 用默认结构修复缺失或类型不兼容的字段，保留未知字段以兼容后续版本。
        auto applyDefaults(json &config, const json &defaults) -> bool {
            bool changed = false;
            for (const auto &[key, defaultValue]: defaults.items()) {
                auto value = config.find(key);
                if (value == config.end() || !compatibleType(*value, defaultValue)) {
                    config[key] = defaultValue;
                    changed = true;
                    continue;
                }
                if (defaultValue.is_object()) {
                    changed = applyDefaults(*value, defaultValue) || changed;
                }
            }
            return changed;
        }

        /// @brief 将早期配置文件字段迁移为当前命名与结构。
        auto migrateLegacyFields(json &config) -> bool {
            bool changed = false;
            changed = config.erase("version") > 0;
            auto llm = config.find("llm");
            if (llm == config.end() || !llm->is_object())
                return false;

            if (!llm->contains("memory")) {
                json memoryConfig = llm->value("executor", defaultChatConfig(4000, 0.4, 0.9));
                if (!memoryConfig.is_object()) {
                    memoryConfig = defaultChatConfig(4000, 0.4, 0.9);
                }
                const json &legacyMemory = atOrNull(config, "memory");
                const i32 maxTokens = getInt(legacyMemory, "memoryExtractMaxTokens", 4000);
                memoryConfig["maxTokens"] = maxTokens > 0 ? maxTokens : 4000;
                memoryConfig["temperature"] = 0.4;
                memoryConfig["topP"] = 0.9;
                (*llm)["memory"] = std::move(memoryConfig);
                changed = true;
            }
            if (auto memory = config.find("memory"); memory != config.end() && memory->is_object()) {
                changed = memory->erase("memoryExtractMaxTokens") > 0 || changed;
            }

            for (auto &entry: llm->items()) {
                auto &modelConfig = entry.value();
                if (!modelConfig.is_object())
                    continue;
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

        void writeConfigFile(const std::filesystem::path &path, const json &config) {
            const std::filesystem::path temporaryPath = path.string() + ".tmp";
            {
                std::ofstream output(temporaryPath, std::ios::trunc);
                if (!output) {
                    throw std::runtime_error("无法写入配置临时文件: " + temporaryPath.string());
                }
                output << dumpJson(config, true, 2) << '\n';
                output.flush();
                if (!output) {
                    throw std::runtime_error("配置临时文件写入失败: " + temporaryPath.string());
                }
            }

            std::error_code error;
            std::filesystem::rename(temporaryPath, path, error);
            if (error) {
                throw std::runtime_error("无法原子替换配置文件: " + error.message());
            }
        }

        void ensureInitialized() {
            auto &fileState = state();
            {
                std::scoped_lock lock(fileState.mutex);
                if (fileState.initialized)
                    return;
            }
            initialize();
        }

        auto getSection(const std::string_view section) -> json {
            ensureInitialized();
            auto &fileState = state();
            std::scoped_lock lock(fileState.mutex);
            return fileState.content[std::string(section)];
        }

        void saveSection(const std::string_view section, json content) {
            ensureInitialized();
            auto &fileState = state();
            std::scoped_lock lock(fileState.mutex);
            fileState.content[std::string(section)] = std::move(content);
            migrateLegacyFields(fileState.content);
            applyDefaults(fileState.content, defaultConfig());
            writeConfigFile(fileState.path, fileState.content);
        }
    } // namespace

    void initialize(const std::string &path) {
        auto &fileState = state();
        std::scoped_lock lock(fileState.mutex);

        const std::filesystem::path configPath(path);
        if (configPath.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(configPath.parent_path(), error);
            if (error) {
                throw std::runtime_error("无法创建配置目录: " + error.message());
            }
        }

        json config = defaultConfig();
        bool needsWrite = !std::filesystem::exists(configPath);
        if (!needsWrite) {
            std::ifstream input(configPath);
            const std::string payload{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            json parsed;
            if (!input || !tryParseJson(payload, parsed) || !parsed.is_object()) {
                const auto timestamp =
                  std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                    .count();
                const std::filesystem::path backupPath = configPath.string() + ".broken." + std::to_string(timestamp);
                std::error_code error;
                std::filesystem::rename(configPath, backupPath, error);
                if (error) {
                    throw std::runtime_error("无法备份损坏的配置文件: " + error.message());
                }
                Logger::error(
                  0, "Config", fmt::format("配置文件格式无效，已备份为 {} 并重建默认配置", backupPath.string()));
                needsWrite = true;
            } else {
                config = std::move(parsed);
                needsWrite = migrateLegacyFields(config);
                needsWrite = applyDefaults(config, defaultConfig()) || needsWrite;
            }
        }

        needsWrite = normalizeExecutionConfig(config) || needsWrite;
        fileState.path = configPath;
        fileState.content = std::move(config);
        fileState.initialized = true;
        if (needsWrite) {
            writeConfigFile(fileState.path, fileState.content);
            Logger::info(0, "Config", fmt::format("全局配置文件已初始化: {}", fileState.path.string()));
        } else {
            Logger::info(0, "Config", fmt::format("全局配置文件已加载: {}", fileState.path.string()));
        }
    }

    auto getLLMConfig(const std::string &name) -> json {
        const json llm = getSection("llm");
        const auto it = llm.find(name);
        return it == llm.end() ? json{} : *it;
    }

    void saveLLMConfig(const std::string &name, const json &config) {
        json llm = getSection("llm");
        json persistedConfig = config;
        persistedConfig.erase("name");
        persistedConfig.erase("top_P");
        if (name == "embedding" || name == "jev" || name == "imageGeneration") {
            persistedConfig.erase("maxTokens");
            persistedConfig.erase("temperature");
            persistedConfig.erase("topP");
            persistedConfig.erase("reasoningEffort");
            if (name == "jev") {
                const f64 confidence =
                  getDouble(config, "minConfidence", getDouble(atOrNull(llm, "jev"), "minConfidence", 0.6));
                persistedConfig["minConfidence"] = std::isfinite(confidence) ? std::clamp(confidence, 0.0, 1.0) : 0.6;
            }
        } else {
            persistedConfig["topP"] = getDouble(config, "topP", getDouble(config, "top_P", 0.9));
        }
        if (name != "jev") {
            persistedConfig.erase("minConfidence");
        }
        llm[name] = std::move(persistedConfig);
        saveSection("llm", std::move(llm));
        Logger::info(0, "Config", fmt::format("LLM 配置已保存: {}", name));
    }

    auto getAllLLMConfigs() -> json { return getSection("llm"); }

    auto getQQConfig() -> json { return getSection("qq"); }

    void saveQQConfig(const json &config) {
        saveSection("qq", config);
        Logger::info(0, "Config", fmt::format("QQ Bot 配置已保存"));
    }

    auto getMemoryConfig() -> json { return getSection("memory"); }

    void saveMemoryConfig(const json &config) {
        saveSection("memory", config);
        Logger::info(0, "Config", fmt::format("记忆配置已保存"));
    }

    auto getExecutionConfig() -> json { return getSection("execution"); }

    void saveExecutionConfig(const json &config) {
        if (!config.is_object() || !validToolRounds(atOrNull(config, "maxToolRounds"))) {
            throw std::invalid_argument("最大工具迭代轮数必须是 1 到 100 之间的整数");
        }

        ensureInitialized();
        auto &fileState = state();
        std::scoped_lock lock(fileState.mutex);
        auto nextConfig = fileState.content;
        nextConfig["execution"].update(config);
        writeConfigFile(fileState.path, nextConfig);
        fileState.content = std::move(nextConfig);
        // 与配置文件在同一个锁内更新，避免并发保存使运行时值回退。
        Config::instance().execution.maxToolRounds.store(config["maxToolRounds"].get<i32>(), std::memory_order_relaxed);
        Logger::info(0, "Config", "执行配置已保存");
    }
} // namespace insoulforge::ConfigStore
