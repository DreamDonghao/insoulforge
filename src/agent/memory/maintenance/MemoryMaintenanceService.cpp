/// @file MemoryMaintenanceService.cpp
/// @brief 从消息批次提取并整理聊天记忆

#include <agent/memory/LongTermMemoryStore.hpp>
#include <agent/memory/MemoryStore.hpp>
#include <agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/SessionStore.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>

namespace insoulforge {
    namespace {
        constexpr i32 kRecallPerMemory = 2;
        constexpr size_t kMaxRecalledEntries = 20;
        auto splitLines(const std::string &text) -> std::vector<std::string> {
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

        [[nodiscard]] auto joinLines(const std::vector<std::string> &lines) -> std::string {
            std::string result;
            for (const std::string &line: lines) {
                result += line + '\n';
            }
            return result;
        }

        [[nodiscard]] auto numberedLines(const std::vector<std::string> &lines) -> std::string {
            std::string text;
            for (size_t index = 0; index < lines.size(); ++index) {
                text += std::to_string(index + 1) + ". " + lines[index] + '\n';
            }
            return text.empty() ? "（空）" : text;
        }

        [[nodiscard]] auto parseLlmJson(const std::optional<std::string> &result, const std::string_view tag,
          const u64 sessionId) -> std::optional<json> {
            if (!result) {
                Logger::error(sessionId, "Memory", fmt::format("{}: API 请求失败", tag));
                return std::nullopt;
            }
            std::string payload;
            if (!tryExtractJsonObject(*result, payload)) {
                Logger::warn(sessionId, "Memory", fmt::format("{}: 响应中无 JSON: {}", tag, result->substr(0, 100)));
                return std::nullopt;
            }
            json parsed;
            if (!tryParseJson(payload, parsed) || !parsed.is_object()) {
                Logger::warn(sessionId, "Memory", fmt::format("{}: JSON 解析失败", tag));
                return std::nullopt;
            }
            return parsed;
        }

        auto extractMemories(std::string records, std::string context, const u64 sessionId)
          -> drogon::Task<std::optional<std::vector<std::string>>> {
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
              parseLlmJson(co_await LlmClient::requestMaintenance(messages, sessionId), "记忆提取", sessionId);
            if (!parsed) {
                co_return std::nullopt;
            }
            const json &values = atOrNull(*parsed, "memories");
            if (!values.is_array()) {
                Logger::warn(sessionId, "Memory", fmt::format("记忆提取: 输出缺少 memories 数组"));
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

        auto recallForMerge(const std::vector<std::string> &memories, const u64 sessionId)
          -> drogon::Task<std::optional<std::vector<SimilarMemory>>> {
            const f32 threshold = static_cast<f32>(Config::instance().longTermRecallThreshold);
            std::unordered_map<i64, SimilarMemory> recalled;
            for (const std::string &memory: memories) {
                const auto embedding = co_await LlmClient::requestEmbedding(memory, sessionId);
                if (!embedding) {
                    Logger::warn(sessionId, "Memory", "记忆召回: 向量化失败，本次维护不提交");
                    co_return std::nullopt;
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
                std::ranges::sort(result, [](const SimilarMemory &left, const SimilarMemory &right) -> bool {
                    return left.similarity > right.similarity;
                });
                result.resize(kMaxRecalledEntries);
            }
            co_return result;
        }

        struct ReconciledLongTermMemory {
            std::vector<i64> sources;
            std::string content;
        };

        struct ReconcileResult {
            std::vector<std::string> shortTerm;
            std::vector<ReconciledLongTermMemory> longTerm;
        };

        auto reconcileMemory(std::vector<std::string> currentShortTerm, const std::vector<std::string> &newMemories,
          const std::vector<SimilarMemory> &recalled, const bool longTermEnabled, const u64 sessionId)
          -> drogon::Task<std::optional<ReconcileResult>> {
            const auto &config = Config::instance();
            std::string prompt =
              std::format(R"(你是一个【群聊记忆整理器】。把新提取的记忆与当前短期记忆、召回的长期记忆合并整理。

规则：去重并合并相关条目；冲突时以新提取的信息为准；每条记忆带人物归属，简短客观。
短期记忆保存临时状态、近期事件和上下文相关内容，最多 {} 条。长期记忆只保留稳定特征、关系和重要约定，宁缺毋滥。
召回的长期记忆没有变化时不要输出；修改或合并时 sources 必须填入被替代条目的编号，全新条目填空数组。
只输出 JSON：{{"shortTerm":["小明正在准备考试"],"longTerm":[{{"sources":[12],"content":"小明喜欢Python"}}]}})",
                config.shortTermMemoryMax);
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
            const auto parsed =
              parseLlmJson(co_await LlmClient::requestMaintenance(messages, sessionId), "记忆整理", sessionId);
            if (!parsed || !atOrNull(*parsed, "shortTerm").is_array()) {
                Logger::warn(sessionId, "Memory", fmt::format("记忆整理: 输出缺少 shortTerm 数组"));
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
                        if (const i64 id = source.get<i64>(); std::ranges::any_of(
                              recalled, [id](const SimilarMemory &item) -> bool { return item.id == id; })) {
                            memory.sources.push_back(id);
                        }
                    }
                    result.longTerm.push_back(std::move(memory));
                }
            }
            co_return result;
        }

        [[nodiscard]] auto projectMessages(const json &messages, const bool includeGroupCard) -> json {
            auto records = json::array();
            for (const auto &message: messages) {
                if (message.is_object()) {
                    records.push_back(MessageRecord::projectForAgent(message, false, includeGroupCard));
                }
            }
            return records;
        }

    } // namespace

