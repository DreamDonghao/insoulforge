#include <admin/AdminStore.hpp>
#include <admin/BlacklistStore.hpp>
#include <admin/auth/AdminAccessToken.hpp>
#include <admin/http/AdminController.hpp>
#include <admin/http/AdminResponse.hpp>
#include <agent/memory/LongTermMemoryStore.hpp>
#include <agent/memory/MemoryStore.hpp>
#include <agent/runtime/AgentSystem.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/maintenance/affinity/AffinityStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/session/SessionStore.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <include/agent/ability/TaskScheduler.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <llm/UsageStore.hpp>
#include <onebot/OneBotClient.hpp>
#include <onebot/OneBotWebSocketClient.hpp>

using namespace insoulforge;
using namespace drogon;

namespace {
    // 进程启动时间（文件作用域 static，程序启动时初始化）
    const auto g_processStartTime = std::chrono::system_clock::now();

    uint64_t parseQueryUInt64(const HttpRequestPtr &req, const std::string &name, uint64_t fallback = 0) {
        const auto value = req->getParameter(name);
        return value.empty() ? fallback : parseUInt64(value, fallback);
    }

    /// @brief 会话列表项的公共头部字段（会话 ID 数值+字符串形式，私聊附带类型与 QQ 号）
    json sessionItemHeader(const uint64_t sessionId) {
        json item;
        // 会话 ID 可能带私聊标志位（超过 JS Number 安全范围），同步提供字符串形式
        item["groupId"] = sessionId;
        item["groupIdStr"] = std::to_string(sessionId);
        if (SessionId::isPrivate(sessionId)) {
            item["sessionType"] = "private";
            item["userId"] = SessionId::privateUserId(sessionId);
        }
        return item;
    }
} // namespace

// ==================== 管理后台认证 ====================

Task<> AdminController::getAuthStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    callback(jsonResponse({{"authenticated", AdminAccessToken::isAuthorized(req)}}));
    co_return;
}

Task<> AdminController::login(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("token") || !(*body)["token"].is_string() ||
        !AdminAccessToken::matches((*body)["token"].get<std::string>())) {
        auto response = jsonResponse(AdminResponse::failJson("访问令牌无效"));
        response->setStatusCode(k401Unauthorized);
        callback(response);
        co_return;
    }

    auto response = jsonResponse(AdminResponse::okJson());
    AdminAccessToken::grantSession(response);
    callback(response);
    co_return;
}

Task<> AdminController::logout(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    auto response = jsonResponse(AdminResponse::okJson());
    AdminAccessToken::revokeSession(response);
    callback(response);
    co_return;
}

// ==================== 运行日志 ====================

Task<> AdminController::getLogs(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    LogQuery query;

    // 线上参数沿用 "groupId"（内部语义为 sessionId）
    if (const std::string groupIdParam = req->getParameter("groupId"); !groupIdParam.empty()) {
        if (groupIdParam == "system") {
            query.sessionId = 0;
        } else {
            query.sessionId = parseUInt64(groupIdParam);
        }
    }

    if (const std::string level = req->getParameter("level"); !level.empty() && level != "all") {
        query.level = level;
    }

    query.keyword = req->getParameter("keyword");
    query.afterId = parseQueryUInt64(req, "afterId");
    query.beforeId = tryParseUInt64(req->getParameter("beforeId"));
    query.limit = static_cast<int>(std::clamp<uint64_t>(parseQueryUInt64(req, "limit", 200), 1, 1000));

    const auto result = LogBuffer::instance().query(query);

    json resp;
    resp["entries"] = json::array();
    for (const auto &entry: result.entries) {
        json item;
        item["id"] = entry.id;
        item["timestamp"] = entry.timestamp;
        item["level"] = entry.level;
        // 字符串形式：会话 ID 可能带私聊标志位，超过 JS 安全整数范围。
        item["sessionId"] = std::to_string(entry.sessionId);
        item["source"] = entry.source;
        item["content"] = entry.content;
        resp["entries"].push_back(item);
    }
    resp["hasMore"] = result.hasMore;
    resp["nextAfterId"] = result.nextAfterId;
    resp["nextBeforeId"] = result.nextBeforeId;
    resp["oldestId"] = result.oldestId;
    resp["newestId"] = result.newestId;
    resp["size"] = LogBuffer::instance().size();
    resp["currentLevel"] = Logger::level();
    callback(jsonResponse(resp));
    co_return;
}

// ==================== HTTP 请求调试 ====================

