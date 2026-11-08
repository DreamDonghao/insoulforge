/// @file MessageService.cpp
/// @brief 消息发送与发送结果上下文记录

#include <utility>

#include <fmt/format.h>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/UserId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/messaging/MessageFormatter.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/EventWorkflow.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/MessageService.hpp>

namespace insoulforge::MessageService {
    auto sendToSession(const SessionId sessionId, std::string message, std::optional<std::string> imageDescription,
      json toolHistory, std::string imageId, json connection) -> drogon::Task<std::expected<u64, std::string>> {
        // 群聊与私聊使用相同的格式转换及发送结果记录，只在协议边界选择接口。
        const std::string processedMessage = message.starts_with("[CQ:image,file=base64://")
                                               ? std::move(message)
                                               : MessageFormatter::convertAtToCQCode(std::move(message));
        auto sendTask = sessionId.isPrivate() ? OneBotClient::sendPrivateMsg(
                                                  UserId{sessionId.targetId()}, processedMessage, sessionId, connection)
                                              : OneBotClient::sendGroupMsg(sessionId, processedMessage, connection);
        const auto messageId = co_await std::move(sendTask);
        const auto recordMessageId = messageId ? std::optional<u64>{*messageId} : std::nullopt;

        const auto storedMessage =
          processedMessage.starts_with("[CQ:image,file=base64://") ? std::string("[生成图片]") : processedMessage;
        const auto senderName = Config::instance().botName + "(我)";
        json record = imageDescription && processedMessage.starts_with("[CQ:image,file=")
                        ? MessageRecord::createAssistantGeneratedImageRecord(
                            senderName, recordMessageId, *imageDescription, imageId)
                        : MessageRecord::createAssistantRecord(senderName, recordMessageId, storedMessage);
        if (!messageId) {
            record["send_error"] = messageId.error();
        }
        if (toolHistory.is_array() && !toolHistory.empty()) {
            record["tool_history"] = std::move(toolHistory);
        }
        // 发送结果写入上下文，但不作为新消息触发回复，避免失败后循环重试。
        if (!messageId) {
            EventWorkflow::instance().appendAssistantExecutionRecord(sessionId, std::move(record));
            co_return std::unexpected(messageId.error());
        }
        EventWorkflow::instance().appendDeliveredAssistantMessage(sessionId, std::move(record));
        Logger::info(sessionId, "OneBot",
          fmt::format("成功发送{}: {} (message_id={})", sessionId.isPrivate() ? "私聊消息" : "群消息", storedMessage,
            *messageId));
        co_return *messageId;
    }
} // namespace insoulforge::MessageService
