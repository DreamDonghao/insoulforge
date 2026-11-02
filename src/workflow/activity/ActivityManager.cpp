/// @file ActivityManager.cpp
/// @brief 以持久化快照连接外部活动服务，聊天上下文不等待网络。
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <vector>

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/Utilities.h>
#include <openssl/crypto.h>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/activity/ActivityManager.hpp>

namespace insoulforge {
    namespace {
        auto now() -> i64 {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
        }
        auto validOrigin(const json &origin) -> bool {
            return origin.is_object() && SessionId::parse(getStr(origin, "session_id")).has_value() &&
                   UserId::parse(getStr(origin, "user_id")).has_value();
        }
        auto sameToken(const std::string &expected, const std::string &actual) -> bool {
            return !expected.empty() && expected.size() == actual.size() &&
                   CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) == 0;
        }
        auto failure(const std::string &message) -> json { return {{"success", false}, {"error", message}}; }
        auto serviceDefinition(const json &value) -> json {
            return {{"name", getStr(value, "name")}, {"description", getStr(value, "description")},
              {"enabled", atOrNull(value, "enabled")}};
        }
    } // namespace

    ActivityManager::ActivityManager(std::string statePath, EventSink sink) :
        m_statePath(std::move(statePath)), m_sink(std::move(sink)) {}

    auto ActivityManager::instance() -> ActivityManager & {
        static auto manager = ActivityManager{"data/activity-state.json", [](Event event) {
                                                  OneBotEventWorkflow::instance().enqueueActivityMessage(
                                                    event.sessionId, std::move(event.text), std::move(event.name),
                                                    std::move(event.mode), std::move(event.origin));
                                              }};
        return manager;
    }

    auto ActivityManager::validateConfiguration(const json &config) -> std::expected<void, std::string> {
        if (!config.is_object() || !atOrNull(config, "servers").is_array())
            return std::unexpected("活动配置需要 servers 数组");
        const auto token = getStr(config, "token");
        if ((config.contains("token") && !config["token"].is_string()) ||
            (!token.empty() && (token.size() < 16 || token.find_first_of("\r\n") != std::string::npos)))
            return std::unexpected("接入 Token 至少需要 16 个字符且不能包含换行");
        auto names = std::set<std::string>{};
        for (const auto &server: config["servers"]) {
            const auto name = getStr(server, "name");
            if (!server.is_object() || name.empty() || name.size() > 80 ||
                std::ranges::any_of(name, [](unsigned char c) { return c < 32; }) || !names.insert(name).second)
                return std::unexpected("活动名称必须非空、唯一且不含控制字符（最多 80 字节）");
            if (!atOrNull(server, "description").is_string() || !atOrNull(server, "enabled").is_boolean() ||
                getStr(server, "description").size() > 4096)
                return std::unexpected("活动需要描述和启用状态，描述最多 4096 字节");
        }
        return {};
    }

    auto ActivityManager::initialize(json config) -> std::expected<void, std::string> {
        if (auto valid = validateConfiguration(config); !valid)
            return valid;
        std::lock_guard lock(m_mutex);
        try {
            auto saved = json::object();
            if (std::filesystem::exists(m_statePath)) {
                auto input = std::ifstream(m_statePath);
                input >> saved;
                if (!saved.is_object())
                    return std::unexpected("活动状态文件必须是 JSON 对象");
            }
            auto services = Services{};
            for (const auto &definition: config["servers"]) {
                auto service = std::make_shared<Service>();
                service->config = serviceDefinition(definition);
                const auto name = getStr(definition, "name");
                if (saved.contains(name)) {
                    const auto &runtime = saved[name];
                    if (!runtime.is_object() || !atOrNull(runtime, "running").is_boolean() ||
                        !atOrNull(runtime, "run_id").is_string() ||
                        !atOrNull(runtime, "sequence").is_number_integer() ||
                        (getBool(runtime, "running") &&
                          (!validOrigin(atOrNull(runtime, "origin")) || getStr(runtime, "run_id").empty() ||
                            !atOrNull(runtime, "status").is_string())))
                        return std::unexpected("活动状态文件无效，拒绝丢弃运行来源");
                    service->runtime = runtime;
                }
                services.emplace(name, std::move(service));
            }
            m_services = std::move(services);
            m_token = getStr(config, "token");
            m_stopping = false;
            return {};
        } catch (const std::exception &) {
            return std::unexpected("无法读取活动状态文件，请检查文件格式和权限");
        }
    }

    auto ActivityManager::configuration() const -> json {
        std::lock_guard lock(m_mutex);
        auto servers = json::array();
        for (const auto &[name, service]: m_services) {
            auto item = service->config;
            item["running"] = getBool(service->runtime, "running");
            item["status"] = getStr(service->runtime, "status");
            item["connected"] = service->connection && service->connection->connected();
            item["synchronized"] = service->synchronized;
            item["busy"] = service->busy;
            item["updatedAt"] = service->runtime.value("updated_at", i64{0});
            servers.push_back(std::move(item));
        }
        return {{"token", m_token}, {"servers", std::move(servers)}};
    }

    auto ActivityManager::configurationLocked() const -> json {
        auto servers = json::array();
        for (const auto &[name, service]: m_services)
            servers.push_back(service->config);
        return {{"token", m_token}, {"servers", std::move(servers)}};
    }

    auto ActivityManager::saveToken(std::string token, Config &store) -> std::expected<void, std::string> {
        if (!token.empty() && (token.size() < 16 || token.find_first_of("\r\n") != std::string::npos))
            return std::unexpected("接入 Token 至少需要 16 个字符且不能包含换行");
        std::vector<drogon::WebSocketConnectionPtr> connections;
        {
            std::lock_guard lock(m_mutex);
            auto previous = std::exchange(m_token, std::move(token));
            if (auto saved = store.saveActivitiesConfig(configurationLocked()); !saved) {
                m_token = std::move(previous);
                return std::unexpected(saved.error().message);
            }
            if (previous == m_token)
                return {};
            ++m_authGeneration;
            for (const auto &[connection, registration]: m_connections)
                connections.push_back(connection);
            // 在释放锁前撤销旧连接的写入与请求权限，避免关闭回调到达前仍接受操作。
            for (const auto &[name, service]: m_services) {
                service->connection.reset();
                service->synchronized = service->busy = false;
            }
        }
        for (const auto &connection: connections) {
            disconnect(connection);
            connection->shutdown();
        }
        return {};
    }

    auto ActivityManager::setEnabled(const std::string &name, bool enabled, Config &store)
      -> std::expected<void, std::string> {
        std::lock_guard lock(m_mutex);
        const auto found = m_services.find(name);
        if (found == m_services.end())
            return std::unexpected("活动服务不存在");
        auto &service = *found->second;
        const auto previous = getBool(service.config, "enabled");
        if (previous == enabled)
            return {};
        if (service.busy || getBool(service.runtime, "running"))
            return std::unexpected("请先确认活动结束及请求处理完成，再修改启用状态");
        service.config["enabled"] = enabled;
        if (auto saved = store.saveActivitiesConfig(configurationLocked()); !saved) {
            service.config["enabled"] = previous;
            return std::unexpected(saved.error().message);
        }
        return {};
    }

    auto ActivityManager::removeService(const std::string &name, Config &store, bool force)
      -> std::expected<void, std::string> {
        std::unique_lock lock(m_mutex);
        const auto found = m_services.find(name);
        if (found == m_services.end())
            return std::unexpected("活动服务不存在");
        const auto service = found->second;
        if (!force && (service->connection || service->busy || getBool(service->runtime, "running")))
            return std::unexpected("只能删除离线且已经结束的活动服务");
        m_services.erase(found);
        if (auto saved = store.saveActivitiesConfig(configurationLocked()); !saved) {
            m_services.emplace(name, service);
            return std::unexpected(saved.error().message);
        }
        const auto connection = std::exchange(service->connection, {});
        service->synchronized = service->busy = false;
        const auto saved = persistLocked();
        lock.unlock();
        if (connection) {
            disconnect(connection);
            connection->shutdown();
        }
        return saved;
    }

    auto ActivityManager::directory() const -> json {
        std::lock_guard lock(m_mutex);
        auto result = json::array();
        for (const auto &[name, service]: m_services)
            if (getBool(service->config, "enabled"))
                result.push_back({{"name", name}, {"description", getStr(service->config, "description")}});
        return result;
    }

    auto ActivityManager::runningActivities() const -> json {
        std::lock_guard lock(m_mutex);
        auto result = json::array();
        for (const auto &[name, service]: m_services) {
            if (!getBool(service->config, "enabled") || !getBool(service->runtime, "running"))
                continue;
            auto status = getStr(service->runtime, "status");
            if (!service->connection || !service->connection->connected() || !service->synchronized)
                status += "（状态未确认，最近更新时间 Unix 秒：" +
                          std::to_string(service->runtime.value("updated_at", i64{0})) + "）";
            result.push_back({{"name", name}, {"status", status}});
        }
        return result;
    }

    auto ActivityManager::persistLocked() const -> std::expected<void, std::string> {
        try {
            const auto path = std::filesystem::path(m_statePath);
            if (!path.parent_path().empty())
                std::filesystem::create_directories(path.parent_path());
            auto document = json::object();
            for (const auto &[name, service]: m_services)
                document[name] = service->runtime;
            const auto temporary = m_statePath + ".tmp";
            {
                auto output = std::ofstream(temporary, std::ios::trunc);
                output << dumpJson(document, true, 2);
                output.flush();
                if (!output)
                    return std::unexpected("无法写入活动状态文件");
            }
            std::filesystem::rename(temporary, path);
            return {};
        } catch (const std::exception &) {
            return std::unexpected("无法原子保存活动状态文件");
        }
    }

    auto ActivityManager::applySnapshotLocked(Service &service, const json &snapshot, const bool fromEvent)
      -> std::expected<void, std::string> {
        if (!snapshot.is_object() || !atOrNull(snapshot, "running").is_boolean())
            return std::unexpected("活动响应缺少 running 布尔字段");
        const auto running = getBool(snapshot, "running");
        const auto runId = getStr(snapshot, "run_id");
        auto currentId = getStr(service.runtime, "run_id");
        if (runId.empty() && !running && !fromEvent) {
            // 从未启动的服务可返回简单空闲状态；已运行时必须明确报告对应运行已结束。
            if (getBool(service.runtime, "running")) {
                if (!getBool(service.runtime, "starting"))
                    return std::unexpected("服务未确认原活动的结束状态");
                auto confirmed = service.runtime;
                confirmed["running"] = false;
                confirmed["starting"] = false;
                confirmed["sequence"] = 0;
                confirmed["status"] = "服务确认活动未启动";
                return applySnapshotLocked(service, confirmed, false);
            }
            service.synchronized = true;
            return {};
        }
        if (runId.empty() || !atOrNull(snapshot, "sequence").is_number_integer() ||
            !atOrNull(snapshot, "status").is_string() || getStr(snapshot, "status").size() > 4096)
            return std::unexpected("活动状态需要 run_id、非负 sequence 和不超过 4096 字节的 status");
        i64 sequence;
        try {
            sequence = snapshot.at("sequence").get<i64>();
        } catch (...) {
            return std::unexpected("sequence 超出整数范围");
        }
        if (sequence < 0)
            return std::unexpected("sequence 必须非负");
        // 多个主程序竞争同一外部服务时，以启动后的权威查询恢复已存在实例；事件不能切换实例。
        if (!fromEvent && currentId != runId && getBool(service.runtime, "starting"))
            currentId.clear();
        if (!currentId.empty() && currentId != runId)
            return std::unexpected("活动启动标识不匹配");
        if (fromEvent && currentId.empty())
            return std::unexpected("尚未恢复该活动，请等待状态同步");
        const auto currentSequence = service.runtime.value("sequence", i64{-1});
        if (currentId == runId && sequence <= currentSequence) {
            if (sequence == currentSequence && (running != getBool(service.runtime, "running") ||
                                                 getStr(snapshot, "status") != getStr(service.runtime, "status")))
                return std::unexpected("同一活动序号的状态不能改变");
            if (!fromEvent) {
                service.synchronized = true;
                service.runtime["updated_at"] = now();
            }
            return {};
        }
        if (currentId == runId && !getBool(service.runtime, "running"))
            return std::unexpected("已结束的活动不能被旧事件重新激活");
        const auto origin = currentId.empty() ? atOrNull(snapshot, "origin") : atOrNull(service.runtime, "origin");
        if (!validOrigin(origin))
            return std::unexpected("活动缺少有效的启动来源");
        auto previous = service.runtime;
        service.runtime = {{"running", running}, {"run_id", runId}, {"sequence", sequence},
          {"status", getStr(snapshot, "status")}, {"origin", origin}, {"updated_at", now()},
          {"event_sequence", currentId == runId ? previous.value("event_sequence", i64{-1}) : -1}};
        if (auto saved = persistLocked(); !saved) {
            service.runtime = std::move(previous);
            return std::unexpected(saved.error());
        }
        service.synchronized = true;
        return {};
    }

    auto ActivityManager::authorized(const std::string &token) const -> bool {
        std::lock_guard lock(m_mutex);
        return !m_stopping && sameToken(m_token, token);
    }

    auto ActivityManager::connect(const std::string &token, drogon::WebSocketConnectionPtr connection) -> bool {
        std::lock_guard lock(m_mutex);
        if (m_stopping || !sameToken(m_token, token))
            return false;
        connection->disablePing();
        auto *loop = m_loop ? m_loop : drogon::app().getLoop();
        const auto timer = loop->runAfter(10, [this, connection] {
            bool expired;
            {
                std::lock_guard lock(m_mutex);
                const auto found = m_connections.find(connection);
                expired = found != m_connections.end() && found->second.name.empty();
            }
            if (expired) {
                disconnect(connection);
                connection->shutdown(drogon::CloseCode::kViolation, "Activity registration timeout");
            }
        });
        m_connections.emplace(connection, Registration{"", m_authGeneration, loop, timer});
        return true;
    }

    auto ActivityManager::registerService(const drogon::WebSocketConnectionPtr &connection, const json &message)
      -> std::expected<void, std::string> {
        const auto name = getStr(message, "name");
        auto definition = json{{"name", name}, {"description", atOrNull(message, "description")}, {"enabled", true}};
        if (auto valid = validateConfiguration({{"servers", json::array({definition})}}); !valid)
            return valid;
        drogon::WebSocketConnectionPtr previous;
        {
            std::lock_guard lock(m_mutex);
            const auto registration = m_connections.find(connection);
            if (m_stopping || registration == m_connections.end() || !registration->second.name.empty() ||
                registration->second.generation != m_authGeneration)
                return std::unexpected("活动连接无法注册");
            const auto found = m_services.find(name);
            const auto isNew = found == m_services.end();
            auto service = isNew ? std::make_shared<Service>() : found->second;
            const auto previousDefinition = service->config;
            if (!isNew)
                definition["enabled"] = getBool(service->config, "enabled");
            service->config = definition;
            m_services[name] = service;
            if (auto saved = Config::instance().saveActivitiesConfig(configurationLocked()); !saved) {
                if (isNew)
                    m_services.erase(name);
                else
                    service->config = previousDefinition;
                return std::unexpected(saved.error().message);
            }
            registration->second.name = name;
            registration->second.loop->invalidateTimer(registration->second.timer);
            previous = std::exchange(service->connection, connection);
            service->synchronized = service->busy = false;
            service->lastSeen = std::chrono::steady_clock::now();
        }
        if (previous) {
            disconnect(previous);
            previous->shutdown();
        }
        drogon::async_run(
          [this, name, connection]() -> drogon::Task<> { co_await synchronize(name, connection, SyncMode::Recovery); });
        return {};
    }

    ActivityManager::ResponseAwaiter::ResponseAwaiter(ActivityManager &manager, std::string name,
      drogon::WebSocketConnectionPtr connection, std::string operation, json body) :
        m_manager(manager), m_name(std::move(name)), m_connection(std::move(connection)),
        m_operation(std::move(operation)), m_body(std::move(body)) {}

    auto ActivityManager::ResponseAwaiter::await_suspend(std::coroutine_handle<> continuation) -> bool {
        auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (!loop)
            loop = drogon::app().getLoop();
        // 先挂起再发送，所有完成路径都回到调用协程的事件循环，避免同步完成和跨线程恢复竞态。
        loop->queueInLoop([this, loop, continuation] {
            m_manager.sendRequest(std::move(m_name), std::move(m_connection), std::move(m_operation), std::move(m_body),
              loop, [this, continuation](json value) {
                  setValue(std::move(value));
                  continuation.resume();
              });
        });
        return true;
    }

    void ActivityManager::sendRequest(std::string name, drogon::WebSocketConnectionPtr connection,
      std::string operation, json body, trantor::EventLoop *loop, std::function<void(json)> callback) {
        const auto id = drogon::utils::getUuid();
        auto message =
          json{{"type", "request"}, {"request_id", id}, {"operation", operation}, {"body", std::move(body)}};
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || found->second->connection != connection ||
                !connection->connected()) {
                loop->queueInLoop(
                  [callback = std::move(callback)] { callback(failure("活动连接已断开，结果未确认")); });
                return;
            }
            const auto timer = loop->runAfter(
              10, [this, id, connection] { completeRequest(id, connection, failure("活动请求超时，结果未确认")); });
            m_pending.emplace(id, Pending{connection, loop, timer, std::move(callback), name, message});
        }
        try {
            connection->send(dumpJson(message));
        } catch (const std::exception &) {
            completeRequest(id, connection, failure("活动请求发送失败，结果未确认"));
        }
    }

    void ActivityManager::completeRequest(
      const std::string &id, const drogon::WebSocketConnectionPtr &connection, json response) {
        std::optional<Pending> pending;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_pending.find(id);
            if (found == m_pending.end() || found->second.connection != connection)
                return;
            pending = std::move(found->second);
            m_pending.erase(found);
        }
        pending->loop->invalidateTimer(pending->timer);
        HttpTrace::instance().append({.tag = "Activity",
          .method = "WS",
          .url = "/activities/ws?name=" + drogon::utils::urlEncodeComponent(pending->name) + "#" +
                 getStr(pending->message, "operation"),
          .requestBody = dumpJson(pending->message),
          .status = getBool(response, "success") ? 200 : 0,
          .responseBody = dumpJson(response)});
        pending->loop->queueInLoop([callback = std::move(pending->callback), response = std::move(response)]() mutable {
            callback(std::move(response));
        });
    }

    auto ActivityManager::request(std::string name, drogon::WebSocketConnectionPtr connection, std::string operation,
      json body) -> drogon::Task<json> {
        auto response = co_await ResponseAwaiter(
          *this, std::move(name), std::move(connection), std::move(operation), std::move(body));
        if (!getBool(response, "success"))
            throw std::runtime_error(getStr(response, "error", "活动服务请求失败，结果未确认"));
        if (!atOrNull(response, "result").is_object())
            throw std::runtime_error("活动服务 result 必须为 JSON 对象");
        co_return std::move(response["result"]);
    }

    auto ActivityManager::refresh(std::string name) -> drogon::Task<std::expected<void, std::string>> {
        drogon::WebSocketConnectionPtr connection;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end())
                co_return std::unexpected("活动服务不存在或管理器已停止");
            connection = found->second->connection;
            if (!connection || !connection->connected())
                co_return std::unexpected("服务未连接，无法同步状态");
            if (!found->second->synchronized)
                co_return std::unexpected("请等待连接恢复完成后再同步");
        }
        co_return co_await synchronize(name, std::move(connection), SyncMode::Check);
    }

    auto ActivityManager::synchronize(std::string name, drogon::WebSocketConnectionPtr connection, SyncMode mode)
      -> drogon::Task<std::expected<void, std::string>> {
        std::shared_ptr<Service> service;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || found->second->connection != connection)
                co_return std::unexpected("活动连接已断开或更换");
            // 工具确认沿用 invoke 的占用，其他同步独立取得占用。
            if (mode != SyncMode::AfterOperation && found->second->busy)
                co_return std::unexpected("活动正在处理请求，请稍后同步");
            service = found->second;
            service->busy = true;
            service->lastCheck = std::chrono::steady_clock::now();
        }
        std::string error;
        try {
            auto snapshot = co_await request(name, connection, "status", json::object());
            std::lock_guard lock(m_mutex);
            if (!m_stopping && service->connection == connection) {
                if (auto result = applySnapshotLocked(*service, snapshot, false); !result)
                    error = result.error();
            }
        } catch (const std::exception &e) {
            error = e.what();
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || service->connection != connection)
                co_return std::unexpected("活动连接已断开或更换");
            service->busy = false;
            if (!error.empty())
                service->synchronized = false;
        }
        if (!error.empty()) {
            Logger::warn(std::nullopt, "Activity", "活动状态同步失败，等待服务重新连接: " + name);
            disconnect(connection);
            connection->shutdown();
            co_return std::unexpected(error);
        } else if (mode == SyncMode::Recovery)
            connection->send(dumpJson(json{{"type", "ready"}}));
        co_return std::expected<void, std::string>{};
    }

    void ActivityManager::disconnect(const drogon::WebSocketConnectionPtr &connection) {
        std::vector<std::string> pending;
        {
            std::lock_guard lock(m_mutex);
            const auto registration = m_connections.find(connection);
            if (registration == m_connections.end())
                return;
            const auto name = registration->second.name;
            registration->second.loop->invalidateTimer(registration->second.timer);
            m_connections.erase(registration);
            const auto found = m_services.find(name);
            if (found != m_services.end() && found->second->connection == connection) {
                found->second->connection.reset();
                found->second->synchronized = found->second->busy = false;
            }
            for (const auto &[id, request]: m_pending)
                if (request.connection == connection)
                    pending.push_back(id);
        }
        for (const auto &id: pending)
            completeRequest(id, connection, failure("活动连接已断开或更换，结果未确认"));
    }

    void ActivityManager::receive(
      const drogon::WebSocketConnectionPtr &connection, const std::string &message, drogon::WebSocketMessageType type) {
        std::string name;
        {
            std::lock_guard lock(m_mutex);
            const auto registration = m_connections.find(connection);
            if (m_stopping || registration == m_connections.end() ||
                registration->second.generation != m_authGeneration)
                return;
            name = registration->second.name;
            if (!name.empty()) {
                const auto found = m_services.find(name);
                if (found == m_services.end() || found->second->connection != connection)
                    return;
                found->second->lastSeen = std::chrono::steady_clock::now();
            }
        }
        if (type == drogon::WebSocketMessageType::Ping || type == drogon::WebSocketMessageType::Pong ||
            type == drogon::WebSocketMessageType::Close)
            return;
        json value;
        if (type != drogon::WebSocketMessageType::Text || message.size() > 1024 * 1024 ||
            !tryParseJson(message, value) || !value.is_object()) {
            disconnect(connection);
            connection->shutdown(drogon::CloseCode::kViolation, "Invalid activity message");
            return;
        }
        if (name.empty()) {
            const auto registered = getStr(value, "type") == "register" && message.size() <= 128 * 1024
                                      ? registerService(connection, value)
                                      : std::expected<void, std::string>{std::unexpected("首条消息必须为 register")};
            if (!registered) {
                disconnect(connection);
                connection->shutdown(drogon::CloseCode::kViolation, "Invalid activity registration");
            }
            return;
        }
        if (getStr(value, "type") == "response" && !getStr(value, "request_id").empty() &&
            atOrNull(value, "success").is_boolean()) {
            const auto id = getStr(value, "request_id");
            completeRequest(id, connection, std::move(value));
        } else if (getStr(value, "type") == "event" && atOrNull(value, "event").is_object()) {
            const auto &event = value["event"];
            const auto result = message.size() <= 128 * 1024
                                  ? receiveEvent(name, connection, event)
                                  : std::expected<void, std::string>{std::unexpected("活动事件过大")};
            auto ack = json{{"type", "event_ack"}, {"run_id", getStr(event, "run_id")},
              {"sequence", atOrNull(event, "sequence")}, {"success", result.has_value()}};
            if (!result)
                ack["error"] = result.error();
            connection->send(dumpJson(ack));
            HttpTrace::instance().append({.tag = "Activity",
              .method = "WS",
              .url = "/activities/ws?name=" + drogon::utils::urlEncodeComponent(name) + "#event",
              .requestBody = message,
              .status = result ? 200 : 400,
              .responseBody = dumpJson(ack)});
        } else {
            disconnect(connection);
            connection->shutdown(drogon::CloseCode::kViolation, "Unknown activity message");
        }
    }

    auto ActivityManager::invoke(std::string operation, std::string name, json arguments, json origin)
      -> drogon::Task<json> {
        std::shared_ptr<Service> service;
        drogon::WebSocketConnectionPtr connection;
        json body;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || !getBool(found->second->config, "enabled"))
                co_return failure("活动不存在或未启用");
            service = found->second;
            if (service->busy)
                co_return failure("活动正在处理请求，请稍后查询状态");
            if (!validOrigin(origin))
                co_return failure("缺少可信的调用来源");
            if (operation != "start" && operation != "command" && operation != "stop" && operation != "details")
                co_return failure("未知活动操作");
            if (!service->synchronized || !service->connection)
                co_return failure("活动服务状态尚未确认，请等待服务连接并同步");
            if (operation == "start") {
                if (getBool(service->runtime, "running"))
                    co_return failure("活动已在全局运行：" + getStr(service->runtime, "status"));
                auto previous = service->runtime;
                service->runtime = {{"running", true}, {"run_id", drogon::utils::getUuid()}, {"origin", origin},
                  {"sequence", -1}, {"starting", true}, {"status", "启动结果待确认"}, {"updated_at", now()}};
                if (auto saved = persistLocked(); !saved) {
                    service->runtime = std::move(previous);
                    co_return failure(saved.error());
                }
            } else if (!getBool(service->runtime, "running"))
                co_return failure("活动当前没有运行");
            body = {{"run_id", getStr(service->runtime, "run_id")}, {"origin", service->runtime["origin"]},
              {"caller", std::move(origin)}, {"arguments", std::move(arguments)}};
            service->busy = true;
            connection = service->connection;
        }
        json result;
        auto error = std::string{};
        try {
            result = co_await request(name, connection, operation, std::move(body));
            if (result.contains("snapshot")) {
                std::lock_guard lock(m_mutex);
                if (m_stopping || service->connection != connection)
                    co_return failure("活动连接已更换，操作结果未确认");
                const auto applied = applySnapshotLocked(*service, result["snapshot"], false);
                if (!applied)
                    error = applied.error();
            }
        } catch (const std::exception &e) {
            error = e.what();
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping || service->connection != connection)
                co_return failure("活动连接已断开或更换，操作结果未确认");
            if (!error.empty())
                service->synchronized = false;
        }
        // 无论响应是否完整，都确认真实状态；超时不会自动重发有副作用的请求。
        const auto synchronized = co_await synchronize(name, connection, SyncMode::AfterOperation);
        if (!synchronized && error.empty())
            error = synchronized.error();
        if (!error.empty())
            co_return failure(error + "；请查询活动状态，不要直接重复启动");
        co_return result;
    }

    auto ActivityManager::receiveEvent(const std::string &name, const drogon::WebSocketConnectionPtr &connection,
      const json &event) -> std::expected<void, std::string> {
        std::optional<Event> notification;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || !found->second->synchronized ||
                found->second->connection != connection)
                return std::unexpected("活动连接未就绪或已经更换");
            auto &service = *found->second;
            const auto mode = getStr(event, "mode", "silent");
            if (event.contains("mode") && !event["mode"].is_string())
                return std::unexpected("mode 必须是字符串");
            if (mode != "silent" && mode != "judge" && mode != "force")
                return std::unexpected("mode 必须为 silent、judge 或 force");
            const auto text = getStr(event, "text");
            if (event.contains("text") && (!event["text"].is_string() || text.size() > 65536))
                return std::unexpected("通知正文最多 65536 字节");
            const auto target = event.contains("target_session")
                                  ? getStr(event, "target_session")
                                  : getStr(atOrNull(service.runtime, "origin"), "session_id");
            const auto session = SessionId::parse(target);
            if (!text.empty() && !session)
                return std::unexpected("无效的目标会话");
            const auto applied = applySnapshotLocked(service, event, true);
            if (!applied)
                return std::unexpected(applied.error());
            auto &runtime = service.runtime;
            const auto sequence = event.at("sequence").get<i64>();
            // 查询/工具响应可先同步状态；同序号的事件仍有一次通知机会。旧事件不反向覆盖新进展。
            if (sequence < runtime.value("sequence", i64{-1}) || sequence <= runtime.value("event_sequence", i64{-1}))
                return {};
            const auto previousEvent = runtime.value("event_sequence", i64{-1});
            runtime["event_sequence"] = sequence;
            if (auto saved = persistLocked(); !saved) {
                runtime["event_sequence"] = previousEvent;
                return std::unexpected(saved.error());
            }
            if (!text.empty() && getBool(service.config, "enabled"))
                notification = Event{*session, name, text, mode, runtime.at("origin")};
        }
        if (notification && m_sink) {
            try {
                m_sink(std::move(*notification));
            } catch (const std::exception &) {
                return std::unexpected("活动状态已保存，但通知投递失败");
            }
        }
        return {};
    }

    void ActivityManager::tick() {
        std::vector<drogon::WebSocketConnectionPtr> expired;
        std::vector<std::pair<std::string, drogon::WebSocketConnectionPtr>> due;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            const auto current = std::chrono::steady_clock::now();
            for (const auto &[name, service]: m_services) {
                if (!service->connection)
                    continue;
                if (current - service->lastSeen > std::chrono::seconds(45))
                    expired.push_back(service->connection);
                else if (service->synchronized && !service->busy && current - service->lastCheck >= m_checkInterval)
                    due.emplace_back(name, service->connection);
            }
        }
        for (const auto &connection: expired) {
            disconnect(connection);
            connection->shutdown();
        }
        for (const auto &[name, connection]: due)
            drogon::async_run([this, name, connection]() -> drogon::Task<> {
                co_await synchronize(name, connection, SyncMode::Check);
            });
    }

    void ActivityManager::start(trantor::EventLoop &loop, std::chrono::seconds checkInterval) {
        std::lock_guard lock(m_mutex);
        if (m_loop)
            return;
        m_stopping = false;
        m_loop = &loop;
        m_checkInterval = std::max(checkInterval, std::chrono::seconds{1});
        m_timer = loop.runEvery(std::min(5.0, static_cast<double>(m_checkInterval.count())), [this] { tick(); });
    }

    void ActivityManager::stop() {
        std::vector<drogon::WebSocketConnectionPtr> connections;
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            if (m_loop) {
                m_loop->invalidateTimer(m_timer);
                m_loop = nullptr;
            }
            for (const auto &[connection, registration]: m_connections)
                connections.push_back(connection);
        }
        for (const auto &connection: connections) {
            disconnect(connection);
            connection->shutdown();
        }
    }
} // namespace insoulforge