Task<> AdminController::getHttpTraces(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto afterId = parseQueryUInt64(req, "afterId");
    const auto limit = std::clamp<size_t>(parseQueryUInt64(req, "limit", 50), 1, 500);

    json resp;
    resp["entries"] = json::array();
    resp["total"] = HttpTrace::instance().size();
    for (auto &entry: HttpTrace::instance().query(afterId, limit)) {
        json item;
        item["id"] = entry.id;
        item["timestamp"] = entry.timestamp;
        item["tag"] = entry.tag;
        item["method"] = entry.method;
        item["url"] = entry.url;
        item["status"] = entry.status;
        // 字符串形式：会话 ID 可能带私聊标志位，超过 JS 安全整数范围
        if (entry.sessionId.has_value()) {
            item["groupId"] = std::to_string(*entry.sessionId);
        } else {
            item["groupId"] = nullptr;
        }
        item["requestBody"] = entry.requestBody;
        item["responseBody"] = entry.responseBody;
        resp["entries"].push_back(item);
    }
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::clearHttpTraces(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    HttpTrace::instance().clear();
    callback(jsonResponse(AdminResponse::okJson()));
    co_return;
}

// ==================== 用量统计 ====================

Task<> AdminController::getUsage(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    int days = 30;
    if (const std::string p = req->getParameter("days"); !p.empty()) {
        // 与 stoi 行为一致：跳过前导空白，解析失败时保留默认值
        const auto *begin = p.data();
        const auto *end = p.data() + p.size();
        while (begin < end && (*begin == ' ' || *begin == '\t'))
            ++begin;
        std::from_chars(begin, end, days);
    }
    days = std::clamp(days, 1, 365);

    json resp = UsageStore::getUsageSummary(days);
    resp["recent"] = UsageStore::getRecentUsage(50);
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 运行信息 ====================

Task<> AdminController::getSystemInfo(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto now = std::chrono::system_clock::now();
    const auto uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - g_processStartTime).count();
    const auto startEpoch =
      std::chrono::duration_cast<std::chrono::seconds>(g_processStartTime.time_since_epoch()).count();

    json resp;
    resp["startTime"] = startEpoch;
    resp["uptimeSeconds"] = uptimeSeconds;
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::getBotStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    json resp;
    resp["running"] = AgentSystem::instance().isRunning();
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::setBotStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto body = parseJsonBody(req);
    if (!body || !(*body)["running"].is_boolean()) {
        callback(jsonResponse(AdminResponse::failJson("running字段必须为布尔值")));
        co_return;
    }

    const bool running = (*body)["running"].get<bool>();
    AgentSystem::instance().setRunning(running);
    Logger::warn(0, "Admin", fmt::format("管理后台{}机器人", running ? "打开" : "暂停"));

    json resp = AdminResponse::okJson(running ? "机器人已打开" : "机器人已暂停");
    resp["running"] = running;
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 表情包库（QQ 收藏表情，以实际收藏为基准） ====================

Task<> AdminController::getEmojis(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    callback(jsonResponse(co_await ToolRuntime::fetchFavoriteEmojis()));
    co_return;
}

Task<> AdminController::updateEmojiDesc(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("res_id") || !body->contains("desc")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少必要字段: res_id、desc")));
        co_return;
    }

    const std::string resId = getStr(*body, "res_id");
    const std::string desc = getStr(*body, "desc");
    if (!co_await OneBotClient::setCustomFaceDesc(body->contains("emoji_id") ? getStr(*body, "emoji_id") : "0", resId,
          body->contains("md5") ? getStr(*body, "md5") : "", desc)) {
        callback(jsonResponse(AdminResponse::errorJson("修改表情描述失败，请确认 QQ 客户端在线")));
        co_return;
    }

    ToolRuntime::invalidateFavoriteEmojiCache();
    Logger::info(0, "Admin", fmt::format("已修改表情描述: res_id={} desc={}", resId, desc));

    callback(jsonResponse(AdminResponse::okJson("描述已修改")));
    co_return;
}

Task<> AdminController::getOneBotStatus(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto &config = Config::instance();
    const bool usesWebSocket = config.oneBotTransport == "websocket";

    json resp;
    resp["transport"] = usesWebSocket ? "websocket" : "http";
    resp["address"] = usesWebSocket ? config.qqWebSocketHost : config.qqHttpHost;
    resp["configured"] = !resp["address"].get<std::string>().empty();
    resp["websocketConnected"] = usesWebSocket && OneBotWebSocketClient::instance().isConnected();
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 管理员 ====================

Task<> AdminController::getAdmins(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto admins = AdminStore::getAdmins();

    json result(json::array());
    for (const uint64_t qq: admins) {
        json admin;
        admin["qq"] = qq;
        result.push_back(admin);
    }
    callback(jsonResponse(result));
    co_return;
}

Task<> AdminController::addAdmin(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("qq")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少qq字段")));
        co_return;
    }

    const uint64_t qq = jsonToUInt64((*body)["qq"]);
    AdminStore::addAdmin(qq);

    callback(jsonResponse(AdminResponse::okJson("管理员已添加")));
    co_return;
}

Task<> AdminController::removeAdmin(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &qq) const {
    const uint64_t qqNum = std::stoull(qq);
    AdminStore::removeAdmin(qqNum);

    callback(jsonResponse(AdminResponse::okJson("管理员已删除")));
    co_return;
}

Task<> AdminController::getBlacklist(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    json result = json::array();
    for (const uint64_t qq: BlacklistStore::getAll()) {
        result.push_back({{"qq", qq}});
    }
    callback(jsonResponse(result));
    co_return;
}

Task<> AdminController::addBlacklistEntry(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto body = parseJsonBody(req);
    const uint64_t qq = body && body->contains("qq") ? jsonToUInt64((*body)["qq"]) : 0;
    if (qq == 0) {
        callback(jsonResponse(AdminResponse::errorJson("缺少有效的 qq 字段")));
        co_return;
    }

    BlacklistStore::add(qq);
    callback(jsonResponse(AdminResponse::okJson("黑名单已添加")));
    co_return;
}

Task<> AdminController::removeBlacklistEntry(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &qq) const {
    const auto parsedQq = tryParseUInt64(qq);
    if (!parsedQq || *parsedQq == 0) {
        callback(jsonResponse(AdminResponse::errorJson("无效的 QQ 号")));
        co_return;
    }

    BlacklistStore::remove(*parsedQq);
    callback(jsonResponse(AdminResponse::okJson("黑名单已移除")));
    co_return;
}

// ==================== 启用群 ====================

Task<> AdminController::getGroups(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    auto groups = SessionStore::getAllSessionsWithStatus();

    json result(json::array());
    for (const auto &[sessionId, groupName, enabled, messageCount]: groups) {
        json group = sessionItemHeader(sessionId);
        group["groupName"] = groupName;
        group["enabled"] = enabled;
        group["messageCount"] = messageCount;
        result.push_back(group);
    }
    callback(jsonResponse(result));
    co_return;
}

Task<> AdminController::enableSession(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    auto body = parseJsonBody(req);
    if (!body || (!body->contains("sessionId") && !body->contains("userId"))) {
        callback(jsonResponse(AdminResponse::errorJson("缺少groupId或userId字段")));
        co_return;
    }

    uint64_t sessionId = 0;
    if (getStr(*body, "sessionType") == "private") {
        // 私聊会话 ID 带标志位，由后端按 QQ 号构造（前端无法安全表示超出 JS 精度的大数）
        const uint64_t userId = jsonToUInt64(atOrNull(*body, "userId"));
        if (userId == 0) {
            callback(jsonResponse(AdminResponse::errorJson("QQ号无效")));
            co_return;
        }
        sessionId = SessionId::fromPrivateUser(userId);
    } else {
        // 前端以字符串传递（避免 JS 大数精度丢失），需安全解析而非 asUInt64()
        sessionId = jsonToUInt64(atOrNull(*body, "groupId"));
        if (sessionId == 0 || SessionId::isPrivate(sessionId)) {
            callback(jsonResponse(AdminResponse::errorJson("群号无效")));
            co_return;
        }
    }
    SessionStore::enableSession(sessionId);

    // 自动获取会话名称（群聊为群名，私聊为 QQ 昵称）
    std::string groupName = co_await MessageService::fetchAndUpdateSessionName(sessionId);

    json resp = AdminResponse::okJson(SessionId::isPrivate(sessionId) ? "私聊已启用" : "群已启用");
    resp["groupName"] = groupName;
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::toggleSession(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);
    SessionStore::toggleSessionStatus(gid);

    callback(jsonResponse(AdminResponse::okJson("群状态已切换")));
    co_return;
}

Task<> AdminController::removeSession(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);
    SessionStore::disableSession(gid);

    callback(jsonResponse(AdminResponse::okJson("群已删除")));
    co_return;
}

