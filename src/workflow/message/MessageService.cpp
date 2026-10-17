/// @file MessageService.cpp
/// @brief 消息发送与发送结果上下文记录

#include <string_view>
#include <utility>

#include <fmt/format.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/messaging/MessageFormatter.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/MessageService.hpp>
#include <workflow/message/SessionId.hpp>

namespace insoulforge {
    namespace {
        /// @brief 发送消息后记录到工作流并推送 WebSocket（群聊/私聊共用）
        /// @param sendTask 发送协程（OneBotClient）
        /// @param processedMessage 已完成 CQ 码转换的消息内容
        /// @param sessionId 会话 ID
        /// @param channelName 日志中的渠道名（"群消息"/"私聊消息"）
        /// @param imageDescription
        /// @param toolHistory
        /// @return 发送成功返回 message_id；失败保留带 send_error 的上下文记录，并返回具体错误。
        auto afterSendMessage(drogon::Task<std::expected<u64, std::string>> sendTask, std::string processedMessage,
          const u64 sessionId, std::string_view channelName, std::optional<std::string> imageDescription,
          json toolHistory) -> drogon::Task<std::expected<u64, std::string>> {
            const auto messageId = co_await std::move(sendTask);
            const auto recordMessageId = messageId ? std::optional<u64>{*messageId} : std::nullopt;

            const auto storedMessage =
              processedMessage.starts_with("[CQ:image,file=base64://") ? std::string("[生成图片]") : processedMessage;
            const auto senderName = Config::instance().botName + "(我)";
            json record =
              imageDescription && processedMessage.starts_with("[CQ:image,file=")
                ? MessageRecord::createAssistantGeneratedImageRecord(senderName, recordMessageId, *imageDescription)
                : MessageRecord::createAssistantRecord(senderName, recordMessageId, storedMessage);
            if (!messageId) {
                record["send_error"] = messageId.error();
            }
            if (toolHistory.is_array() && !toolHistory.empty()) {
                record["tool_history"] = std::move(toolHistory);
            }
            // 发送结果写入上下文，但不作为新消息触发回复，避免失败后循环重试。
            if (!messageId) {
                OneBotEventWorkflow::instance().appendAssistantExecutionRecord(sessionId, std::move(record));
                co_return std::unexpected(messageId.error());
            }
            OneBotEventWorkflow::instance().appendDeliveredAssistantMessage(sessionId, std::move(record));
            Logger::info(sessionId, "OneBot",
              fmt::format("成功发送{}: {} (message_id={})", channelName, storedMessage, *messageId));
            co_return *messageId;
        }
    } // namespace

    auto MessageService::sendGroupMsg(const u64 groupId, std::string message,
      std::optional<std::string> imageDescription, json toolHistory) -> drogon::Task<std::expected<u64, std::string>> {
        // 转换 @[QQ:xxx] 为 CQ 码
        const std::string processedMessage = message.starts_with("[CQ:image,file=base64://")
                                               ? std::move(message)
                                               : MessageFormatter::convertAtToCQCode(std::move(message));
        co_return co_await afterSendMessage(OneBotClient::sendGroupMsg(groupId, processedMessage), processedMessage,
          groupId, "群消息", std::move(imageDescription), std::move(toolHistory));
    }

    auto MessageService::sendPrivateMsg(const u64 userId, std::string message,
      std::optional<std::string> imageDescription, json toolHistory) -> drogon::Task<std::expected<u64, std::string>> {
        const std::string processedMessage = message.starts_with("[CQ:image,file=base64://")
                                               ? std::move(message)
                                               : MessageFormatter::convertAtToCQCode(std::move(message));
        co_return co_await afterSendMessage(
          OneBotClient::sendPrivateMsg(userId, processedMessage, SessionId::fromPrivateUser(userId)), processedMessage,
          SessionId::fromPrivateUser(userId), "私聊消息", std::move(imageDescription), std::move(toolHistory));
    }
} // namespace insoulforge
