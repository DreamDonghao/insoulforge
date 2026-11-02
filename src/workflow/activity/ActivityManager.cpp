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
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/activity/ActivityManager.hpp>

namespace insoulforge {
    namespace {
        auto now() -> i64 {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
        }
        auto validUrl(const std::string &url) -> bool {
            return (url.starts_with("http://") || url.starts_with("https://")) &&
                   url.size() > (url.starts_with("https://") ? 8U : 7U) &&
                   url.find_first_of("\r\n\t @?#") == std::string::npos;
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
            return {{"name", getStr(value, "name")}, {"url", getStr(value, "url")},
              {"description", getStr(value, "description")}, {"enabled", atOrNull(value, "enabled")},
              {"token", getStr(value, "token")}};
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
        if (!config.is_object() || !atOrNull(config, "servers").is_array() ||
            !atOrNull(config, "callbackUrl").is_string())
            return std::unexpected("活动配置需要 callbackUrl 和 servers 数组");
        const auto callback = getStr(config, "callbackUrl");
        if (!callback.empty() && !validUrl(callback))
            return std::unexpected("回调地址必须是无凭据、查询参数或片段的 HTTP(S) URL");
        auto names = std::set<std::string>{};
        for (const auto &server: config["servers"]) {
            const auto name = getStr(server, "name");
            if (!server.is_object() || name.empty() || name.size() > 80 ||
                std::ranges::any_of(name, [](unsigned char c) { return c < 32; }) || !names.insert(name).second)
                return std::unexpected("活动名称必须非空、唯一且不含控制字符（最多 80 字节）");
            if (!validUrl(getStr(server, "url")) || !atOrNull(server, "description").is_string() ||
                !atOrNull(server, "enabled").is_boolean() || !atOrNull(server, "token").is_string() ||
                getStr(server, "token").size() < 16 || getStr(server, "description").size() > 4096 ||
                getStr(server, "token").find_first_of("\r\n") != std::string::npos)
                return std::unexpected("活动需要有效地址、描述、启用状态和至少 16 字符的共享密钥");
            if (getBool(server, "enabled") && callback.empty())
                return std::unexpected("启用活动服务前需要设置 action 能访问的完整回调地址");
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
            m_callbackUrl = getStr(config, "callbackUrl");
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
            item["reachable"] = service->reachable;
            item["synchronized"] = service->synchronized;
            item["busy"] = service->busy;
            item["updatedAt"] = service->runtime.value("updated_at", i64{0});
            servers.push_back(std::move(item));
        }
        return {{"callbackUrl", m_callbackUrl}, {"servers", std::move(servers)}};
    }