Task<> AdminController::refreshSessionName(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);
    const auto groupName = co_await MessageService::fetchAndUpdateSessionName(gid);

    json resp = AdminResponse::okJson();
    resp["groupName"] = groupName;
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::refreshAllSessionNames(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    auto groups = SessionStore::getAllSessionsWithStatus();

    for (const auto &[sessionId, groupName, enabled, messageCount]: groups) {
        co_await MessageService::fetchAndUpdateSessionName(sessionId);
    }

    callback(jsonResponse(AdminResponse::okJson("所有会话名称已刷新")));
    co_return;
}

// ==================== 聊天记录 ====================

Task<> AdminController::getChatSessions(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    auto groups = SessionStore::getSessionsWithChatRecords();

    json result(json::array());
    for (const auto &[sessionId, groupName, messageCount]: groups) {
        json group = sessionItemHeader(sessionId);
        group["groupName"] = groupName;
        group["messageCount"] = messageCount;
        result.push_back(group);
    }
    callback(jsonResponse(result));
    co_return;
}

Task<> AdminController::getChatRecords(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);

    // 支持limit参数
    int limit = 50;
    if (const std::string limitParam = req->getParameter("limit"); !limitParam.empty()) {
        limit = std::stoi(limitParam);
    }

    // 运行中的消息仅在正常退出时才落库；管理后台必须优先读取其完整内存快照。
    if (const auto messages = OneBotEventWorkflow::instance().getSessionMessages(gid)) {
        json result = json::array();
        const size_t first = messages->size() > static_cast<size_t>(limit) ? messages->size() - limit : 0;
        for (size_t index = first; index < messages->size(); ++index) {
            const json &message = (*messages)[index];
            result.push_back(
              {{"role", MessageRecord::isAssistant(message) ? "assistant" : "user"}, {"content", dumpJson(message)}});
        }
        callback(jsonResponse(result));
        co_return;
    }

    // 工作流尚未初始化时回退到持久化恢复副本；该副本才有可编辑的数据库行 ID。
    const auto result = ChatRecordStore::getChatRecordsWithIds(gid, limit);

    // 反转顺序，最新的在底部
    json reversed(json::array());
    for (size_t i = result.size(); i > 0; --i) {
        reversed.push_back(result[i - 1]);
    }

    callback(jsonResponse(reversed));
    co_return;
}

