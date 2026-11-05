#include <charconv>
#include <drogon/MultiPart.h>
#include <filesystem>

#include <admin/access/AdminAccessToken.hpp>
#include <admin/http/AdminController.hpp>
#include <admin/http/AdminResponse.hpp>
#include <admin/http/PersonNameResolver.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/monitoring/ProcessMetrics.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <onebot/transport/OneBotWebSocketClient.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/access/AdminStore.hpp>
#include <workflow/access/BlacklistStore.hpp>
#include <workflow/agent/llm/usage/UsageStore.hpp>
#include <workflow/agent/memory/LongTermMemoryStore.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <workflow/agent/memory/maintenance/PersonMemoryConsolidator.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/scheduling/TaskScheduler.hpp>
#include <workflow/session/QQNameDirectory.hpp>
#include <workflow/session/SessionConfigManager.hpp>
#include <workflow/session/SessionStore.hpp>

using namespace insoulforge;
using namespace drogon;

namespace {
    // 进程启动时间（文件作用域 static，程序启动时初始化）
    const auto g_processStartTime = std::chrono::system_clock::now();

    void addPersonName(json &person) {
        const auto name = PersonNameResolver::instance().lookup(UserId{getStr(person, "user_id")});
        person["name"] = name.value;
        person["name_pending"] = name.pending;
    }

    auto processSampleJson(const ProcessSample &sample) -> json {
        auto entry = json{{"timestamp", sample.timestamp}};
        entry["rssBytes"] = sample.rssBytes ? json(*sample.rssBytes) : json(nullptr);
        entry["peakRssBytes"] = sample.peakRssBytes ? json(*sample.peakRssBytes) : json(nullptr);
        entry["threads"] = sample.threads ? json(*sample.threads) : json(nullptr);
        return entry;
    }

    auto parseQueryUInt64(const HttpRequestPtr &req, const std::string &name, u64 fallback = 0) -> u64 {
        const auto value = req->getParameter(name);
        return value.empty() ? fallback : parseUInt64(value, fallback);
    }

    /// @brief 返回统一会话身份；真实目标保持字符串，不与会话编码混用。
    auto sessionItemHeader(const SessionId &sessionId) -> json {
        return {{"sessionId", sessionId.toString()}, {"sessionType", sessionId.typeName()},
          {"targetId", sessionId.targetId()}};
    }
} // namespace

// ==================== 管理后台认证 ====================

auto AdminController::getAuthStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    callback(jsonResponse({{"authenticated", AdminAccessToken::isAuthorized(req)}}));
    co_return;
}

auto AdminController::login(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    if (const auto body = parseJsonBody(req); !body || !body->contains("token") || !(*body)["token"].is_string() ||
                                              !AdminAccessToken::matches((*body)["token"].get<std::string>())) {
        const auto response = jsonResponse(AdminResponse::failJson("访问令牌无效"));
        response->setStatusCode(k401Unauthorized);
        callback(response);
        co_return;
    }

    auto response = jsonResponse(AdminResponse::okJson());
    AdminAccessToken::grantSession(response);
    callback(response);
    co_return;
}

auto AdminController::logout(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto response = jsonResponse(AdminResponse::okJson());
    AdminAccessToken::revokeSession(response);
    callback(response);
    co_return;
}

// ==================== 运行日志 ====================

auto AdminController::getLogs(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    LogQuery query;

    // sessionId 传规范字符串；system 仅作为全局日志的筛选值
    if (const std::string groupIdParam = req->getParameter("sessionId"); !groupIdParam.empty()) {
        if (groupIdParam == "system") {
            query.systemOnly = true;
        } else {
            const auto parsed = SessionId::parse(groupIdParam);
            if (!parsed) {
                callback(jsonResponse(AdminResponse::errorJson(parsed.error())));
                co_return;
            }
            query.sessionId = *parsed;
        }
    }

    if (const std::string level = req->getParameter("level"); !level.empty() && level != "all") {
        query.level = level;
    }

    query.keyword = req->getParameter("keyword");
    query.afterId = parseQueryUInt64(req, "afterId");
    query.beforeId = tryParseUInt64(req->getParameter("beforeId"));
    query.limit = static_cast<i32>(std::clamp<u64>(parseQueryUInt64(req, "limit", 200), 1, 1000));

    const auto [entries, hasMore, nextAfterId, nextBeforeId, oldestId, newestId] = LogBuffer::instance().query(query);

    json resp;
    resp["entries"] = json::array();
    for (const auto &[id, timestamp, level, sessionId, source, content]: entries) {
        json item;
        item["id"] = id;
        item["timestamp"] = timestamp;
        item["level"] = level;
        // 会话 ID 使用带类型的规范字符串。
        item["sessionId"] = sessionId ? json(*sessionId) : json(nullptr);
        item["source"] = source;
        item["content"] = content;
        resp["entries"].push_back(item);
    }
    resp["hasMore"] = hasMore;
    resp["nextAfterId"] = nextAfterId;
    resp["nextBeforeId"] = nextBeforeId;
    resp["oldestId"] = oldestId;
    resp["newestId"] = newestId;
    resp["size"] = LogBuffer::instance().size();
    resp["currentLevel"] = Logger::level();
    callback(jsonResponse(resp));
    co_return;
}

