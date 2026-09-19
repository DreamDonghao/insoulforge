/// @file ConfigStore.cpp
/// @brief 配置存储 - 实现
/// @author donghao
/// @date 2026-08-30

#include <algorithm>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <spdlog/spdlog.h>


    namespace insoulforge::ConfigStore {
        namespace {
            json loadConfigJson(const std::string &key, const json &defaults) {
                const auto &db = Database::instance();
                std::shared_lock lock(db.mutex());

                json config = defaults;
                const Statement stmt(db.handle(), "SELECT value FROM settings WHERE key = ?");
                stmt.bind(1, key);
                if (stmt.step()) {
                    const std::string payload = stmt.getText(0);
                    if (json parsed; tryParseJson(payload, parsed) && parsed.is_object()) {
                        // 以存储值覆盖默认值
                        for (const auto &name: defaults.items()) {
                            if (const auto it = parsed.find(name.key()); it != parsed.end()) {
                                config[name.key()] = *it;
                            }
                        }
                    } else {
                        spdlog::error("settings 配置 {} 解析失败，使用默认值", key);
                    }
                }
                return config;
            }

            void saveConfigJson(const std::string &key, const json &config) {
                const auto &db = Database::instance();
                std::unique_lock lock(db.mutex());

                const Statement stmt(db.handle(), "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)");
                stmt.bind(1, key);
                stmt.bind(2, dumpJson(config));
                stmt.exec();
            }
        } // namespace

        json getLLMConfig(const std::string &name) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            json config;

            const Statement stmt(db.handle(), "SELECT api_key, base_url, path, model, max_tokens, temperature, top_p, "
                                              "reasoning_effort FROM llm_config WHERE name = ?");
            stmt.bind(1, name);

            if (stmt.step()) {
                config["apiKey"] = stmt.getText(0);
                config["baseUrl"] = stmt.getText(1);
                config["path"] = stmt.getText(2);
                config["model"] = stmt.getText(3);
                config["maxTokens"] = stmt.getInt(4);
                config["temperature"] = stmt.getDouble(5);
                config["topP"] = stmt.getDouble(6);
                config["reasoningEffort"] = stmt.getText(7);
            }
            return config;
        }

        void saveLLMConfig(const std::string &name, const json &config) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());

            const Statement stmt(db.handle(),
              "INSERT OR REPLACE INTO llm_config (name, api_key, base_url, path, model, max_tokens, "
              "temperature, top_p, reasoning_effort) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)");
            stmt.bind(1, name);
            stmt.bind(2, getStr(config, "apiKey"));
            stmt.bind(3, getStr(config, "baseUrl"));
            stmt.bind(4, getStr(config, "path"));
            stmt.bind(5, getStr(config, "model"));
            stmt.bind(6, getInt(config, "maxTokens"));
            stmt.bind(7, getDouble(config, "temperature"));
            stmt.bind(8, getDouble(config, "topP"));
            stmt.bind(9, getStr(config, "reasoningEffort"));
            stmt.exec();
        }

        json getAllLLMConfigs() {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            json configs;

            const Statement stmt(db.handle(), "SELECT name, api_key, base_url, path, model, max_tokens, temperature, "
                                              "top_p, reasoning_effort FROM llm_config");
            while (stmt.step()) {
                json cfg;
                cfg["apiKey"] = stmt.getText(1);
                cfg["baseUrl"] = stmt.getText(2);
                cfg["path"] = stmt.getText(3);
                cfg["model"] = stmt.getText(4);
                cfg["maxTokens"] = stmt.getInt(5);
                cfg["temperature"] = stmt.getDouble(6);
                cfg["topP"] = stmt.getDouble(7);
                cfg["reasoningEffort"] = stmt.getText(8);
                configs[stmt.getText(0)] = cfg;
            }
            return configs;
        }

        json getQQConfig() {
            json defaults;
            defaults["accessToken"] = "";
            defaults["selfQQNumber"] = 0;
            defaults["oneBotTransport"] = "http";
            defaults["qqHttpHost"] = "http://127.0.0.1:3000";
            defaults["qqWebSocketHost"] = "ws://127.0.0.1:3001";
            defaults["botName"] = "小喵";
            return loadConfigJson("qq_config", defaults);
        }

        void saveQQConfig(const json &config) {
            saveConfigJson("qq_config", config);
            spdlog::info("QQ Bot 配置已保存");
        }

        json getMemoryConfig() {
            json defaults;
            defaults["contextWindowLimit"] = 100;
            defaults["memorySummaryTriggerCount"] = 100;
            defaults["memorySummaryBatchSize"] = 50;
            defaults["memorySummaryContextCount"] = 10;
            defaults["memoryExtractMaxTokens"] = 4000;
            defaults["routerWindowTriggerCount"] = 20;
            defaults["routerWindowKeepCount"] = 10;
            defaults["shortTermMemoryMax"] = 15;
            defaults["longTermRecallThreshold"] = 0.65;
            defaults["longTermInjectThreshold"] = 0.45;
            json config = loadConfigJson("memory_config", defaults);

            // 兼容 v9 前的窗口配置，避免升级后丢失用户已设定的阈值与保留数量。
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement statement(db.handle(), "SELECT value FROM settings WHERE key = 'memory_config'");
            if (!statement.step()) {
                return config;
            }
            json legacy;
            if (!tryParseJson(statement.getText(0), legacy) || !legacy.is_object()) {
                return config;
            }
            if (!legacy.contains("contextWindowLimit")) {
                config["contextWindowLimit"] = std::max(getInt(legacy, "windowTriggerCount"), 1);
            }
            if (!legacy.contains("memorySummaryTriggerCount")) {
                config["memorySummaryTriggerCount"] = std::max(getInt(legacy, "windowTriggerCount"), 1);
            }
            if (!legacy.contains("memorySummaryBatchSize")) {
                config["memorySummaryBatchSize"] =
                  std::max(getInt(legacy, "windowTriggerCount") - getInt(legacy, "windowKeepCount"), 1);
            }
            return config;
        }

        void saveMemoryConfig(const json &config) {
            saveConfigJson("memory_config", config);
            spdlog::info("记忆配置已保存");
        }

        void initDefaults() {
            struct DefaultConfig {
                const char *name, *apiKey, *baseUrl, *path, *model;
                int maxTokens;
                double temperature, topP;
            };

            constexpr DefaultConfig defaults[] = {{.name = "router",
                                                    .apiKey = "",
                                                    .baseUrl = "http://127.0.0.1:3001",
                                                    .path = "/v1/chat/completions",
                                                    .model = "deepseek-chat",
                                                    .maxTokens = 100,
                                                    .temperature = 0.3,
                                                    .topP = 0.9},
              {.name = "executor",
                .apiKey = "",
                .baseUrl = "http://127.0.0.1:3001",
                .path = "/v1/chat/completions",
                .model = "deepseek-chat",
                .maxTokens = 150,
                .temperature = 0.7,
                .topP = 0.9},
              {.name = "executorThinking",
                .apiKey = "",
                .baseUrl = "http://127.0.0.1:3001",
                .path = "/v1/chat/completions",
                .model = "deepseek-reasoner",
                .maxTokens = 512,
                .temperature = 0.7,
                .topP = 0.9},
              {.name = "image",
                .apiKey = "",
                .baseUrl = "https://dashscope.aliyuncs.com",
                .path = "/compatible-mode/v1/chat/completions",
                .model = "qwen-vl-plus",
                .maxTokens = 1024,
                .temperature = 0.7,
                .topP = 0.9},
              {.name = "embedding",
                .apiKey = "",
                .baseUrl = "http://127.0.0.1:3001",
                .path = "/v1/embeddings",
                .model = "bge-m3",
                .maxTokens = 0,
                .temperature = 0,
                .topP = 0}};

            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());

            for (const auto &[name, apiKey, baseUrl, path, model, maxTokens, temperature, topP]: defaults) {
                Statement checkStmt(db.handle(), "SELECT COUNT(*) FROM llm_config WHERE name = ?");
                checkStmt.bind(1, name);
                if (checkStmt.step() && checkStmt.getInt(0) > 0) {
                    continue; // 已存在，跳过
                }

                const Statement stmt(db.handle(), "INSERT INTO llm_config (name, api_key, base_url, path, model, "
                                                  "max_tokens, temperature, top_p) VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
                stmt.bind(1, name);
                stmt.bind(2, apiKey);
                stmt.bind(3, baseUrl);
                stmt.bind(4, path);
                stmt.bind(5, model);
                stmt.bind(6, maxTokens);
                stmt.bind(7, temperature);
                stmt.bind(8, topP);
                stmt.exec();
                spdlog::info("已初始化默认 LLM 配置: {}", name);
            }
        }

    } // namespace insoulforge::ConfigStore