    auto MemoryMaintenanceService::prepare(const u64 sessionId, json messages, json contextMessages)
      -> drogon::Task<std::optional<MemoryMaintenanceResult>> {
        const bool includeGroupCard =
          !SessionId::isPrivate(sessionId) && SessionStore::getSessionConfig(sessionId).sendGroupCard;
        const auto records = projectMessages(messages, includeGroupCard);
        if (records.empty()) {
            co_return MemoryMaintenanceResult{};
        }

        const auto &config = Config::instance();
        const auto contextRecords = projectMessages(contextMessages, includeGroupCard);
        const auto extracted = co_await extractMemories(dumpJson(records), dumpJson(contextRecords), sessionId);
        if (!extracted) {
            co_return std::nullopt;
        }
        if (extracted->empty()) {
            co_return MemoryMaintenanceResult{};
        }

        const bool longTermEnabled = !config.embedding.baseUrl.empty() && !config.embedding.model.empty();
        std::vector<SimilarMemory> recalled;
        if (longTermEnabled) {
            auto result = co_await recallForMerge(*extracted, sessionId);
            if (!result) {
                co_return std::nullopt;
            }
            recalled = std::move(*result);
        }
        const auto reconciled = co_await reconcileMemory(
          splitLines(MemoryStore::getShortTermMemory(sessionId)), *extracted, recalled, longTermEnabled, sessionId);
        if (!reconciled) {
            co_return std::nullopt;
        }

        std::vector<PreparedLongTermMemory> prepared;
        std::unordered_set<std::string> contents;
        for (const auto &[sources, content]: reconciled->longTerm) {
            if (!contents.insert(content).second) {
                continue;
            }
            PreparedLongTermMemory item{.content = content, .replacedIds = sources};
            const auto embedding = co_await LlmClient::requestEmbedding(item.content, sessionId);
            if (!embedding) {
                Logger::warn(sessionId, "Memory", "长期记忆向量化失败，本次维护不提交");
                co_return std::nullopt;
            }
            item.embedding = *embedding;
            prepared.push_back(std::move(item));
        }

        co_return MemoryMaintenanceResult{
          .shortTerm = joinLines(reconciled->shortTerm), .longTerm = std::move(prepared)};
    }
} // namespace insoulforge