    auto ActivityManager::saveConfiguration(json config, Config &store) -> std::expected<void, std::string> {
        std::lock_guard lock(m_mutex);
        if (!atOrNull(config, "servers").is_array())
            return std::unexpected("servers 必须是数组");
        // 空密钥仍表示沿用；运行中或正在同步的服务不允许换地址、删除或停用。
        auto next = Services{};
        for (auto &definition: config["servers"]) {
            if (!definition.is_object())
                return std::unexpected("无效的活动服务");
            const auto name = getStr(definition, "name");
            const auto old = m_services.find(name);
            auto token = getStr(definition, "token");
            if (token.empty() && old != m_services.end())
                token = getStr(old->second->config, "token");
            definition["token"] = token;
            definition = serviceDefinition(definition);
            if (old != m_services.end())
                next.emplace(name, old->second);
            else
                next.emplace(name, std::make_shared<Service>());
        }
        if (auto valid = validateConfiguration(config); !valid)
            return valid;
        for (const auto &[name, service]: m_services) {
            if (service->busy || getBool(service->runtime, "running")) {
                const auto found =
                  std::ranges::find_if(config["servers"], [&](const json &s) { return getStr(s, "name") == name; });
                if (found == config["servers"].end() || *found != service->config ||
                    getStr(config, "callbackUrl") != m_callbackUrl)
                    return std::unexpected("请先确认活动结束及服务同步完成，再修改或删除其配置");
            }
        }
        if (auto result = store.saveActivitiesConfig(config); !result)
            return std::unexpected(result.error().message);
        for (const auto &definition: config["servers"]) {
            auto &service = *next.at(getStr(definition, "name"));
            if (service.config != definition) {
                service.synchronized = false;
                service.reachable = false;
                service.nextSync = 0;
            }
            service.config = definition;
        }
        m_services = std::move(next);
        m_callbackUrl = getStr(config, "callbackUrl");
        return persistLocked();
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
            if (!getBool(service->runtime, "running"))
                continue;
            auto status = getStr(service->runtime, "status");
            if (!service->reachable || now() - service->runtime.value("updated_at", i64{0}) > 90)
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

    auto ActivityManager::applySnapshotLocked(Service &service, const json &snapshot, const bool callback)
      -> std::expected<void, std::string> {
        if (!snapshot.is_object() || !atOrNull(snapshot, "running").is_boolean())
            return std::unexpected("活动响应缺少 running 布尔字段");
        const auto running = getBool(snapshot, "running");
        const auto runId = getStr(snapshot, "run_id");
        auto currentId = getStr(service.runtime, "run_id");
        if (runId.empty() && !running && !callback) {
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
            service.synchronized = service.reachable = true;
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
        // 多个主程序竞争同一外部服务时，以启动后的权威查询恢复已存在实例；回调不能切换实例。
        if (!callback && currentId != runId && getBool(service.runtime, "starting"))
            currentId.clear();
        if (!currentId.empty() && currentId != runId)
            return std::unexpected("活动启动标识不匹配");
        if (callback && currentId.empty())
            return std::unexpected("尚未恢复该活动，请等待状态同步");
        const auto currentSequence = service.runtime.value("sequence", i64{-1});
        if (currentId == runId && sequence <= currentSequence) {
            if (sequence == currentSequence && (running != getBool(service.runtime, "running") ||
                                                 getStr(snapshot, "status") != getStr(service.runtime, "status")))
                return std::unexpected("同一活动序号的状态不能改变");
            if (!callback) {
                service.synchronized = service.reachable = true;
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
        service.synchronized = service.reachable = true;
        service.failures = 0;
        service.nextSync = now() + 30;
        return {};
    }

    auto ActivityManager::request(json config, std::string operation, json body) -> drogon::Task<json> {
        const auto response = co_await HttpUtil::send("Activity", getStr(config, "url"), "/" + operation, drogon::Post,
          std::move(body), getStr(config, "token"), 10);
        if (!response)
            throw std::runtime_error("活动服务请求失败或超时，结果尚未确认");
        if ((*response)->statusCode() < 200 || (*response)->statusCode() >= 300)
            throw std::runtime_error("活动服务返回 HTTP " + std::to_string((*response)->statusCode()));
        auto result = json{};
        if ((*response)->body().size() > 1024 * 1024 || !tryParseJson((*response)->body(), result) ||
            !result.is_object())
            throw std::runtime_error("活动服务响应必须是最多 1 MiB 的 JSON 对象");
        co_return result;
    }

    auto ActivityManager::synchronize(std::string name) -> drogon::Task<> {
        std::shared_ptr<Service> service;
        json config;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || found->second->busy ||
                !getBool(found->second->config, "enabled"))
                co_return;
            service = found->second;
            service->busy = true;
            config = service->config;
        }
        auto error = std::string{};
        try {
            auto snapshot = co_await request(std::move(config), "status", json::object());
            std::lock_guard lock(m_mutex);
            if (!m_stopping) {
                const auto result = applySnapshotLocked(*service, snapshot, false);
                if (!result)
                    error = result.error();
            }
        } catch (const std::exception &e) {
            error = e.what();
        }
        std::lock_guard lock(m_mutex);
        service->busy = false;
        if (!error.empty()) {
            service->reachable = false;
            ++service->failures;
        } else
            service->failures = 0;
        const auto delay = error.empty() ? 30 : std::min<i64>(300, 5LL << std::min<size_t>(service->failures - 1, 6));
        service->nextSync = now() + delay;
        // 状态在后台可见；重复失联不逐轮刷日志。
        if (!error.empty() && service->failures == 1)
            Logger::warn(std::nullopt, "Activity", "活动服务失联或状态无效，将后台重试: " + name);
    }

    auto ActivityManager::invoke(std::string operation, std::string name, json arguments, json origin)
      -> drogon::Task<json> {
        std::shared_ptr<Service> service;
        json config;
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
            if (operation != "details" && (!service->synchronized || !service->reachable))
                co_return failure("活动服务状态尚未确认，请等待后台同步");
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
              {"caller", std::move(origin)}, {"arguments", std::move(arguments)}, {"callback_url", m_callbackUrl},
              {"name", name}};
            service->busy = true;
            config = service->config;
        }
        json result;
        auto error = std::string{};
        try {
            result = co_await request(std::move(config), operation, std::move(body));
            if (result.contains("snapshot")) {
                std::lock_guard lock(m_mutex);
                const auto applied = applySnapshotLocked(*service, result["snapshot"], false);
                if (!applied)
                    error = applied.error();
            }
        } catch (const std::exception &e) {
            error = e.what();
        }
        {
            std::lock_guard lock(m_mutex);
            service->busy = false;
            service->nextSync = 0;
            if (!error.empty())
                service->reachable = false;
        }
        // 无论响应是否完整，都确认真实状态；超时不会自动重发有副作用的请求。
        co_await synchronize(name);
        if (!error.empty())
            co_return failure(error + "；请查询活动状态，不要直接重复启动");
        co_return result;
    }

    auto ActivityManager::receiveEvent(const std::string &name, const std::string &token, const json &event)
      -> std::expected<void, std::string> {
        std::optional<Event> notification;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || !getBool(found->second->config, "enabled") ||
                !sameToken(getStr(found->second->config, "token"), token))
                return std::unexpected("活动认证失败");
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
            // 查询/工具响应可先同步状态；同序号的回调仍有一次通知机会。旧事件不反向覆盖新进展。
            if (sequence < runtime.value("sequence", i64{-1}) || sequence <= runtime.value("event_sequence", i64{-1}))
                return {};
            const auto previousEvent = runtime.value("event_sequence", i64{-1});
            runtime["event_sequence"] = sequence;
            if (auto saved = persistLocked(); !saved) {
                runtime["event_sequence"] = previousEvent;
                return std::unexpected(saved.error());
            }
            if (!text.empty())
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
        auto due = std::vector<std::string>{};
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            for (const auto &[name, service]: m_services)
                if (!service->busy && getBool(service->config, "enabled") && service->nextSync <= now())
                    due.push_back(name);
        }
        for (const auto &name: due)
            drogon::async_run([this, name]() -> drogon::Task<> { return synchronize(name); });
    }

    void ActivityManager::start(trantor::EventLoop &loop) {
        if (m_loop)
            return;
        m_loop = &loop;
        m_timer = loop.runEvery(5, [this] { tick(); });
        loop.queueInLoop([this] { tick(); });
    }

    void ActivityManager::stop() {
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
        }
        if (m_loop) {
            m_loop->invalidateTimer(m_timer);
            m_loop = nullptr;
        }
    }
} // namespace insoulforge