Task<> AdminController::updateChatRecord(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &recordId) const {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("content")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少content字段")));
        co_return;
    }

    const int id = std::stoi(recordId);
    const std::string content = getStr(*body, "content");
    ChatRecordStore::updateChatRecord(id, content);

    callback(jsonResponse(AdminResponse::okJson("聊天记录已更新")));
    co_return;
}

Task<> AdminController::deleteChatRecord(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &recordId) const {
    const int id = std::stoi(recordId);
    ChatRecordStore::deleteChatRecord(id);

    callback(jsonResponse(AdminResponse::okJson("聊天记录已删除")));
    co_return;
}

Task<> AdminController::clearSessionChatRecords(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);
    ChatRecordStore::clearSessionChatRecords(gid);

    callback(jsonResponse(AdminResponse::okJson("聊天记录已清空")));
    co_return;
}

// ==================== 长期记忆 ====================

Task<> AdminController::getLongTermMemories(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    uint64_t sessionId = 0;
    if (const std::string sessionParam = req->getParameter("sessionId"); !sessionParam.empty()) {
        sessionId = std::stoull(sessionParam);
    }

    int limit = 20;
    if (const std::string limitParam = req->getParameter("limit"); !limitParam.empty()) {
        limit = std::clamp(std::stoi(limitParam), 1, 100);
    }

    int offset = 0;
    if (const std::string offsetParam = req->getParameter("offset"); !offsetParam.empty()) {
        offset = std::max(0, std::stoi(offsetParam));
    }

    json items(json::array());
    for (const auto &[id, groupId, content, createdAt]: LongTermMemoryStore::listMemories(sessionId, limit, offset)) {
        json item;
        item["id"] = id;
        // 会话 ID（私聊带标志位）可能超出 JS 安全整数范围，统一以字符串输出
        item["groupId"] = std::to_string(groupId);
        item["content"] = content;
        item["createdAt"] = createdAt;
        items.push_back(item);
    }

    json resp;
    resp["items"] = items;
    resp["total"] = LongTermMemoryStore::countMemories(sessionId);
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::deleteLongTermMemory(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const {
    if (!LongTermMemoryStore::deleteMemory(std::stoll(id))) {
        callback(jsonResponse(AdminResponse::errorJson("记忆不存在或已被删除")));
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("长期记忆已删除")));
    co_return;
}


// ==================== 群记忆 ====================

Task<> AdminController::getSessionMemory(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);
    const std::string memory = MemoryStore::getShortTermMemory(gid);

    json resp;
    resp["groupId"] = gid;
    resp["memory"] = memory;
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::updateSessionMemory(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("memory")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少memory字段")));
        co_return;
    }

    const uint64_t gid = std::stoull(sessionId);
    const std::string memory = getStr(*body, "memory");
    MemoryStore::updateShortTermMemory(gid, memory);

    callback(jsonResponse(AdminResponse::okJson("记忆已更新")));
    co_return;
}

