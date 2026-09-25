/// @file AffinityMaintenanceService.cpp
/// @brief 基于持久化消息批次的好感度维护实现

#include <infrastructure/NumericTypes.hpp>

#include <conversation/maintenance/affinity/AffinityMaintenanceService.hpp>
#include <conversation/maintenance/affinity/AffinityMaintenanceStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>
namespace insoulforge {
    namespace {
        /// @brief 单次评估允许的最大好感度变化量
        constexpr i32 kMaxAffinityDelta = 5;
        /// @brief 单次好感度评估输入的记录数量上限
        constexpr size_t kMaxAffinityRecords = 300;
        constexpr std::chrono::seconds kInitialRetryDelay{2};
        constexpr std::chrono::seconds kMaxRetryDelay{60};

        /// @brief 每个会话至多运行一个好感度维护消费者
        struct AffinityScheduler {
            std::mutex mutex;
            std::unordered_set<u64> activeSessions;
        };

        [[nodiscard]] auto scheduler() -> AffinityScheduler & {
            static AffinityScheduler instance;
            return instance;
        }

        /// @brief 将记录内容拼接为供 LLM 消费的 JSON 数组
        [[nodiscard]] auto formatRecordsText(const std::vector<json> &records, const size_t limit) -> std::string {
            std::string text = "[";
            for (size_t index = 0; index < limit; ++index) {
                if (index != 0)
                    text += ',';
                text += records[index]["content"].get<std::string>();
            }
            return text + ']';
        }

        /// @brief 解析好感度评估响应；格式错误视为本轮评估失败
        [[nodiscard]] auto parseAffinityDeltas(const std::optional<std::string> &result, const u64 sessionId)
          -> std::optional<json> {
            if (!result) {
                Logger::error(sessionId, "Affinity", fmt::format("好感度评分: API 请求失败"));
                return std::nullopt;
            }

            std::string payload;
            if (!tryExtractJsonObject(*result, payload)) {
                Logger::warn(
                  sessionId, "Affinity", fmt::format("好感度评分: 响应中无 JSON: {}", result->substr(0, 100)));
                return std::nullopt;
            }

            json deltas;
            if (!tryParseJson(payload, deltas) || !deltas.is_object()) {
                Logger::warn(sessionId, "Affinity", fmt::format("好感度评分: JSON 解析失败"));
                return std::nullopt;
            }
            return deltas;
        }
        auto maintainJob(const AffinityMaintenanceJob &job) -> drogon::Task<bool> {
            std::vector<json> records;
            records.reserve(job.messages.size());
            for (const json &message: job.messages) {
                if (message.is_object()) {
                    records.push_back({{"content", dumpJson(MessageRecord::projectForAgent(message))}});
                }
            }
            const size_t limit = std::min(records.size(), kMaxAffinityRecords);
            if (limit == 0) {
                AffinityMaintenanceStore::complete(job.id, job.sessionId, {});
                co_return true;
            }

            json messages;
            json item;
            item["role"] = "system";
            item["content"] = std::format(R"(你是一个 [好感度评估器]。
你需要看完一段群聊记录，评估每个发言用户在这段对话中给 {} 留下的印象变化。

评分规则：
- 只输出一个 JSON 对象，键为用户 QQ 号（字符串），值为好感度变化量（整数，-5 到 5）
- 正面（有趣、友好、真诚分享、帮助、陪伴）给正分；负面（辱骂、恶意挑衅、骚扰、令人不适）给负分
- 普通闲聊、无明显印象变化 → 不要输出该用户
- 机器人自己的消息（qq 为 "self"）和系统消息不要评估
- 只依据对话内容判断，不要虚构记录中不存在的 QQ 号

示例：
{{"123456": 2, "789012": -3}}

没有任何值得调整的变化时输出：{{}})",
              Config::instance().botName);
            messages.push_back(item);
            item.clear();
            item["role"] = "user";
            item["content"] = "=== 群聊记录 ===\n" + formatRecordsText(records, limit) + "\n\n请输出好感度变化 JSON：";
            messages.push_back(item);

