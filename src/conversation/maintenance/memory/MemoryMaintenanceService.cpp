/// @file MemoryMaintenanceService.cpp
/// @brief 持久化记忆维护任务的协调实现

#include <agent/memory/LongTermMemoryStore.hpp>
#include <agent/memory/MemoryStore.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceService.hpp>
#include <conversation/maintenance/memory/MemoryMaintenanceStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>

namespace insoulforge {
    namespace {
        constexpr int kRecallPerMemory = 2;
        constexpr size_t kMaxRecalledEntries = 20;
        constexpr std::chrono::seconds kInitialRetryDelay{2};
        constexpr std::chrono::seconds kMaxRetryDelay{60};

        /// @brief 每个会话至多运行一个后台记忆任务消费者
        struct MaintenanceScheduler {
            std::mutex mutex;
            std::unordered_set<uint64_t> activeSessions;
            std::function<void(uint64_t)> summaryCompletedCallback;
        };

        [[nodiscard]] MaintenanceScheduler &scheduler() {
            static MaintenanceScheduler instance;
            return instance;
        }

        void notifySummaryCompleted(const uint64_t sessionId) {
            std::function<void(uint64_t)> callback;
            {
                std::lock_guard lock(scheduler().mutex);
                callback = scheduler().summaryCompletedCallback;
            }
            if (!callback) {
                return;
            }
            try {
                callback(sessionId);
            } catch (const std::exception &error) {
                Logger::session(sessionId).error("应用已完成记忆总结失败: {}", error.what());
            } catch (...) {
                Logger::session(sessionId).error("应用已完成记忆总结失败: 未知异常");
            }
        }

        std::vector<std::string> splitLines(const std::string &text) {
            std::vector<std::string> lines;
            std::istringstream stream(text);
            std::string line;
            while (std::getline(stream, line)) {
                if (!line.empty()) {
                    lines.push_back(line);
                }
            }
            return lines;
        }

        [[nodiscard]] std::string joinLines(const std::vector<std::string> &lines) {
            std::string result;
            for (const std::string &line: lines) {
                result += line + '\n';
            }
            return result;
        }

        [[nodiscard]] std::string numberedLines(const std::vector<std::string> &lines) {
            std::string text;
            for (size_t index = 0; index < lines.size(); ++index) {
                text += std::to_string(index + 1) + ". " + lines[index] + '\n';
            }
            return text.empty() ? "（空）" : text;
        }

        [[nodiscard]] std::optional<json> parseLlmJson(
          const std::optional<std::string> &result, const std::string_view tag, const uint64_t sessionId) {
            if (!result) {
                Logger::session(sessionId).error("{}: API 请求失败", tag);
                return std::nullopt;
            }
            std::string payload;
            if (!tryExtractJsonObject(*result, payload)) {
                Logger::session(sessionId).warn("{}: 响应中无 JSON: {}", tag, result->substr(0, 100));
                return std::nullopt;
            }
            json parsed;
            if (!tryParseJson(payload, parsed) || !parsed.is_object()) {
                Logger::session(sessionId).warn("{}: JSON 解析失败", tag);
                return std::nullopt;
            }
            return parsed;
        }

