/// @file MemoryMaintenanceService.cpp
/// @brief 从消息批次提取并整理聊天记忆

#include <set>
#include <unordered_set>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/memory/LongTermMemoryStore.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge {
    namespace {
        constexpr i32 kRecallPerMemory = 2;
        constexpr size_t kMaxRecalledEntries = 20;
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

        auto extractMemories(const json &records, const json &context, const u64 sessionId)
          -> drogon::Task<std::optional<MemoryMaintenanceResult>> {
            std::unordered_set<u64> participants;
            for (const auto &record: records) {
                const auto qq = parseUInt64(getStr(atOrNull(record, "sender"), "qq"));
                if (qq > 0 && qq != SessionId::kSystemAccountId)
                    participants.insert(qq);
            }
            if (participants.empty())
                co_return MemoryMaintenanceResult{};
            const json messages = json::array({
              {{"role", "system"},
                {"content", R"(你是一个【人物记忆提取器】，同时评估用户给机器人留下的印象变化。群聊和私聊采用同样规则。

人物记忆按真实发送者 QQ 归属；每条只包含一条完整信息，简短客观；不推测、不扩写。
发送者昵称、群名片、@ 对象名称等身份元数据只用于识别人，不直接提取为昵称或外号记忆；只有聊天正文明确表达称呼约定时才记录别称。
着重提取外号别称、喜好、习惯、关系和重要约定。
不提取一次性玩笑、情绪宣泄、系统指令、控制信息、固定套话或无价值内容。
好感度变化 delta 为 -5 到 5 的整数；友好、帮助、陪伴等正面印象给正分，辱骂、骚扰等负面印象给负分；普通聊天没有明显变化不输出。
两个结果相互独立：有记忆不一定有好感度变化，反之亦然；两个数组都允许为空，涉及的人也可以不同。
只允许评估或记录“待提取消息”中实际发言的用户，不输出机器人自身（qq 为 self）或系统账号，不虚构 QQ，不重复输出同一 QQ 的好感度。
“补充上下文”仅用于消除歧义，绝不能从中提取记忆或评估好感度。
只输出 JSON 对象：{"memories":[{"qq":"123456","entries":["喜欢Python"]}],"affinity_changes":[{"qq":"789012","delta":2}]}。
没有值得记住或调整的内容时输出：{"memories":[],"affinity_changes":[]})"}},
              {{"role", "user"},
                {"content", "=== 待提取消息 ===\n" + dumpJson(records) + "\n=== 补充上下文（不得提取） ===\n" +
                              dumpJson(context) + "\n\n请输出提取结果 JSON："}},
            });
            const auto parsed =
              parseLlmJson(co_await LlmClient::requestMaintenance(messages, sessionId), "记忆提取", sessionId);
            if (!parsed) {
                co_return std::nullopt;
            }
            const json &values = atOrNull(*parsed, "memories");
            const auto &affinity = atOrNull(*parsed, "affinity_changes");
            if (!values.is_array() || !affinity.is_array()) {
                Logger::warn(sessionId, "Memory", "记忆提取: 缺少 memories 或 affinity_changes 数组");
                co_return std::nullopt;
            }
            MemoryMaintenanceResult result;
            std::set<std::pair<u64, std::string>> seenEntries;
            for (const json &value: values) {
                const auto qq = parseUInt64(getStr(value, "qq"));
                if (!value.is_object() || !participants.contains(qq) || !atOrNull(value, "entries").is_array()) {
                    Logger::warn(sessionId, "Memory", "人物记忆格式或 QQ 归属无效，本批次不提交");
                    co_return std::nullopt;
                }
                for (const auto &entry: value["entries"]) {
                    if (!entry.is_string())
                        co_return std::nullopt;
                    if (auto content = trim(entry.get<std::string>());
                      !content.empty() && seenEntries.emplace(qq, content).second) {
                        result.personEntries.push_back({qq, std::move(content)});
                    }
                }
            }
            std::unordered_set<u64> seenAffinity;
            for (const auto &value: affinity) {
                const auto qq = parseUInt64(getStr(value, "qq"));
                const auto &delta = atOrNull(value, "delta");
                if (!value.is_object() || !participants.contains(qq) || !delta.is_number_integer() ||
                    !seenAffinity.insert(qq).second) {
                    Logger::warn(sessionId, "Memory", "好感度格式、QQ 归属或重复项无效，本批次不提交");
                    co_return std::nullopt;
                }
                const auto bounded = static_cast<i32>(std::clamp(jsonToDouble(delta), -5.0, 5.0));
                if (bounded != 0)
                    result.affinity.emplace_back(qq, bounded);
            }
            co_return result;
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
            json shortTerm;
            std::vector<ReconciledLongTermMemory> longTerm;
        };

        auto reconcileMemory(const std::string &currentShortTerm, const json &records, const json &contextRecords,
          const std::vector<std::string> &newMemories, const std::vector<SimilarMemory> &recalled,
          const bool longTermEnabled, const u64 sessionId) -> drogon::Task<std::optional<ReconcileResult>> {
            const auto current = MemoryStore::projectShortTermMemory(currentShortTerm);
            const bool migratingLegacy = current.is_array();
            const auto legacy = migratingLegacy ? current : current["legacy_entries"];
            std::string prompt =
              R"(你是一个【会话记忆整理器】。群聊和私聊采用同样的会话内整理规则。

shortTerm 包含当前会话的 aliases、recent_events，以及仅用于旧内容过渡的 legacy_entries。新消息的个人喜好、职业、性格等人物信息不可写入这些字段。
aliases 按有明确依据的真实 QQ 归属，保存本会话的称呼和外号，合并去重；保留已有称呼，有明确纠正时才更新，不把一次性调侃当成固定外号，也不因事件过期而删除称呼。
发送者昵称、群名片、@ 对象名称等身份元数据只用于识别人，不直接提取为昵称或外号记忆；只有聊天正文明确表达称呼约定时才记录别称。
recent_events 记录本会话发生的事情和最新进展，不记流水账；同一事件更新合并，没有相关新信息时保留。可根据完成状态和后续价值移除过时事件，不使用固定过期天数，不限制条数。
直接依据“当前短期记忆”和“待总结消息”更新，即使没有“新提取的记忆”也要整理会话事件。“补充上下文”只消除歧义，不可直接提取其中的新内容。
legacy_entries 只保留“已有过渡记忆”中的原文条目，不能新增或改写，也不能从当前消息或人物档案补入内容。后续可根据新证据、重复程度和相关性逐步删去过时或已归类的内容，不设固定天数，不因旧内容是个人属性就立即清空。没有明确理由时保留，不要一次性遗忘全部内容。
旧短期记忆可能是纯文本：首次转换时保留全部旧条目到 legacy_entries，后续再逐步整理；不得据此删除、修改或迁移任何全局人物记忆与好感度。
内部工具执行记录、发送失败的尝试不是已经在会话中发生的互动，不可当作已完成事件。
longTerm 沿用原规则，仅依据“新提取的记忆”和“召回的长期记忆”，去重合并，冲突时以新提取的信息为准；每条带人物归属，简短客观，只保留稳定特征、关系和重要约定，宁缺毋滥。不要从新增的会话消息材料额外提取长期记忆。
召回的长期记忆没有变化时不要输出；修改或合并时 sources 必须填入被替代条目的编号，全新条目填空数组。
只输出 JSON：{"shortTerm":{"aliases":[{"qq":"123456","names":["老王"]}],"recent_events":["正在组织周末联机，时间尚未确定"],"legacy_entries":["已有旧条目原文"]},"longTerm":[{"sources":[12],"content":"小明喜欢Python"}]}。没有内容的数组为空。)";
            if (!longTermEnabled) {
                prompt += "\n本批次没有新人物记忆或长期记忆库不可用：longTerm 必须为空数组。";
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
                {"content", "=== 当前短期记忆 ===\n" + currentShortTerm + "\n=== 已有过渡记忆 ===\n" +
                              dumpJson(legacy) + "\n=== 待总结消息 ===\n" + dumpJson(records) +
                              "\n=== 补充上下文（不得提取） ===\n" + dumpJson(contextRecords) +
                              "\n=== 新提取的记忆 ===\n" + numberedLines(newMemories) + "\n=== 召回的长期记忆 ===\n" +
                              recalledText + "\n请输出整理结果 JSON："}},
            });
            const auto parsed =
              parseLlmJson(co_await LlmClient::requestMaintenance(messages, sessionId), "记忆整理", sessionId);
            auto shortTerm =
              parsed ? MemoryStore::normalizeSessionMemory(atOrNull(*parsed, "shortTerm")) : std::nullopt;
            if (!shortTerm) {
                Logger::warn(sessionId, "Memory", "记忆整理: shortTerm 的 aliases 或 recent_events 格式无效");
                co_return std::nullopt;
            }
            // 首次转换完整保留；旧版响应未输出过渡区时也不能被默认为清空。
            if (migratingLegacy || !(*parsed)["shortTerm"].contains("legacy_entries")) {
                (*shortTerm)["legacy_entries"] = legacy;
            } else {
                for (const auto &entry: (*shortTerm)["legacy_entries"]) {
                    if (std::ranges::find(legacy, entry) == legacy.end()) {
                        Logger::warn(sessionId, "Memory", "记忆整理: 过渡区出现非已有条目，本批次不提交");
                        co_return std::nullopt;
                    }
                }
            }

            ReconcileResult result;
            result.shortTerm = std::move(*shortTerm);

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

        [[nodiscard]] auto projectMessages(const json &messages) -> json {
            auto records = json::array();
            for (const auto &message: messages) {
                if (message.is_object()) {
                    auto record = MessageRecord::projectForAgent(message, false, false);
                    // 旧的召回结果不能作为本批新事实再次提取。
                    record.erase("memories");
                    records.push_back(std::move(record));
                }
            }
            return records;
        }

    } // namespace

    auto MemoryMaintenanceService::prepare(const u64 sessionId, json messages, json contextMessages)
      -> drogon::Task<std::optional<MemoryMaintenanceResult>> {
        const auto records = projectMessages(messages);
        if (records.empty()) {
            co_return MemoryMaintenanceResult{};
        }

        const auto &config = Config::instance();
        const auto contextRecords = projectMessages(contextMessages);
        auto extracted = co_await extractMemories(records, contextRecords, sessionId);
        if (!extracted) {
            co_return std::nullopt;
        }

        std::vector<std::string> newMemories;
        for (const auto &[qq, content]: extracted->personEntries) {
            newMemories.push_back(std::to_string(qq) + ": " + content);
        }

        const bool longTermEnabled =
          !newMemories.empty() && !config.embedding.baseUrl.empty() && !config.embedding.model.empty();
        std::vector<SimilarMemory> recalled;
        if (longTermEnabled) {
            auto result = co_await recallForMerge(newMemories, sessionId);
            if (!result) {
                co_return std::nullopt;
            }
            recalled = std::move(*result);
        }
        const auto reconciled = co_await reconcileMemory(MemoryStore::getShortTermMemory(sessionId), records,
          contextRecords, newMemories, recalled, longTermEnabled, sessionId);
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

        extracted->shortTerm = dumpJson(reconciled->shortTerm);
        extracted->longTerm = std::move(prepared);
        co_return std::move(*extracted);
    }
} // namespace insoulforge