// ==================== 好感度 ====================

Task<> AdminController::getSessionAffinity(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t gid = std::stoull(sessionId);
    auto affinityMap = AffinityStore::getAffinityMap(gid);

    std::vector<std::pair<uint64_t, int>> entries(affinityMap.begin(), affinityMap.end());
    std::ranges::sort(entries, [](const auto &a, const auto &b) { return a.second > b.second; });

    json list(json::array());
    for (const auto &[qq, affinity]: entries) {
        json item;
        // QQ 号以字符串返回（超过 JS Number 安全范围）
        item["qq"] = std::to_string(qq);
        // 昵称优先取运行时映射（含自定义昵称），缺失时回退 OneBot 实时查询
        std::string name(QQNameDirectory::getName(qq));
        if (name.empty() || name == "未知") {
            const auto info = co_await OneBotClient::getStrangerInfo(qq, gid);
            if (atOrNull(info, "data").contains("nickname")) {
                name = getStr(atOrNull(info, "data"), "nickname");
            }
        }
        if (!name.empty() && name != "未知") {
            item["name"] = name;
        }
        item["affinity"] = affinity;
        list.push_back(item);
    }

    json resp;
    resp["groupIdStr"] = std::to_string(gid);
    resp["affinities"] = list;
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 定时任务 ====================

Task<> AdminController::getScheduledTasks(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &sessionId) const {
    const uint64_t sid = parseUInt64(sessionId);
    if (sid == 0) {
        callback(jsonResponse(AdminResponse::errorJson("无效的会话 ID")));
        co_return;
    }

    const auto [sessionType, targetId] = SessionId::toStorageTarget(sid);
    json list(json::array());
    for (const auto &task: TaskStore::getPendingScheduledTasksByTarget(sessionType, targetId)) {
        json item;
        item["id"] = task.id;
        item["remindTime"] = task.remindTime;
        item["content"] = task.content;
        item["daily"] = task.isDaily;
        list.push_back(item);
    }

    json resp;
    resp["tasks"] = list;
    callback(jsonResponse(resp));
    co_return;
}

Task<> AdminController::cancelScheduledTask(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const {
    const uint64_t taskId = parseUInt64(id);
    if (taskId == 0) {
        callback(jsonResponse(AdminResponse::failJson("无效的任务 ID")));
        co_return;
    }

    if (TaskScheduler::instance().cancel(static_cast<int64_t>(taskId))) {
        json resp = AdminResponse::okJson(fmt::format("定时任务 #{} 已取消", taskId));
        Logger::info(0, "Admin", fmt::format("已取消定时任务 #{}", taskId));
        callback(jsonResponse(resp));
    } else {
        callback(jsonResponse(AdminResponse::failJson("任务不存在或已触发/已取消")));
    }
    co_return;
}

// ==================== 记忆配置 ====================

Task<> AdminController::getMemoryConfig(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto config = ConfigStore::getMemoryConfig();
    callback(jsonResponse(config));
    co_return;
}

Task<> AdminController::saveMemoryConfig(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    // 需就地补默认值，body 须可变
    auto body = parseJsonBody(req);
    if (!body) {
        callback(jsonResponse(AdminResponse::errorJson("缺少配置数据")));
        co_return;
    }

    if (getInt(*body, "contextWindowLimit") <= 0) {
        (*body)["contextWindowLimit"] = Config::instance().contextWindowLimit;
    }
    if (getInt(*body, "memorySummaryTriggerCount") <= 0) {
        (*body)["memorySummaryTriggerCount"] = Config::instance().memorySummaryTriggerCount;
    }
    if (getInt(*body, "memorySummaryBatchSize") <= 0 ||
        getInt(*body, "memorySummaryBatchSize") > getInt(*body, "memorySummaryTriggerCount")) {
        (*body)["memorySummaryBatchSize"] = getInt(*body, "memorySummaryTriggerCount") / 2;
    }
    if (getInt(*body, "memorySummaryContextCount") < 0) {
        (*body)["memorySummaryContextCount"] = Config::instance().memorySummaryContextCount;
    }
    if (getInt(*body, "memoryExtractMaxTokens") <= 0) {
        (*body)["memoryExtractMaxTokens"] = Config::instance().memoryExtractMaxTokens;
    }
    // Router 子窗口校验: 保留条数必须小于触发条数
    if (getInt(*body, "routerWindowTriggerCount") <= 0) {
        (*body)["routerWindowTriggerCount"] = Config::instance().routerWindowTriggerCount;
    }
    if (getInt(*body, "routerWindowKeepCount") <= 0 ||
        getInt(*body, "routerWindowKeepCount") >= getInt(*body, "routerWindowTriggerCount")) {
        (*body)["routerWindowKeepCount"] = getInt(*body, "routerWindowTriggerCount") / 2;
    }
    // 召回阈值: 必须在 (0,1) 开区间内
    if (getDouble(*body, "longTermRecallThreshold") <= 0.0 || getDouble(*body, "longTermRecallThreshold") >= 1.0) {
        (*body)["longTermRecallThreshold"] = Config::instance().longTermRecallThreshold;
    }
    // 注入阈值: 必须在 (0,1) 开区间内
    if (getDouble(*body, "longTermInjectThreshold") <= 0.0 || getDouble(*body, "longTermInjectThreshold") >= 1.0) {
        (*body)["longTermInjectThreshold"] = Config::instance().longTermInjectThreshold;
    }

    ConfigStore::saveMemoryConfig(*body);

    // 更新内存中的配置
    auto &config = Config::instance();
    config.contextWindowLimit = getInt(*body, "contextWindowLimit");
    config.memorySummaryTriggerCount = getInt(*body, "memorySummaryTriggerCount");
    config.memorySummaryBatchSize = getInt(*body, "memorySummaryBatchSize");
    config.memorySummaryContextCount = getInt(*body, "memorySummaryContextCount");
    config.memoryExtractMaxTokens = getInt(*body, "memoryExtractMaxTokens");
    config.routerWindowTriggerCount = getInt(*body, "routerWindowTriggerCount");
    config.routerWindowKeepCount = getInt(*body, "routerWindowKeepCount");
    config.shortTermMemoryMax = getInt(*body, "shortTermMemoryMax");
    config.longTermRecallThreshold = getDouble(*body, "longTermRecallThreshold");
    config.longTermInjectThreshold = getDouble(*body, "longTermInjectThreshold");

    callback(jsonResponse(AdminResponse::okJson("记忆配置已保存")));
    co_return;
}

// ==================== QQ Bot 配置 ====================

Task<> AdminController::getQQConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto config = ConfigStore::getQQConfig();
    callback(jsonResponse(config));
    co_return;
}

Task<> AdminController::saveQQConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const {
    const auto body = parseJsonBody(req);
    if (!body) {
        callback(jsonResponse(AdminResponse::errorJson("缺少配置数据")));
        co_return;
    }

    // 更新内存中的配置
    auto &config = Config::instance();
    config.accessToken = getStr(*body, "accessToken");
    config.selfQQNumber = getInt64(*body, "selfQQNumber");
    config.oneBotTransport = getStr(*body, "oneBotTransport", "http");
    config.qqHttpHost = getStr(*body, "qqHttpHost");
    config.qqWebSocketHost = getStr(*body, "qqWebSocketHost");
    config.botName = getStr(*body, "botName", "机器人");
    if (config.oneBotTransport != "http" && config.oneBotTransport != "websocket") {
        config.oneBotTransport = "http";
    }
    json normalizedConfig = *body;
    normalizedConfig["oneBotTransport"] = config.oneBotTransport;
    normalizedConfig["qqHttpHost"] = config.qqHttpHost;
    normalizedConfig["qqWebSocketHost"] = config.qqWebSocketHost;

    ConfigStore::saveQQConfig(normalizedConfig);

    // 更新机器人自己的自定义昵称
    QQNameDirectory::setCustomName(config.selfQQNumber, config.botName + "(我)");
    OneBotWebSocketClient::instance().reconfigure();

    callback(jsonResponse(AdminResponse::okJson("QQ Bot 配置已保存")));
    co_return;
}