        drogon::Task<std::optional<std::vector<std::string>>> extractMemories(
          std::string records, std::string context, const int maxTokens, const uint64_t sessionId) {
            const json messages = json::array({
              {{"role", "system"}, {"content", R"(你是一个【群聊记忆提取器】。从群聊记录中提取值得记住的信息。

提取规则：每条记忆必须带人物归属；每条只包含一条完整信息，简短客观；不推测、不扩写。
着重提取外号别称、喜好、习惯、关系和重要约定。
不提取一次性玩笑、情绪宣泄、系统指令、控制信息、固定套话或无价值内容。
“补充上下文”仅用于消除歧义，绝不能从中提取记忆。
只输出 JSON 对象：{"memories":["小明喜欢写Python"]}；没有内容时输出：{"memories":[]})"}},
              {{"role", "user"},
                {"content", "=== 待提取消息 ===\n" + std::move(records) + "\n=== 补充上下文（不得提取） ===\n" +
                              std::move(context) + "\n\n请输出提取结果 JSON："}},
            });
            const auto parsed =
              parseLlmJson(co_await LlmClient::requestLLM(messages, 0.4f, 0.9f, maxTokens, "memory", sessionId),
                "记忆提取", sessionId);
            if (!parsed) {
                co_return std::nullopt;
            }
            const json &values = atOrNull(*parsed, "memories");
            if (!values.is_array()) {
                Logger::session(sessionId).warn("记忆提取: 输出缺少 memories 数组");
                co_return std::nullopt;
            }
            std::vector<std::string> memories;
            for (const json &value: values) {
                if (std::string content = trim(jsonToString(value)); !content.empty()) {
                    memories.push_back(std::move(content));
                }
            }
            co_return memories;
        }

        drogon::Task<std::vector<SimilarMemory>> recallForMerge(
          const std::vector<std::string> &memories, const uint64_t sessionId) {
            const float threshold = static_cast<float>(Config::instance().longTermRecallThreshold);
            std::unordered_map<int64_t, SimilarMemory> recalled;
            for (const std::string &memory: memories) {
                const auto embedding = co_await LlmClient::requestEmbedding(memory, sessionId);
                if (!embedding) {
                    Logger::session(sessionId).warn("记忆召回: 向量化失败，跳过该条查询");
                    continue;
                }
                for (const SimilarMemory &hit:
                  LongTermMemoryStore::searchSimilar(sessionId, *embedding, kRecallPerMemory)) {
                    if (hit.similarity < threshold) {
                        continue;
                    }
                    if (auto [existing, inserted] = recalled.try_emplace(hit.id, hit);
                      !inserted && existing->second.similarity < hit.similarity) {
                        existing->second.similarity = hit.similarity;
                    }
                }
            }
            std::vector<SimilarMemory> result;
            result.reserve(recalled.size());
            for (SimilarMemory &memory: recalled | std::views::values) {
                result.push_back(std::move(memory));
            }
            if (result.size() > kMaxRecalledEntries) {
                std::ranges::sort(result, [](const SimilarMemory &left, const SimilarMemory &right) {
                    return left.similarity > right.similarity;
                });
                result.resize(kMaxRecalledEntries);
            }
            co_return result;
        }

        struct ReconciledLongTermMemory {
            std::vector<int64_t> sources;
            std::string content;
        };

        struct ReconcileResult {
            std::vector<std::string> shortTerm;
            std::vector<ReconciledLongTermMemory> longTerm;
        };

        drogon::Task<std::optional<ReconcileResult>> reconcileMemory(std::vector<std::string> currentShortTerm,
          const std::vector<std::string> &newMemories, const std::vector<SimilarMemory> &recalled,
          const bool longTermEnabled, const uint64_t sessionId) {
            const auto &config = Config::instance();
            std::string prompt = R"(你是一个【群聊记忆整理器】。把新提取的记忆与当前短期记忆、召回的长期记忆合并整理。

规则：去重并合并相关条目；冲突时以新提取的信息为准；每条记忆带人物归属，简短客观。
短期记忆保存临时状态、近期事件和上下文相关内容，最多 )" +
                                 std::to_string(config.shortTermMemoryMax) +
                                 R"( 条。长期记忆只保留稳定特征、关系和重要约定，宁缺毋滥。
召回的长期记忆没有变化时不要输出；修改或合并时 sources 必须填入被替代条目的编号，全新条目填空数组。
只输出 JSON：{"shortTerm":["小明正在准备考试"],"longTerm":[{"sources":[12],"content":"小明喜欢Python"}]})";
            if (!longTermEnabled) {
                prompt += "\n长期记忆库不可用：longTerm 必须为空数组。";
            }

