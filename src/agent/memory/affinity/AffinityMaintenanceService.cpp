/// @file AffinityMaintenanceService.cpp
/// @brief 从消息批次评估好感度变化

#include <agent/memory/affinity/AffinityMaintenanceService.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/SessionStore.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/LlmClient.hpp>
namespace insoulforge {
    namespace {
        /// @brief 单次评估允许的最大好感度变化量
        constexpr i32 kMaxAffinityDelta = 5;
        /// @brief 单次好感度评估输入的记录数量上限
        constexpr size_t kMaxAffinityRecords = 300;
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
    } // namespace

    auto AffinityMaintenanceService::prepare(const u64 sessionId, json messages)
      -> drogon::Task<std::optional<AffinityDeltas>> {
        const bool includeGroupCard =
          !SessionId::isPrivate(sessionId) && SessionStore::getSessionConfig(sessionId).sendGroupCard;
        auto records = json::array();
        for (const json &message: messages) {
            if (records.size() == kMaxAffinityRecords) {
                break;
            }
            if (message.is_object()) {
                records.push_back(MessageRecord::projectForAgent(message, false, includeGroupCard));
            }
        }
        if (records.empty()) {
            co_return AffinityDeltas{};
        }

        json promptMessages;
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
        promptMessages.push_back(item);
        item.clear();
        item["role"] = "user";
        item["content"] = "=== 群聊记录 ===\n" + dumpJson(records) + "\n\n请输出好感度变化 JSON：";
        promptMessages.push_back(item);

        const auto deltas = parseAffinityDeltas(
          co_await LlmClient::requestMaintenance(std::move(promptMessages), sessionId, "affinity", 256), sessionId);
        if (!deltas) {
            co_return std::nullopt;
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
        co_return appliedDeltas;
    }
} // namespace insoulforge