            const auto deltas = parseAffinityDeltas(
              co_await LlmClient::requestLLM(std::move(messages), 0.3f, 0.9f, 256, "affinity", job.sessionId),
              job.sessionId);
            if (!deltas) {
                co_return false;
            }

            std::vector<std::pair<u64, i32>> appliedDeltas;
            for (const auto &[qqStr, deltaValue]: deltas->items()) {
                const u64 qqNumber = parseUInt64(qqStr);
                if (qqNumber == 0 || qqNumber == SessionId::kSystemAccountId || !deltaValue.is_number_integer()) {
                    continue;
                }
                if (const i32 delta = std::clamp(jsonToInt(deltaValue), -kMaxAffinityDelta, kMaxAffinityDelta);
                  delta != 0) {
                    appliedDeltas.emplace_back(qqNumber, delta);
                }
            }
            AffinityMaintenanceStore::complete(job.id, job.sessionId, appliedDeltas);
            Logger::info(job.sessionId, "Affinity",
              fmt::format("好感度评分完成: {} 条记录，更新 {} 人", limit, appliedDeltas.size()));
            co_return true;
        }

        [[nodiscard]] auto retryDelay(const i32 attempts) -> std::chrono::seconds {
            const i32 exponent = std::clamp(attempts, 0, 5);
            return std::min(kInitialRetryDelay * (1 << exponent), kMaxRetryDelay);
        }

        auto drainSession(const u64 sessionId) -> drogon::Task<>;

        [[nodiscard]] auto acquireSessionConsumer(const u64 sessionId) -> bool {
            auto &state = scheduler();
            std::lock_guard lock(state.mutex);
            return state.activeSessions.insert(sessionId).second;
        }

        void releaseSessionConsumer(const u64 sessionId) {
            std::lock_guard lock(scheduler().mutex);
            scheduler().activeSessions.erase(sessionId);
        }

        auto drainSession(const u64 sessionId) -> drogon::Task<> {
            try {
                while (const auto job = AffinityMaintenanceStore::next(sessionId)) {
                    bool completed = false;
                    try {
                        completed = co_await maintainJob(*job);
                    } catch (const std::exception &error) {
                        Logger::error(
                          sessionId, "Affinity", fmt::format("好感度任务 #{} 异常: {}", job->id, error.what()));
                    } catch (...) {
                        Logger::error(sessionId, "Affinity", fmt::format("好感度任务 #{} 异常: 未知异常", job->id));
                    }
                    if (completed) {
                        continue;
                    }
                    AffinityMaintenanceStore::incrementAttempt(job->id);
                    const auto delay = retryDelay(job->attemptCount);
                    Logger::warn(
                      sessionId, "Affinity", fmt::format("好感度任务 #{} 将在 {} 秒后重试", job->id, delay.count()));
                    co_await drogon::sleepCoro(drogon::app().getLoop(), delay);
                }
            } catch (const std::exception &error) {
                Logger::error(sessionId, "Affinity", fmt::format("好感度任务消费者异常退出: {}", error.what()));
            } catch (...) {
                Logger::error(sessionId, "Affinity", fmt::format("好感度任务消费者异常退出: 未知异常"));
            }

            co_return;
        }
    } // namespace

    auto AffinityMaintenanceService::processPending(const u64 sessionId) -> drogon::Task<> {
        if (!acquireSessionConsumer(sessionId)) {
            co_return;
        }
        co_await drainSession(sessionId);
        releaseSessionConsumer(sessionId);
        try {
            if (AffinityMaintenanceStore::next(sessionId)) {
                co_await processPending(sessionId);
            }
        } catch (const std::exception &error) {
            Logger::error(sessionId, "Affinity", fmt::format("检查待处理好感度任务失败: {}", error.what()));
        }
        co_return;
    }
} // namespace insoulforge
