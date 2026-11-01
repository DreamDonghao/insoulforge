/// @file CommandProcessor.cpp
/// @brief 消息工作流的管理命令处理实现

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/access/AdminStore.hpp>
#include <workflow/access/BlacklistStore.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/image/ImageDescriptionStore.hpp>
#include <workflow/session/SessionConfigManager.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge::CommandProcessor {
    namespace {
        /// @brief 跳过命令文本开头的空白字符
        [[nodiscard]] auto firstCommandCharacter(const std::string_view text) -> size_t {
            size_t position = 0;
            while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) {
                ++position;
            }
            return position;
        }

        /// @brief 提取以 `/` 开头的完整命令文本
        [[nodiscard]] auto commandText(const json &message) -> std::string {
            const std::string text = MessageRecord::extractText(message);
            const size_t position = firstCommandCharacter(text);
            return position < text.size() && text[position] == '/' ? text.substr(position) : std::string{};
        }
    } // namespace

    auto isCommand(const json &message) -> bool {
        const auto parsedSession = SessionId::parse(getStr(message, "session_id"));
        if (!parsedSession)
            return false;
        const auto sessionId = *parsedSession;
        if (sessionId.isPrivate() || !MessageRecord::mentions(message, UserId{Config::instance().selfQQNumber})) {
            return false;
        }
        if (commandText(message).empty())
            return false;
        return SessionStore::getSessionConfig(sessionId).commandEnabled;
    }

    auto execute(const json &message) -> drogon::Task<std::string> {
        const SessionId sessionId = SessionId::parse(getStr(message, "session_id")).value();
        const auto sender = UserId::parse(getStr(atOrNull(message, "sender"), "user_id"));
        const bool hasPermission = sender && AdminStore::isAdmin(*sender);

        std::istringstream input(commandText(message));
        std::string command;
        input >> command;

        if (command == "/help" || command == "/帮助") {
            co_return "可用命令:\n"
                      "【会话管理】\n"
                      "/enable [群号] - 启用指定群聊，省略时为当前群\n"
                      "/disable [群号] - 禁用指定群聊，省略时为当前群\n"
                      "/groups - 查看启用的会话列表\n"
                      "/status - 查看当前会话状态\n"
                      "/groupcard on|off - 开启或关闭本群向模型发送群名片\n"
                      "/autoreply on|off - 开启或关闭本群普通消息自动回复\n"
                      "【管理员】\n"
                      "/admins - 查看管理员列表\n"
                      "/addadmin <用户 ID> - 添加管理员\n"
                      "/deladmin <用户 ID> - 移除管理员\n"
                      "/blacklist - 查看黑名单\n"
                      "/addblacklist <用户 ID> - 添加黑名单\n"
                      "/delblacklist <用户 ID> - 移除黑名单\n"
                      "/clearimagecache - 清除全部图片识别缓存\n"
                      "【表情管理】\n"
                      "/delemoji <名称> - 删除表情包\n"
                      "/listemoji - 查看表情包列表\n"
                      "【其他】\n"
                      "/help - 显示帮助\n"
                      "/about - 关于本项目\n\n"
                      "注意: 管理命令仅限管理员使用";
        }
        if (command == "/status" || command == "/状态") {
            const bool enabled = SessionStore::isSessionEnabled(sessionId);
            const auto config = SessionConfigManager::getConfig(sessionId);
            co_return fmt::format("会话 {} 状态:\n- 启用: {}\n- 消息数: {}\n- 发送群名片: {}\n- 自动回复: {}",
              sessionId, enabled ? "是" : "否", config.allMesCount, config.sendGroupCard ? "是" : "否",
              config.autoReply ? "是" : "否");
        }
        if (command == "/admins" || command == "/管理员") {
            const auto admins = AdminStore::getAdmins();
            if (admins.empty()) {
                co_return "暂无管理员";
            }
            std::string response = "管理员列表:\n";
            for (const auto &userId: admins) {
                response += fmt::format("- {}\n", userId);
            }
            co_return response;
        }
        if (command == "/about" || command == "/关于") {
            co_return "InSoulForge\n"
                      "基于 Agent 架构，支持自定义角色、长期记忆、多工具调用\n\n"
                      "项目地址: https://github.com/DreamDonghao/insoulforge\n"
                      "项目发起人: DreamDonghao\n"
                      "贡献者: https://github.com/DreamDonghao/insoulforge/graphs/contributors\n"
                      "许可证: AGPL-3.0-only";
        }
        if (!hasPermission) {
            co_return fmt::format("权限不足，你({})不是管理员", getStr(atOrNull(message, "sender"), "user_id"));
        }
        if (command == "/enable" || command == "/启用") {
            SessionId targetSession = sessionId;
            if (std::string argument; input >> argument) {
                if (const auto parsed = UserId::parse(argument)) {
                    targetSession = SessionId::group(argument);
                } else {
                    co_return "无效的ID格式";
                }
            }
            SessionStore::enableSession(targetSession);
            co_return fmt::format("已启用会话: {}", targetSession);
        }
        if (command == "/disable" || command == "/禁用") {
            SessionId targetSession = sessionId;
            if (std::string argument; input >> argument) {
                if (const auto parsed = UserId::parse(argument)) {
                    targetSession = SessionId::group(argument);
                } else {
                    co_return "无效的ID格式";
                }
            }
            SessionStore::disableSession(targetSession);
            co_return fmt::format("已禁用会话: {}", targetSession);
        }
        if (command == "/autoreply" || command == "/自动回复") {
            if (sessionId.isPrivate())
                co_return "此命令仅限群聊使用";
            std::string argument, extra;
            input >> argument;
            if ((argument != "on" && argument != "off") || (input >> extra))
                co_return "用法: /autoreply on|off";
            SessionStore::setAutoReply(sessionId, argument == "on");
            co_return argument == "on" ? "已开启本群普通消息自动回复"
                                       : "已关闭本群普通消息自动回复，@机器人仍可触发回复";
        }
        if (command == "/groupcard" || command == "/群名片") {
            if (sessionId.isPrivate())
                co_return "此命令仅限群聊使用";
            std::string argument, extra;
            input >> argument;
            if ((argument != "on" && argument != "off") || (input >> extra))
                co_return "用法: /groupcard on|off";
            SessionStore::setSendGroupCard(sessionId, argument == "on");
            co_return argument == "on" ? "已开启本群向模型发送群名片" : "已关闭本群向模型发送群名片";
        }
        if (command == "/groups" || command == "/群列表") {
            const auto groups = SessionStore::getEnabledSessions();
            if (groups.empty()) {
                co_return "没有启用的群聊";
            }
            std::string response = "启用的群聊列表:\n";
            for (const auto &groupId: groups) {
                response += fmt::format("- {}\n", groupId);
            }
            co_return response;
        }
        if (command == "/addadmin" || command == "/添加管理员") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /addadmin <用户 ID>";
            }
            if (const auto userId = UserId::parse(argument)) {
                AdminStore::addAdmin(*userId);
                co_return fmt::format("已添加管理员: {}", *userId);
            }
            co_return "无效的用户 ID格式";
        }
        if (command == "/deladmin" || command == "/移除管理员") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /deladmin <用户 ID>";
            }
            if (const auto userId = UserId::parse(argument)) {
                AdminStore::removeAdmin(*userId);
                co_return fmt::format("已移除管理员: {}", *userId);
            }
            co_return "无效的用户 ID格式";
        }
        if (command == "/blacklist" || command == "/黑名单") {
            const auto users = BlacklistStore::getAll();
            if (users.empty()) {
                co_return "黑名单为空";
            }
            std::string response = "黑名单:\n";
            for (const auto &userId: users) {
                response += fmt::format("- {}\n", userId);
            }
            co_return response;
        }
        if (command == "/addblacklist" || command == "/添加黑名单") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /addblacklist <用户 ID>";
            }
            if (const auto userId = UserId::parse(argument); userId) {
                BlacklistStore::add(*userId);
                co_return fmt::format("已添加黑名单: {}", *userId);
            }
            co_return "无效的用户 ID格式";
        }
        if (command == "/delblacklist" || command == "/移除黑名单") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /delblacklist <用户 ID>";
            }
            if (const auto userId = UserId::parse(argument); userId) {
                BlacklistStore::remove(*userId);
                co_return fmt::format("已移除黑名单: {}", *userId);
            }
            co_return "无效的用户 ID格式";
        }
        if (command == "/clearimagecache" || command == "/清除图片缓存") {
            co_return fmt::format("已清除 {} 条图片识别缓存", ImageDescriptionStore::clearAll());
        }
        if (command == "/delemoji" || command == "/删除表情") {
            std::string name;
            if (!(input >> name)) {
                co_return "用法: /delemoji <名称或序号>";
            }
            const json emoji = co_await ToolRuntime::findFavoriteEmoji(name);
            if (emoji.is_null()) {
                co_return fmt::format("收藏表情中找不到'{}'", name);
            }
            if (!co_await OneBotClient::deleteCustomFace(getStr(emoji, "res_id"))) {
                co_return fmt::format("删除失败: {}（QQ 客户端操作失败）", name);
            }
            ToolRuntime::invalidateFavoriteEmojiCache();
            co_return fmt::format("已从收藏表情中删除: {}", getStr(emoji, "name"));
        }
        if (command == "/listemoji" || command == "/表情列表") {
            const json emojis = co_await ToolRuntime::fetchFavoriteEmojis();
            if (emojis.empty()) {
                co_return "QQ收藏表情为空或获取失败";
            }
            std::string response = "收藏表情列表:\n";
            for (const json &emoji: emojis) {
                response += fmt::format("- {}\n", getStr(emoji, "name"));
            }
            co_return fmt::format("{}\n共 {} 个表情", response, emojis.size());
        }
        co_return fmt::format("未知命令: {}\n使用 /help 查看可用命令", command);
    }
} // namespace insoulforge::CommandProcessor