// ==================== HTTP 请求调试 ====================

auto AdminController::getHttpTraces(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto afterId = parseQueryUInt64(req, "afterId");
    const auto limit = std::clamp<size_t>(parseQueryUInt64(req, "limit", 50), 1, 500);

    json resp;
    resp["entries"] = json::array();
    resp["total"] = HttpTrace::instance().size();
    for (auto &[id, timestamp, tag, method, url, requestBody, status, responseBody, sessionId]:
      HttpTrace::instance().query(afterId, limit)) {
        json item;
        item["id"] = id;
        item["timestamp"] = timestamp;
        item["tag"] = tag;
        item["method"] = method;
        item["url"] = url;
        item["status"] = status;
        // 会话 ID 使用带类型的规范字符串
        if (sessionId.has_value()) {
            item["sessionId"] = sessionId->toString();
        } else {
            item["sessionId"] = nullptr;
        }
        item["requestBody"] = requestBody;
        item["responseBody"] = responseBody;
        resp["entries"].push_back(item);
    }
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::downloadHttpTrace(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto id = tryParseUInt64(req->getParameter("id"));
    if (!id) {
        auto response = jsonResponse(AdminResponse::failJson("无效的请求记录 ID"));
        response->setStatusCode(k400BadRequest);
        callback(response);
        co_return;
    }

    const auto entry = HttpTrace::instance().find(*id);
    if (!entry) {
        auto response = jsonResponse(AdminResponse::failJson("请求记录不存在或已被清除"));
        response->setStatusCode(k404NotFound);
        callback(response);
        co_return;
    }

    const json document = {{"id", entry->id}, {"timestamp", entry->timestamp}, {"tag", entry->tag},
      {"method", entry->method}, {"url", entry->url}, {"status", entry->status}, {"requestBody", entry->requestBody},
      {"responseBody", entry->responseBody},
      {"sessionId", entry->sessionId ? json(entry->sessionId->toString()) : json(nullptr)}};
    auto response = jsonResponse(document);
    response->addHeader("Content-Disposition", "attachment; filename=\"http-trace-" + std::to_string(*id) + ".json\"");
    callback(response);
    co_return;
}

auto AdminController::clearHttpTraces(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    HttpTrace::instance().clear();
    callback(jsonResponse(AdminResponse::okJson()));
    co_return;
}

// ==================== 用量统计 ====================

auto AdminController::getUsage(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    i32 days = 30;
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

auto AdminController::getSystemInfo(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto now = std::chrono::system_clock::now();
    const auto uptimeSeconds = std::chrono::duration_cast<std::chrono::seconds>(now - g_processStartTime).count();
    const auto startEpoch =
      std::chrono::duration_cast<std::chrono::seconds>(g_processStartTime.time_since_epoch()).count();

    json resp;
    resp["startTime"] = startEpoch;
    resp["uptimeSeconds"] = uptimeSeconds;
    // 缺失文件计为零；权限或读取错误返回 null，避免把统计失败显示成零占用。
    const auto fileSize = [](const std::filesystem::path &path) -> std::optional<u64> {
        auto error = std::error_code{};
        const auto size = std::filesystem::file_size(path, error);
        if (error == std::errc::no_such_file_or_directory)
            return 0;
        if (error)
            return std::nullopt;
        return size;
    };
    auto databaseBytes = std::optional<u64>{0};
    for (const auto suffix: {"", "-wal", "-shm"}) {
        const auto size = fileSize(std::string("data/insoulforge.db") + suffix);
        if (!size) {
            databaseBytes.reset();
            break;
        }
        *databaseBytes += *size;
    }
    auto logBytes = u64{0};
    auto error = std::error_code{};
    auto files = std::filesystem::recursive_directory_iterator("logs", error);
    if (error == std::errc::no_such_file_or_directory)
        error.clear();
    const auto end = std::filesystem::recursive_directory_iterator{};
    while (!error && files != end) {
        // 不跟随符号链接，避免统计日志目录之外的文件。
        if (files->is_regular_file(error) && !files->is_symlink(error)) {
            const auto size = fileSize(files->path());
            if (!size) {
                error = std::make_error_code(std::errc::io_error);
                break;
            }
            logBytes += *size;
        }
        if (!error)
            files.increment(error);
    }
    resp["storage"] = {{"databaseBytes", databaseBytes ? json(*databaseBytes) : json(nullptr)},
      {"logBytes", error ? json(nullptr) : json(logBytes)},
      {"timestamp", std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()}};
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::getProcessMetrics(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto history = json::array();
    for (const auto &sample: ProcessMetrics::instance().snapshot()) {
        history.push_back(processSampleJson(sample));
    }
    auto response = json{{"sampleIntervalSeconds", ProcessMetrics::kSampleIntervalSeconds}};
    response["latest"] = history.empty() ? json(nullptr) : history.back();
    response["history"] = std::move(history);
    callback(jsonResponse(response));
    co_return;
}

auto AdminController::getCurrentProcessMetrics(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    callback(jsonResponse(processSampleJson(readProcessSample())));
    co_return;
}

auto AdminController::getBotStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    json resp;
    resp["running"] = AgentSystem::instance().isRunning();
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::setBotStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !(*body)["running"].is_boolean()) {
        callback(jsonResponse(AdminResponse::failJson("running字段必须为布尔值")));
        co_return;
    }

    const bool running = (*body)["running"].get<bool>();
    AgentSystem::instance().setRunning(running);
    Logger::warn(std::nullopt, "Admin", fmt::format("管理后台{}机器人", running ? "打开" : "暂停"));

    json resp = AdminResponse::okJson(running ? "机器人已打开" : "机器人已暂停");
    resp["running"] = running;
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 表情包库（QQ 收藏表情，以实际收藏为基准） ====================

auto AdminController::getEmojis(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    if (req->getParameter("refresh") == "1") ToolRuntime::invalidateFavoriteEmojiCache();
    callback(jsonResponse(co_await ToolRuntime::fetchFavoriteEmojis()));
    co_return;
}

auto AdminController::addEmoji(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    if (req->getHeader("content-type").starts_with("multipart/form-data")) {
        if (req->body().size() > ToolRuntime::kMaxFavoriteEmojiBytes + 1024U * 1024U) {
            auto response = jsonResponse(AdminResponse::failJson("图片不得超过 8 MiB"));
            response->setStatusCode(k413RequestEntityTooLarge);
            callback(response);
            co_return;
        }
        MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().size() != 1 ||
            parser.getFiles()[0].getItemName() != "image") {
            auto response = jsonResponse(AdminResponse::failJson("请以 image 字段上传一张图片"));
            response->setStatusCode(k400BadRequest);
            callback(response);
            co_return;
        }
        const auto bytes = parser.getFiles()[0].fileContent();
        if (bytes.size() > ToolRuntime::kMaxFavoriteEmojiBytes) {
            auto response = jsonResponse(AdminResponse::failJson("图片不得超过 8 MiB"));
            response->setStatusCode(k413RequestEntityTooLarge);
            callback(response);
            co_return;
        }
        const auto result = co_await ToolRuntime::uploadFavoriteEmoji(std::string(bytes));
        callback(jsonResponse(result ? AdminResponse::okJson("已提交 QQ 收藏，请核对列表，可随后编辑描述")
                                     : AdminResponse::failJson(result.error())));
        co_return;
    }
    const auto body = parseJsonBody(req);
    if (!body || !body->is_object() || !body->contains("url") || !(*body)["url"].is_string()) {
        callback(jsonResponse(AdminResponse::failJson("url字段必须为图片直链字符串")));
        co_return;
    }
    const auto result = co_await ToolRuntime::addFavoriteEmoji(getStr(*body, "url"));
    callback(jsonResponse(result ? AdminResponse::okJson("已提交 QQ 收藏，请核对列表，可随后编辑描述")
                                 : AdminResponse::failJson(result.error())));
}

auto AdminController::deleteEmoji(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->is_object() || !body->contains("res_id") || !(*body)["res_id"].is_string()) {
        callback(jsonResponse(AdminResponse::failJson("res_id字段必须为字符串")));
        co_return;
    }
    const auto result = co_await ToolRuntime::deleteFavoriteEmoji(getStr(*body, "res_id"));
    callback(jsonResponse(result ? AdminResponse::okJson("已提交 QQ 删除，请核对列表")
                                 : AdminResponse::failJson(result.error())));
}

