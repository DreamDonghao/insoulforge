/// @file MessageService.cpp
/// @brief OneBot 消息服务 - 实现

#include <conversation/message/MessageRecord.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/session/SessionStore.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/MessageService.hpp>
#include <onebot/OneBotClient.hpp>

namespace insoulforge {
    std::string MessageService::convertAtToCQCode(std::string text) {
        std::string result = std::move(text);

        // 格式 @[...数字...] → 提取数字转为 [CQ:at,qq=数字]
        const std::regex atPattern(R"(@\[.*?(\d{5,11}).*?\])");
        result = std::regex_replace(result, atPattern, "[CQ:at,qq=$1]");

        // 2. 模糊格式 @昵称 → 查找昵称映射
        auto nameToQQ = QQNameDirectory::nameToQQMap();

        // 按昵称长度降序排序，避免短昵称先匹配
        std::vector<std::pair<std::string, uint64_t>> sortedNames(nameToQQ.begin(), nameToQQ.end());
        std::ranges::sort(
          sortedNames, [](const auto &a, const auto &b) { return a.first.length() > b.first.length(); });

        for (const auto &[name, qq]: sortedNames) {
            const std::string mention = "@" + name;
            size_t pos = 0;
            while ((pos = result.find(mention, pos)) != std::string::npos) {
                size_t endPos = pos + mention.length();
                bool isComplete =
                  endPos >= result.length() || (!std::isalnum(static_cast<unsigned char>(result[endPos])) &&
                                                 result[endPos] != '_' && result[endPos] != '-');

                if (isComplete) {
                    // 检查是否已经是CQ码的一部分（避免重复转换）
                    if (pos >= 4 && result.substr(pos - 4, 4) == "qq=") {
                        pos = endPos;
                        continue;
                    }
                    std::string cqCode = fmt::format("[CQ:at,qq={}]", qq);
                    result.replace(pos, mention.length(), cqCode);
                    pos += cqCode.length();
                } else {
                    pos = endPos;
                }
            }
        }

        return result;
    }

    namespace {
        /// @brief 发送消息后记录到工作流并推送 WebSocket（群聊/私聊共用）
        /// @param sendTask 发送协程（OneBotClient）
        /// @param processedMessage 已完成 CQ 码转换的消息内容
        /// @param sessionId 会话 ID
        /// @param channelName 日志中的渠道名（"群消息"/"私聊消息"）
        /// @return 发送成功返回 message_id；失败不记聊天记录，返回 nullopt（已记日志）
        drogon::Task<std::optional<uint64_t>> afterSendMessage(drogon::Task<std::optional<uint64_t>> sendTask,
          std::string processedMessage, const uint64_t sessionId, std::string_view channelName) {
            const auto messageId = co_await std::move(sendTask);
            if (!messageId) {
                co_return std::nullopt;
            }

            const json record =
              MessageRecord::createAssistantRecord(Config::instance().botName + "(我)", *messageId, processedMessage);
            // 发送成功后写入所属会话的内存消息列表；列表在正常退出时统一持久化。
            OneBotEventWorkflow::instance().appendDeliveredAssistantMessage(sessionId, record);

            Logger::session(sessionId).info(
              "成功发送{}: {} (message_id={})", channelName, processedMessage, *messageId);
            co_return *messageId;
        }
    } // namespace

    drogon::Task<std::optional<uint64_t>> MessageService::sendGroupMsg(const uint64_t groupId, std::string message) {
        // 转换 @[QQ:xxx] 为 CQ 码
        const std::string processedMessage = convertAtToCQCode(std::move(message));
        co_return co_await afterSendMessage(
          OneBotClient::sendGroupMsg(groupId, processedMessage), processedMessage, groupId, "群消息");
    }

    drogon::Task<std::optional<uint64_t>> MessageService::sendPrivateMsg(const uint64_t userId, std::string message) {
        const std::string processedMessage = convertAtToCQCode(std::move(message));
        co_return co_await afterSendMessage(
          OneBotClient::sendPrivateMsg(userId, processedMessage, SessionId::fromPrivateUser(userId)), processedMessage,
          SessionId::fromPrivateUser(userId), "私聊消息");
    }

    drogon::Task<std::string> MessageService::fetchAndUpdateSessionName(const uint64_t sessionId) {
        std::string name;
        if (SessionId::isPrivate(sessionId)) {
            // 私聊会话取 QQ 昵称，复用 groupName 列存储
            const uint64_t userId = SessionId::privateUserId(sessionId);
            const auto resp = co_await OneBotClient::getStrangerInfo(userId, sessionId);
            name = getStr(atOrNull(resp, "data"), "nickname");
        } else {
            const auto result = co_await OneBotClient::getGroupInfo(sessionId);
            name = getStr(atOrNull(result, "data"), "group_name");
        }
        if (!name.empty()) {
            SessionStore::updateSessionName(sessionId, name);
        }

        co_return name;
    }
} // namespace insoulforge