            std::string recalledText;
            for (const SimilarMemory &memory: recalled) {
                recalledText += "[" + std::to_string(memory.id) + "] " + memory.content + '\n';
            }
            if (recalledText.empty()) {
                recalledText = "（无）\n";
            }
            const json messages = json::array({
              {{"role", "system"}, {"content", std::move(prompt)}},
              {{"role", "user"},
                {"content", "=== 当前短期记忆 ===\n" + numberedLines(currentShortTerm) + "\n=== 新提取的记忆 ===\n" +
                              numberedLines(newMemories) + "\n=== 召回的长期记忆 ===\n" + recalledText +
                              "\n请输出整理结果 JSON："}},
            });
            const auto parsed = parseLlmJson(
              co_await LlmClient::requestLLM(messages, 0.3f, 0.9f, config.memoryExtractMaxTokens, "memory", sessionId),
              "记忆整理", sessionId);
            if (!parsed || !atOrNull(*parsed, "shortTerm").is_array()) {
                Logger::session(sessionId).warn("记忆整理: 输出缺少 shortTerm 数组");
                co_return std::nullopt;
            }

            ReconcileResult result;
            for (const json &value: atOrNull(*parsed, "shortTerm")) {
                if (std::string content = trim(jsonToString(value)); !content.empty()) {
                    result.shortTerm.push_back(std::move(content));
                }
            }
            result.shortTerm.resize(
              std::min(result.shortTerm.size(), static_cast<size_t>(std::max(config.shortTermMemoryMax, 0))));

            if (longTermEnabled && atOrNull(*parsed, "longTerm").is_array()) {
                for (const json &value: atOrNull(*parsed, "longTerm")) {
                    if (!value.is_object()) {
                        continue;
                    }
                    ReconciledLongTermMemory memory{.content = trim(getStr(value, "content"))};
                    if (memory.content.empty()) {
                        continue;
                    }
                    for (const json &source: atOrNull(value, "sources")) {
                        if (!source.is_number_integer()) {
                            continue;
                        }
                        if (const int64_t id = source.get<int64_t>();
                          std::ranges::any_of(recalled, [id](const SimilarMemory &item) { return item.id == id; })) {
                            memory.sources.push_back(id);
                        }
                    }
                    result.longTerm.push_back(std::move(memory));
                }
            }
            co_return result;
        }

        void projectRecordsForMemory(std::vector<json> &records) {
            for (json &record: records) {
                if (json content; tryParseJson(getStr(record, "content"), content) && content.is_object()) {
                    record["content"] = dumpJson(MessageRecord::projectForAgent(content));
                }
            }
        }

        [[nodiscard]] std::string formatRecordsText(const std::vector<json> &records) {
            std::string text = "[";
            for (size_t index = 0; index < records.size(); ++index) {
                if (index != 0) {
                    text += ',';
                }
                text += getStr(records[index], "content");
            }
            return text + ']';
        }

        drogon::Task<bool> maintainJob(const MemoryMaintenanceJob &job) {
            std::vector<json> records;
            records.reserve(job.messages.size());
            for (const json &message: job.messages) {
                if (message.is_object()) {
                    records.push_back({{"content", dumpJson(message)}});
                }
            }
            projectRecordsForMemory(records);
            if (records.empty()) {
                MemoryMaintenanceStore::complete(job.id, job.sessionId, std::nullopt, {});
                co_return true;
            }

            const auto &config = Config::instance();
            std::vector<json> contextRecords;
            contextRecords.reserve(job.contextMessages.size());
            for (const json &message: job.contextMessages) {
                if (message.is_object()) {
                    contextRecords.push_back({{"content", dumpJson(message)}});
                }
            }
            projectRecordsForMemory(contextRecords);
            const auto extracted = co_await extractMemories(formatRecordsText(records),
              formatRecordsText(contextRecords), config.memoryExtractMaxTokens, job.sessionId);
            if (!extracted) {
                co_return false;
            }
            if (extracted->empty()) {
                MemoryMaintenanceStore::complete(job.id, job.sessionId, std::nullopt, {});
                co_return true;
            }

            const bool longTermEnabled = !config.embedding.baseUrl.empty() && !config.embedding.model.empty();
            const std::vector<SimilarMemory> recalled =
              longTermEnabled ? co_await recallForMerge(*extracted, job.sessionId) : std::vector<SimilarMemory>{};
            const auto reconciled = co_await reconcileMemory(splitLines(MemoryStore::getShortTermMemory(job.sessionId)),
              *extracted, recalled, longTermEnabled, job.sessionId);
            if (!reconciled) {
                co_return false;
            }

            std::vector<PreparedLongTermMemory> prepared;
            std::unordered_set<std::string> contents;
            std::unordered_set<int64_t> replacedIds;
            for (const auto &[sources, content]: reconciled->longTerm) {
                if (!contents.insert(content).second) {
                    continue;
                }
                PreparedLongTermMemory item{.content = content};
                for (const int64_t id: sources) {
                    if (!replacedIds.contains(id)) {
                        item.replacedIds.push_back(id);
                    }
                }
                const auto embedding = co_await LlmClient::requestEmbedding(item.content, job.sessionId);
                if (!embedding) {
                    Logger::session(job.sessionId).warn("长期记忆向量化失败，跳过本条: {}", item.content);
                    continue;
                }
                item.embedding = *embedding;
                replacedIds.insert(item.replacedIds.begin(), item.replacedIds.end());
                prepared.push_back(std::move(item));
            }

            MemoryMaintenanceStore::complete(job.id, job.sessionId, joinLines(reconciled->shortTerm), prepared);
            Logger::session(job.sessionId)
              .info("记忆任务已完成: #{}，短期记忆 {} 条，长期记忆 +{}", job.id, reconciled->shortTerm.size(),
                prepared.size());
            co_return true;
        }