auto AdminController::updateEmojiDesc(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("res_id") || !body->contains("desc")) {
        callback(jsonResponse(AdminResponse::failJson("缺少必要字段: res_id、desc")));
        co_return;
    }

    const std::string resId = getStr(*body, "res_id");
    const std::string desc = getStr(*body, "desc");
    if (!co_await OneBotClient::setCustomFaceDesc(body->contains("emoji_id") ? getStr(*body, "emoji_id") : "0", resId,
          body->contains("md5") ? getStr(*body, "md5") : "", desc)) {
        callback(jsonResponse(AdminResponse::failJson("修改表情描述失败，请确认 QQ 客户端在线")));
        co_return;
    }

    ToolRuntime::invalidateFavoriteEmojiCache();
    Logger::info(std::nullopt, "Admin", fmt::format("已修改表情描述: res_id={} desc={}", resId, desc));

    callback(jsonResponse(AdminResponse::okJson("描述已修改")));
    co_return;
}

auto AdminController::getOneBotStatus(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
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

auto AdminController::getAdmins(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto admins = AdminStore::getAdmins();

    json result(json::array());
    for (const auto &userId: admins) {
        json admin;
        admin["user_id"] = userId.toString();
        result.push_back(admin);
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::addAdmin(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("user_id")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少 user_id 字段")));
        co_return;
    }

    const auto userId = UserId::parse(getStr(*body, "user_id"));
    if (!userId) {
        callback(jsonResponse(AdminResponse::errorJson("缺少有效的 user_id 字段")));
        co_return;
    }
    AdminStore::addAdmin(*userId);

    callback(jsonResponse(AdminResponse::okJson("管理员已添加")));
    co_return;
}

auto AdminController::removeAdmin(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &userId) const -> Task<> {
    const auto id = UserId::parse(userId);
    if (!id) {
        callback(jsonResponse(AdminResponse::errorJson(id.error())));
        co_return;
    }
    AdminStore::removeAdmin(*id);

    callback(jsonResponse(AdminResponse::okJson("管理员已删除")));
    co_return;
}

auto AdminController::getBlacklist(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    json result = json::array();
    for (const auto &userId: BlacklistStore::getAll()) {
        result.push_back({{"user_id", userId.toString()}});
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::addBlacklistEntry(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    const auto userId = UserId::parse(body ? getStr(*body, "user_id") : "");
    if (!userId) {
        callback(jsonResponse(AdminResponse::errorJson("缺少有效的 user_id 字段")));
        co_return;
    }

    BlacklistStore::add(*userId);
    callback(jsonResponse(AdminResponse::okJson("黑名单已添加")));
    co_return;
}

auto AdminController::removeBlacklistEntry(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &userId) const -> Task<> {
    const auto parsedQq = UserId::parse(userId);
    if (!parsedQq) {
        callback(jsonResponse(AdminResponse::errorJson("无效的用户 ID")));
        co_return;
    }

    BlacklistStore::remove(*parsedQq);
    callback(jsonResponse(AdminResponse::okJson("黑名单已移除")));
    co_return;
}

// ==================== 启用群 ====================

auto AdminController::getGroups(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto groups = SessionStore::getAllSessionsWithStatus();

    json result(json::array());
    for (const auto &[sessionId, groupName, enabled, messageCount]: groups) {
        const auto config = SessionStore::getSessionConfig(sessionId);
        json group = sessionItemHeader(sessionId);
        group["sessionName"] = groupName;
        group["enabled"] = enabled;
        group["sendGroupCard"] = !sessionId.isPrivate() && config.sendGroupCard;
        group["messageCount"] = messageCount;
        group["autoReply"] = config.autoReply;
        group["commandEnabled"] = config.commandEnabled;
        group["crossSessionSharing"] = config.crossSessionSharing;
        group["hasAdditionalPrompt"] = !sessionId.isPrivate() && !config.additionalPrompt.empty();
        result.push_back(group);
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::enableSession(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto body = parseJsonBody(req);
    if (!body || (!body->contains("groupId") && !body->contains("userId"))) {
        callback(jsonResponse(AdminResponse::errorJson("缺少groupId或userId字段")));
        co_return;
    }

    const auto kind = getStr(*body, "sessionType", "group");
    const auto raw = getStr(*body, kind == "private" ? "userId" : "groupId");
    const auto user = UserId::parse(raw);
    if ((kind != "private" && kind != "group") || !user) {
        callback(jsonResponse(AdminResponse::errorJson("需要有效的会话类型和字符串目标 ID")));
        co_return;
    }
    const auto sessionId = kind == "private" ? SessionId::privateChat(*user) : SessionId::group(raw);
    SessionStore::enableSession(sessionId);

    // 自动获取会话名称（群聊为群名，私聊为 QQ 昵称）
    std::string groupName = co_await SessionConfigManager::fetchAndUpdateSessionName(sessionId);

    json resp = AdminResponse::okJson(sessionId.isPrivate() ? "私聊已启用" : "群已启用");
    resp["sessionName"] = groupName;
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::getGroupPrompt(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto id = SessionId::parse(sessionId);
    if (!id || id->isPrivate()) {
        auto response = jsonResponse(AdminResponse::errorJson("群号无效"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    callback(jsonResponse({{"additionalPrompt", SessionStore::getSessionConfig(*id).additionalPrompt}}));
    co_return;
}

auto AdminController::saveGroupPrompt(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto id = SessionId::parse(sessionId);
    const auto body = parseJsonBody(req);
    if (!id || id->isPrivate() || !body || !atOrNull(*body, "additionalPrompt").is_string()) {
        auto response = jsonResponse(AdminResponse::errorJson("需要有效群号及字符串类型 additionalPrompt"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto prompt = getStr(*body, "additionalPrompt");
    if (prompt.size() > SessionStore::kMaxAdditionalPromptBytes) {
        auto response = jsonResponse(AdminResponse::errorJson("附加提示词不能超过 16000 字节"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    SessionStore::setAdditionalPrompt(*id, prompt);
    callback(jsonResponse(AdminResponse::okJson("群附加提示词已保存")));
    co_return;
}

auto AdminController::saveGroupAutoReply(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto id = SessionId::parse(sessionId);
    const auto body = parseJsonBody(req);
    if (!id || id->isPrivate() || !body || !atOrNull(*body, "autoReply").is_boolean()) {
        auto response = jsonResponse(AdminResponse::errorJson("需要有效群号及布尔类型 autoReply"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    SessionStore::setAutoReply(*id, (*body)["autoReply"].get<bool>());
    callback(jsonResponse(AdminResponse::okJson("自动回复设置已保存")));
    co_return;
}

auto AdminController::saveGroupCommandEnabled(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto id = SessionId::parse(sessionId);
    const auto body = parseJsonBody(req);
    if (!id || id->isPrivate() || !body || !atOrNull(*body, "commandEnabled").is_boolean()) {
        auto response = jsonResponse(AdminResponse::errorJson("需要有效群号及布尔类型 commandEnabled"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    SessionStore::setCommandEnabled(*id, (*body)["commandEnabled"].get<bool>());
    callback(jsonResponse(AdminResponse::okJson("命令解析设置已保存")));
    co_return;
}

auto AdminController::saveSessionSharing(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto id = SessionId::parse(sessionId);
    const auto body = parseJsonBody(req);
    if (!id || !body || !atOrNull(*body, "crossSessionSharing").is_boolean()) {
        auto response = jsonResponse(AdminResponse::errorJson("需要有效会话 ID 及布尔类型 crossSessionSharing"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    SessionStore::setCrossSessionSharing(*id, (*body)["crossSessionSharing"].get<bool>());
    callback(jsonResponse(AdminResponse::okJson("跨会话共享设置已保存")));
    co_return;
}

auto AdminController::saveGroupCard(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto id = SessionId::parse(sessionId);
    const auto body = parseJsonBody(req);
    if (!id || id->isPrivate() || !body || !atOrNull(*body, "sendGroupCard").is_boolean()) {
        auto response = jsonResponse(AdminResponse::errorJson("需要有效群号及布尔类型 sendGroupCard"));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    SessionStore::setSendGroupCard(*id, (*body)["sendGroupCard"].get<bool>());
    callback(jsonResponse(AdminResponse::okJson("群名片设置已保存")));
    co_return;
}

auto AdminController::toggleSession(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    SessionStore::toggleSessionStatus(gid);

    callback(jsonResponse(AdminResponse::okJson("群状态已切换")));
    co_return;
}

auto AdminController::removeSession(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    SessionStore::disableSession(gid);

    callback(jsonResponse(AdminResponse::okJson("群已删除")));
    co_return;
}

auto AdminController::refreshSessionName(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    const auto groupName = co_await SessionConfigManager::fetchAndUpdateSessionName(gid);

    json resp = AdminResponse::okJson();
    resp["sessionName"] = groupName;
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::refreshAllSessionNames(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {

    for (auto groups = SessionStore::getAllSessionsWithStatus();
      const auto &[sessionId, groupName, enabled, messageCount]: groups) {
        co_await SessionConfigManager::fetchAndUpdateSessionName(sessionId);
    }

    callback(jsonResponse(AdminResponse::okJson("所有会话名称已刷新")));
    co_return;
}

// ==================== 聊天记录 ====================

auto AdminController::getChatSessions(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto groups = SessionStore::getSessionsWithChatRecords();

    json result(json::array());
    for (const auto &[sessionId, groupName, messageCount]: groups) {
        json group = sessionItemHeader(sessionId);
        group["sessionName"] = groupName;
        group["messageCount"] = messageCount;
        result.push_back(group);
    }
    callback(jsonResponse(result));
    co_return;
}

auto AdminController::getChatRecords(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;

    // 支持limit参数
    i32 limit = 50;
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

auto AdminController::updateChatRecord(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &recordId) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("content")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少content字段")));
        co_return;
    }

    const i32 id = std::stoi(recordId);
    const std::string content = getStr(*body, "content");
    ChatRecordStore::updateChatRecord(id, content);

    callback(jsonResponse(AdminResponse::okJson("聊天记录已更新")));
    co_return;
}

auto AdminController::deleteChatRecord(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &recordId) const -> Task<> {
    const i32 id = std::stoi(recordId);
    ChatRecordStore::deleteChatRecord(id);

    callback(jsonResponse(AdminResponse::okJson("聊天记录已删除")));
    co_return;
}

auto AdminController::clearSessionChatRecords(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    OneBotEventWorkflow::instance().clearSessionMessages(gid);

    callback(jsonResponse(AdminResponse::okJson("聊天记录已清空")));
    co_return;
}

// ==================== 长期记忆 ====================

auto AdminController::getLongTermMemories(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const -> Task<> {
    std::optional<SessionId> sessionId = std::nullopt;
    if (const std::string sessionParam = req->getParameter("sessionId"); !sessionParam.empty()) {
        const auto parsed = SessionId::parse(sessionParam);
        if (!parsed) {
            callback(jsonResponse(AdminResponse::errorJson(parsed.error())));
            co_return;
        }
        sessionId = *parsed;
    }

    i32 limit = 20;
    if (const std::string limitParam = req->getParameter("limit"); !limitParam.empty()) {
        limit = std::clamp(std::stoi(limitParam), 1, 100);
    }

    i32 offset = 0;
    if (const std::string offsetParam = req->getParameter("offset"); !offsetParam.empty()) {
        offset = std::max(0, std::stoi(offsetParam));
    }

    json items(json::array());
    for (const auto &[id, groupId, content, createdAt]: LongTermMemoryStore::listMemories(sessionId, limit, offset)) {
        json item;
        item["id"] = id;
        // 带类型的会话 ID可能超出 JS 安全整数范围，统一以字符串输出
        item["sessionId"] = groupId.toString();
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

auto AdminController::deleteLongTermMemory(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    if (!LongTermMemoryStore::deleteMemory(std::stoll(id))) {
        callback(jsonResponse(AdminResponse::errorJson("记忆不存在或已被删除")));
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("长期记忆已删除")));
    co_return;
}


// ==================== 群记忆 ====================

auto AdminController::getSessionMemory(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    std::string memory = MemoryStore::getShortTermMemory(gid);
    if (json parsed; tryParseJson(memory, parsed)) {
        if (auto structured = MemoryStore::normalizeSessionMemory(parsed)) memory = dumpJson(*structured, true, 2);
    }

    json resp;
    resp["sessionId"] = gid;
    resp["memory"] = memory;
    resp["conversationSummaries"] = MemoryStore::conversationSummaries(memory);
    callback(jsonResponse(resp));
    co_return;
}

auto AdminController::updateSessionMemory(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->contains("memory")) {
        callback(jsonResponse(AdminResponse::errorJson("缺少memory字段")));
        co_return;
    }

    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    std::string memory = getStr(*body, "memory");
    if (trim(memory).starts_with("{")) {
        json parsed;
        const auto structured = tryParseJson(memory, parsed) ? MemoryStore::normalizeSessionMemory(parsed) : std::nullopt;
        if (!structured) {
            auto response = jsonResponse(AdminResponse::errorJson("会话记忆 JSON 需要有效的 aliases 数组；pending_entries 如存在必须为字符串数组，仅接受当前格式"));
            response->setStatusCode(drogon::k400BadRequest);
            callback(response);
            co_return;
        }
        memory = dumpJson(*structured);
    }
    MemoryStore::updateShortTermMemory(gid, memory);

    callback(jsonResponse(AdminResponse::okJson("记忆已更新")));
    co_return;
}

// ==================== 好感度 ====================

auto AdminController::getPeople(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto people = PersonMemoryStore::listPeople();
    for (auto &person: people)
        addPersonName(person);
    callback(jsonResponse(people));
    co_return;
}

auto AdminController::getPerson(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &userId) const -> Task<> {
    const auto id = UserId::parse(userId);
    if (!id || id->toString() == "self" || id->toString() == "system") {
        callback(jsonResponse(AdminResponse::errorJson("无效的用户 ID")));
        co_return;
    }
    auto person = PersonMemoryStore::getPerson(*id);
    if (person.is_null()) {
        callback(jsonResponse(AdminResponse::errorJson("没有该人物的记录")));
        co_return;
    }
    addPersonName(person);
    callback(jsonResponse(person));
    co_return;
}

auto AdminController::consolidatePerson(HttpRequestPtr req,
  std::function<void(const HttpResponsePtr &)> callback, const std::string &userId) const -> Task<> {
    auto status = drogon::k400BadRequest;
    std::string message = "无效的用户 ID";
    const auto id = UserId::parse(userId);
    if (id && id->toString() != "self" && id->toString() != "system") {
        if (PersonMemoryStore::getPerson(*id).is_null()) {
            status = drogon::k404NotFound;
            message = "没有该人物的记录";
        } else {
            using Result = PersonMemoryConsolidator::Result;
            const auto result = co_await PersonMemoryConsolidator::instance().runPerson(*id);
            switch (result) {
            case Result::Updated:
                status = drogon::k200OK;
                message = "人物记忆已整理";
                break;
            case Result::NoMemory:
                status = drogon::k200OK;
                message = "暂无需要整理的记忆";
                break;
            case Result::Busy:
                status = drogon::k409Conflict;
                message = "人物整理正在进行，请稍后重试";
                break;
            case Result::Conflict:
                status = drogon::k409Conflict;
                message = "人物记忆在整理期间已变化，本次结果未保存，请刷新后重试";
                break;
            case Result::Stopped:
                status = drogon::k503ServiceUnavailable;
                message = "人物整理服务已停止，请稍后重试";
                break;
            case Result::Failed:
                status = drogon::k502BadGateway;
                message = "人物整理失败，模型请求失败或结果无效，原记忆已保留";
                break;
            }
        }
    }
    auto response = jsonResponse(status == drogon::k200OK ? AdminResponse::okJson(message) : AdminResponse::errorJson(message));
    response->setStatusCode(status);
    callback(response);
    co_return;
}

namespace {
    auto validPersonEdit(const json &body) -> bool {
        if (!body.is_object() || !body.contains("expected") || !body["expected"].is_object() ||
            !atOrNull(body["expected"], "pending_entries").is_array()) return false;
        const auto &edit = atOrNull(body, "replacement");
        if (!edit.is_object()) return false;
        const auto &score = atOrNull(edit, "affinity");
        if (Config::instance().affinityEnabled.load(std::memory_order_relaxed) &&
            (!score.is_number_integer() || score < -100 || score > 100)) return false;
        for (const auto key : {"profile", "relationship", "recent_interactions"}) {
            const auto &items = atOrNull(edit, key);
            if (!items.is_array()) return false;
            for (const auto &item : items)
                if (!item.is_string() || trim(item.get<std::string>()).empty()) return false;
        }
        const auto &entries = atOrNull(edit, "pending_entries");
        if (!entries.is_array()) return false;
        std::unordered_set<std::string> availableIds;
        for (const auto &entry : body["expected"]["pending_entries"])
            availableIds.insert(getStr(entry, "id"));
        for (const auto &entry : entries) {
            const auto id = getStr(entry, "id");
            // 每个已有 ID 只能使用一次，同时拒绝新增和重复的待整理条目。
            if (id.empty() || availableIds.erase(id) != 1 || !atOrNull(entry, "content").is_string() ||
                trim(getStr(entry, "content")).empty()) return false;
        }
        return true;
    }
}

auto AdminController::updatePerson(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &userId) const -> Task<> {
    const auto id = UserId::parse(userId);
    const auto body = parseJsonBody(req);
    auto status = drogon::k400BadRequest;
    std::string message = Config::instance().affinityEnabled.load(std::memory_order_relaxed)
      ? "人物数据无效：好感度须为 -100～100 的整数，记忆须为非空文本"
      : "人物数据无效：记忆须为非空文本";
    if (id && id->toString() != "self" && id->toString() != "system" && body && validPersonEdit(*body)) {
        const bool saved = PersonMemoryStore::updatePerson(*id, body->at("expected"), body->at("replacement"));
        status = saved ? drogon::k200OK : drogon::k409Conflict;
        message = saved ? "人物信息已保存" : "人物信息已变化，请重新读取后编辑；当前草稿已保留";
    }
    auto response = jsonResponse(status == drogon::k200OK ? AdminResponse::okJson(message) : AdminResponse::errorJson(message));
    response->setStatusCode(status);
    callback(response);
    co_return;
}

auto AdminController::deletePerson(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &userId) const -> Task<> {
    const auto id = UserId::parse(userId);
    const auto body = parseJsonBody(req);
    auto status = drogon::k400BadRequest;
    std::string message = "无效的人物删除请求";
    if (id && id->toString() != "self" && id->toString() != "system" && body && body->contains("expected") &&
        body->at("expected").is_object()) {
        const bool removed = PersonMemoryStore::deletePerson(*id, body->at("expected"));
        status = removed ? drogon::k200OK : drogon::k409Conflict;
        message = removed ? "人物已删除" : "人物信息已变化，请刷新后重试";
    }
    auto response = jsonResponse(status == drogon::k200OK ? AdminResponse::okJson(message) : AdminResponse::errorJson(message));
    response->setStatusCode(status);
    callback(response);
    co_return;
}

auto AdminController::getSessionAffinity(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    if (!Config::instance().affinityEnabled.load(std::memory_order_relaxed)) {
        auto response = jsonResponse(AdminResponse::errorJson("好感度已关闭"));
        response->setStatusCode(drogon::k403Forbidden);
        callback(response);
        co_return;
    }
    const auto parsedSession = SessionId::parse(sessionId);
    if (!parsedSession) {
        auto response = jsonResponse(AdminResponse::errorJson(parsedSession.error()));
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        co_return;
    }
    const auto gid = *parsedSession;
    auto affinityMap = AffinityStore::getAffinityMap();

    std::vector<std::pair<UserId, i32>> entries(affinityMap.begin(), affinityMap.end());
    std::ranges::sort(entries, [](const auto &a, const auto &b) -> auto { return a.second > b.second; });

    json list(json::array());
    for (const auto &[userId, affinity]: entries) {
        json item;
        // 用户 ID以字符串返回（超过 JS Number 安全范围）
        item["user_id"] = userId.toString();
        // 昵称优先取运行时映射（含自定义昵称），缺失时回退 OneBot 实时查询
        std::string name(QQNameDirectory::getName(userId));
        if (name.empty() || name == "未知") {
            if (const auto info = co_await OneBotClient::getStrangerInfo(userId, gid);
              atOrNull(info, "data").contains("nickname")) {
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
    resp["sessionId"] = gid.toString();
    resp["scope"] = "global";
    resp["affinities"] = list;
    callback(jsonResponse(resp));
    co_return;
}

// ==================== 定时任务 ====================

auto AdminController::getScheduledTasks(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback,
  const std::string &sessionId) const -> Task<> {
    const auto parsed = SessionId::parse(sessionId);
    if (!parsed) {
        callback(jsonResponse(AdminResponse::errorJson(parsed.error())));
        co_return;
    }
    const auto sid = *parsed;

    json list(json::array());
    for (const auto &task: TaskStore::getPendingScheduledTasksBySession(sid)) {
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

auto AdminController::cancelScheduledTask(
  HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback, const std::string &id) const -> Task<> {
    const u64 taskId = parseUInt64(id);
    if (taskId == 0) {
        callback(jsonResponse(AdminResponse::failJson("无效的任务 ID")));
        co_return;
    }

    if (TaskScheduler::instance().cancel(static_cast<i64>(taskId))) {
        json resp = AdminResponse::okJson(fmt::format("定时任务 #{} 已取消", taskId));
        Logger::info(std::nullopt, "Admin", fmt::format("已取消定时任务 #{}", taskId));
        callback(jsonResponse(resp));
    } else {
        callback(jsonResponse(AdminResponse::failJson("任务不存在或已触发/已取消")));
    }
    co_return;
}

// ==================== 记忆配置 ====================

auto AdminController::getMemoryConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto config = Config::instance().getMemoryConfig();
    callback(jsonResponse(config));
    co_return;
}

auto AdminController::saveMemoryConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    // 需就地补默认值，body 须可变
    auto body = parseJsonBody(req);
    if (!body || !body->is_object()) {
        callback(AdminResponse::configErrorResponse(
          {.type = ConfigErrorType::InvalidArgument, .message = "记忆配置必须是 JSON 对象"}));
        co_return;
    }

    if (auto result = Config::instance().saveMemoryConfig(*body); !result) {
        Logger::error(std::nullopt, "Config", result.error().message);
        callback(AdminResponse::configErrorResponse(result.error()));
        co_return;
    }

    callback(jsonResponse(AdminResponse::okJson("记忆配置已保存")));
    co_return;
}

// ==================== QQ Bot 配置 ====================

auto AdminController::getQQConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    auto config = Config::instance().getQQConfig();
    config["selfQQNumber"] = std::to_string(config["selfQQNumber"].get<u64>());
    callback(jsonResponse(config));
    co_return;
}

auto AdminController::saveQQConfig(HttpRequestPtr req, std::function<void(const HttpResponsePtr &)> callback) const
  -> Task<> {
    const auto body = parseJsonBody(req);
    if (!body || !body->is_object()) {
        callback(AdminResponse::configErrorResponse(
          {.type = ConfigErrorType::InvalidArgument, .message = "QQ 配置必须是 JSON 对象"}));
        co_return;
    }

    const auto previousConfig = Config::instance().getQQConfig();
    if (auto result = Config::instance().saveQQConfig(*body); !result) {
        Logger::error(std::nullopt, "Config", result.error().message);
        callback(AdminResponse::configErrorResponse(result.error()));
        co_return;
    }
    const auto &config = Config::instance();

    // 更新机器人自己的自定义昵称
    QQNameDirectory::setCustomName(UserId{config.selfQQNumber}, config.botName + "(我)");
    const auto savedConfig = config.getQQConfig();
    // 名称和私聊默认状态不影响连接，不应中断正在等待的 OneBot 动作。
    if (previousConfig["oneBotTransport"] != savedConfig["oneBotTransport"] ||
        (savedConfig["oneBotTransport"] == "websocket" &&
          (previousConfig["qqWebSocketHost"] != savedConfig["qqWebSocketHost"] ||
            previousConfig["accessToken"] != savedConfig["accessToken"]))) {
        OneBotWebSocketClient::instance().reconfigure();
    }

    callback(jsonResponse(AdminResponse::okJson("QQ Bot 配置已保存")));
    co_return;
}
