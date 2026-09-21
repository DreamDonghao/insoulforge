/// @file CommandProcessor.cpp
/// @brief 消息工作流的管理命令处理实现

#include <admin/AdminStore.hpp>
#include <admin/BlacklistStore.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/SessionConfigManager.hpp>
#include <infrastructure/config/Config.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <onebot/OneBotClient.hpp>

namespace insoulforge::CommandProcessor {
    namespace {
        /// @brief 跳过命令文本开头的空白字符
        [[nodiscard]] size_t firstCommandCharacter(const std::string_view text) {
            size_t position = 0;
            while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) {
                ++position;
            }
            return position;
        }

        /// @brief 提取以 `/` 开头的完整命令文本
        [[nodiscard]] std::string commandText(const json &message) {
            const std::string text = MessageRecord::extractText(message);
            const size_t position = firstCommandCharacter(text);
            return position < text.size() && text[position] == '/' ? text.substr(position) : std::string{};
        }
    } // namespace

    bool isCommand(const json &message) {
        const uint64_t sessionId = getUInt(message, "session_id");
        if (sessionId == 0 ||
            (!SessionId::isPrivate(sessionId) && !MessageRecord::mentions(message, Config::instance().selfQQNumber))) {
            return false;
        }
        return !commandText(message).empty();
    }

    drogon::Task<std::string> execute(const json &message) {
        const uint64_t sessionId = getUInt(message, "session_id");
        const uint64_t senderQQ = getUInt(atOrNull(message, "sender"), "qq");
        const bool hasPermission = AdminStore::isAdmin(senderQQ);

        std::istringstream input(commandText(message));
        std::string command;
        input >> command;

        if (command == "/help" || command == "/帮助") {
            co_return "可用命令:\n"
                      "【会话管理】\n"
                      "/enable [会话ID] - 启用当前会话（群聊传群号，私聊可不带参数）\n"
                      "/disable [会话ID] - 禁用当前会话（私聊可不带参数）\n"
                      "/groups - 查看启用的会话列表\n"
                      "/status - 查看当前会话状态\n"
                      "【管理员】\n"
                      "/admins - 查看管理员列表\n"
                      "/addadmin <QQ号> - 添加管理员\n"
                      "/deladmin <QQ号> - 移除管理员\n"
                      "/blacklist - 查看黑名单\n"
                      "/addblacklist <QQ号> - 添加黑名单\n"
                      "/delblacklist <QQ号> - 移除黑名单\n"
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
            const SessionConfig config = SessionConfigManager::getConfig(sessionId);
            co_return fmt::format(
              "会话 {} 状态:\n- 启用: {}\n- 消息数: {}", sessionId, enabled ? "是" : "否", config.allMesCount);
        }
        if (command == "/admins" || command == "/管理员") {
            const auto admins = AdminStore::getAdmins();
            if (admins.empty()) {
                co_return "暂无管理员";
            }
            std::string response = "管理员列表:\n";
            for (const uint64_t qq: admins) {
                response += fmt::format("- {}\n", qq);
            }
            co_return response;
        }
        if (command == "/about" || command == "/关于") {
            co_return "InSoulForge\n"
                      "基于 Agent 架构，支持自定义角色、长期记忆、多工具调用\n\n"
                      "项目地址: https://github.com/DreamDonghao/insoulforge\n"
                      "作者: DreamDonghao\n"
                      "许可证: AGPL-3.0-only";
        }
        if (!hasPermission) {
            co_return fmt::format("权限不足，你({})不是管理员", senderQQ);
        }
        if (command == "/enable" || command == "/启用") {
            uint64_t targetSession = sessionId;
            if (std::string argument; input >> argument) {
                if (const auto parsed = tryParseUInt64(argument)) {
                    targetSession = *parsed;
                } else {
                    co_return "无效的ID格式";
                }
            }
            SessionStore::enableSession(targetSession);
            co_return fmt::format("已启用会话: {}", targetSession);
        }
        if (command == "/disable" || command == "/禁用") {
            uint64_t targetSession = sessionId;
            if (std::string argument; input >> argument) {
                if (const auto parsed = tryParseUInt64(argument)) {
                    targetSession = *parsed;
                } else {
                    co_return "无效的ID格式";
                }
            }
            SessionStore::disableSession(targetSession);
            co_return fmt::format("已禁用会话: {}", targetSession);
        }
        if (command == "/groups" || command == "/群列表") {
            const auto groups = SessionStore::getEnabledGroups();
            if (groups.empty()) {
                co_return "没有启用的群聊";
            }
            std::string response = "启用的群聊列表:\n";
            for (const uint64_t groupId: groups) {
                response += fmt::format("- {}\n", groupId);
            }
            co_return response;
        }
        if (command == "/addadmin" || command == "/添加管理员") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /addadmin <QQ号>";
            }
            if (const auto qq = tryParseUInt64(argument)) {
                AdminStore::addAdmin(*qq);
                co_return fmt::format("已添加管理员: {}", *qq);
            }
            co_return "无效的QQ号格式";
        }
        if (command == "/deladmin" || command == "/移除管理员") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /deladmin <QQ号>";
            }
            if (const auto qq = tryParseUInt64(argument)) {
                AdminStore::removeAdmin(*qq);
                co_return fmt::format("已移除管理员: {}", *qq);
            }
            co_return "无效的QQ号格式";
        }
        if (command == "/blacklist" || command == "/黑名单") {
            const auto users = BlacklistStore::getAll();
            if (users.empty()) {
                co_return "黑名单为空";
            }
            std::string response = "黑名单:\n";
            for (const uint64_t qq: users) {
                response += fmt::format("- {}\n", qq);
            }
            co_return response;
        }
        if (command == "/addblacklist" || command == "/添加黑名单") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /addblacklist <QQ号>";
            }
            if (const auto qq = tryParseUInt64(argument); qq && *qq != 0) {
                BlacklistStore::add(*qq);
                co_return fmt::format("已添加黑名单: {}", *qq);
            }
            co_return "无效的QQ号格式";
        }
        if (command == "/delblacklist" || command == "/移除黑名单") {
            std::string argument;
            if (!(input >> argument)) {
                co_return "用法: /delblacklist <QQ号>";
            }
            if (const auto qq = tryParseUInt64(argument); qq && *qq != 0) {
                BlacklistStore::remove(*qq);
                co_return fmt::format("已移除黑名单: {}", *qq);
            }
            co_return "无效的QQ号格式";
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