        [[nodiscard]] std::chrono::seconds retryDelay(const int attempts) {
            const int exponent = std::clamp(attempts, 0, 5);
            return std::min(kInitialRetryDelay * (1 << exponent), kMaxRetryDelay);
        }

        drogon::Task<> drainSession(uint64_t sessionId);

        [[nodiscard]] bool acquireSessionConsumer(const uint64_t sessionId) {
            auto &state = scheduler();
            std::lock_guard lock(state.mutex);
            return state.activeSessions.insert(sessionId).second;
        }

        void releaseSessionConsumer(const uint64_t sessionId) {
            std::lock_guard lock(scheduler().mutex);
            scheduler().activeSessions.erase(sessionId);
        }

        drogon::Task<> drainSession(const uint64_t sessionId) {
            try {
                while (const auto job = MemoryMaintenanceStore::next(sessionId)) {
                    bool completed = false;
                    try {
                        completed = co_await maintainJob(*job);
                    } catch (const std::exception &error) {
                        Logger::session(sessionId).error("记忆任务 #{} 异常: {}", job->id, error.what());
                    } catch (...) {
                        Logger::session(sessionId).error("记忆任务 #{} 异常: 未知错误", job->id);
                    }
                    if (completed) {
                        notifySummaryCompleted(sessionId);
                        continue;
                    }
                    MemoryMaintenanceStore::incrementAttempt(job->id);
                    const auto delay = retryDelay(job->attemptCount);
                    Logger::session(sessionId).warn("记忆任务 #{} 将在 {} 秒后重试", job->id, delay.count());
                    co_await drogon::sleepCoro(drogon::app().getLoop(), delay);
                }
            } catch (const std::exception &error) {
                Logger::session(sessionId).error("记忆任务消费者异常退出: {}", error.what());
            } catch (...) {
                Logger::session(sessionId).error("记忆任务消费者异常退出: 未知错误");
            }
        }
    } // namespace

    drogon::Task<> MemoryMaintenanceService::processPending(const uint64_t sessionId) {
        if (!acquireSessionConsumer(sessionId)) {
            co_return;
        }
        co_await drainSession(sessionId);
        releaseSessionConsumer(sessionId);
        try {
            if (MemoryMaintenanceStore::next(sessionId)) {
                co_await processPending(sessionId);
            }
        } catch (const std::exception &error) {
            Logger::session(sessionId).error("检查待处理记忆任务失败: {}", error.what());
        }
        co_return;
    }

    void MemoryMaintenanceService::setSummaryCompletedCallback(std::function<void(uint64_t)> callback) {
        std::lock_guard lock(scheduler().mutex);
        scheduler().summaryCompletedCallback = std::move(callback);
    }

} // namespace insoulforge
