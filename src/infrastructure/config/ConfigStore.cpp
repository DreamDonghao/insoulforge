/// @file ConfigStore.cpp
/// @brief 全局配置文件存储 - 实现

#include <infrastructure/config/ConfigStore.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>

#include <infrastructure/JsonUtil.hpp>
#include <spdlog/spdlog.h>

namespace insoulforge::ConfigStore {
    namespace {
        struct ConfigFileState {
            std::filesystem::path path;
            json content;
            std::mutex mutex;
            bool initialized = false;
        };

        ConfigFileState &state() {
            static ConfigFileState instance;
            return instance;
        }

        json defaultChatConfig(const int maxTokens, const double temperature, const double topP) {
            return {{"apiKey", ""}, {"baseUrl", ""}, {"path", "/chat/completions"}, {"model", ""},
              {"maxTokens", maxTokens}, {"temperature", temperature}, {"topP", topP}, {"reasoningEffort", ""}};
        }

        json defaultEmbeddingConfig() {
            return {{"apiKey", ""}, {"baseUrl", ""}, {"path", "/embeddings"}, {"model", ""}};
        }

        json defaultConfig() {
            return {
              {"llm", {{"router", defaultChatConfig(100, 0.3, 0.9)}, {"executor", defaultChatConfig(150, 0.7, 0.9)},
                        {"executorThinking", defaultChatConfig(512, 0.7, 0.9)},
                        {"image", defaultChatConfig(1024, 0.7, 0.9)}, {"embedding", defaultEmbeddingConfig()}}},
              {"qq", {{"accessToken", ""}, {"selfQQNumber", 0}, {"oneBotTransport", "websocket"},
                       {"qqHttpHost", "http://127.0.0.1:3000"}, {"qqWebSocketHost", "ws://127.0.0.1:3001"},
                       {"botName", "机器人"}}},
              {"memory",
                {{"contextWindowLimit", 100}, {"memorySummaryTriggerCount", 100}, {"memorySummaryBatchSize", 50},
                  {"memorySummaryContextCount", 10}, {"memoryExtractMaxTokens", 4000}, {"routerWindowTriggerCount", 20},
                  {"routerWindowKeepCount", 10}, {"shortTermMemoryMax", 15}, {"longTermRecallThreshold", 0.65},
                  {"longTermInjectThreshold", 0.45}}}};
        }

        bool compatibleType(const json &value, const json &defaultValue) {
            if (defaultValue.is_number())
                return value.is_number();
            return value.type() == defaultValue.type();
        }

        /// @brief 用默认结构修复缺失或类型不兼容的字段，保留未知字段以兼容后续版本。
        bool applyDefaults(json &config, const json &defaults) {
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
        bool migrateLegacyFields(json &config) {
            bool changed = false;
            changed = config.erase("version") > 0;
            const auto llm = config.find("llm");
            if (llm == config.end() || !llm->is_object())
                return false;

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
                if (entry.key() == "embedding") {
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

        json getSection(const std::string_view section) {
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
                spdlog::error("配置文件格式无效，已备份为 {} 并重建默认配置", backupPath.string());
                needsWrite = true;
            } else {
                config = std::move(parsed);
                needsWrite = migrateLegacyFields(config);
                needsWrite = applyDefaults(config, defaultConfig()) || needsWrite;
            }
        }

        fileState.path = configPath;
        fileState.content = std::move(config);
        fileState.initialized = true;
        if (needsWrite) {
            writeConfigFile(fileState.path, fileState.content);
            spdlog::info("全局配置文件已初始化: {}", fileState.path.string());
        } else {
            spdlog::info("全局配置文件已加载: {}", fileState.path.string());
        }
    }

    json getLLMConfig(const std::string &name) {
        const json llm = getSection("llm");
        const auto it = llm.find(name);
        return it == llm.end() ? json{} : *it;
    }

    void saveLLMConfig(const std::string &name, const json &config) {
        json llm = getSection("llm");
        json persistedConfig = config;
        persistedConfig.erase("name");
        persistedConfig["topP"] = getDouble(config, "topP", getDouble(config, "top_P", 0.9));
        persistedConfig.erase("top_P");
        if (name == "embedding") {
            persistedConfig.erase("maxTokens");
            persistedConfig.erase("temperature");
            persistedConfig.erase("topP");
            persistedConfig.erase("reasoningEffort");
        }
        llm[name] = std::move(persistedConfig);
        saveSection("llm", std::move(llm));
        spdlog::info("LLM 配置已保存: {}", name);
    }

    json getAllLLMConfigs() { return getSection("llm"); }

    json getQQConfig() { return getSection("qq"); }

    void saveQQConfig(const json &config) {
        saveSection("qq", config);
        spdlog::info("QQ Bot 配置已保存");
    }

    json getMemoryConfig() { return getSection("memory"); }

    void saveMemoryConfig(const json &config) {
        saveSection("memory", config);
        spdlog::info("记忆配置已保存");
    }
} // namespace insoulforge::ConfigStore
