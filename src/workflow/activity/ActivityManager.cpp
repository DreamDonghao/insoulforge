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

#include <csetjmp>
#include <drogon/HttpAppFramework.h>
#include <drogon/utils/Utilities.h>
#include <jpeglib.h>
#include <openssl/crypto.h>
#include <png.h>

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
        // Decode bounded pixels rather than trusting a filename, MIME header or magic bytes alone.
        auto validUploadImage(std::string_view bytes, std::string_view mime) -> bool {
            constexpr size_t kMaxPixels = 16U * 1024U * 1024U;
            if (mime == "image/png") {
                auto image = png_image{};
                image.version = PNG_IMAGE_VERSION;
                if (!png_image_begin_read_from_memory(&image, bytes.data(), bytes.size())) {
                    png_image_free(&image);
                    return false;
                }
                image.format = PNG_FORMAT_RGBA;
                if (!image.width || !image.height || size_t(image.width) * image.height > kMaxPixels) {
                    png_image_free(&image);
                    return false;
                }
                auto pixels = std::vector<unsigned char>(PNG_IMAGE_SIZE(image));
                const auto valid = png_image_finish_read(&image, nullptr, pixels.data(), 0, nullptr) != 0;
                png_image_free(&image);
                return valid;
            }
            struct Decoder {
                jpeg_decompress_struct image{};
                jpeg_error_mgr error{};
                std::jmp_buf jump;
                bool warning = false;
                std::vector<unsigned char> row;
                ~Decoder() { jpeg_destroy_decompress(&image); }
            };
            auto decoder = std::make_unique<Decoder>();
            decoder->image.err = jpeg_std_error(&decoder->error);
            decoder->image.client_data = decoder.get();
            decoder->error.error_exit = [](j_common_ptr info) {
                auto *state = static_cast<Decoder *>(info->client_data);
                std::longjmp(state->jump, 1);
            };
            decoder->error.emit_message = [](j_common_ptr info, int level) {
                if (level < 0)
                    static_cast<Decoder *>(info->client_data)->warning = true;
            };
            if (setjmp(decoder->jump))
                return false;
            jpeg_create_decompress(&decoder->image);
            jpeg_mem_src(&decoder->image, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
            if (jpeg_read_header(&decoder->image, TRUE) != JPEG_HEADER_OK || !decoder->image.image_width ||
                !decoder->image.image_height ||
                size_t(decoder->image.image_width) * decoder->image.image_height > kMaxPixels)
                return false;
            jpeg_start_decompress(&decoder->image);
            decoder->row.resize(size_t(decoder->image.output_width) * decoder->image.output_components);
            while (decoder->image.output_scanline < decoder->image.output_height) {
                auto *row = decoder->row.data();
                if (jpeg_read_scanlines(&decoder->image, &row, 1) != 1)
                    return false;
            }
            return jpeg_finish_decompress(&decoder->image) && !decoder->warning;
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
        static auto manager = ActivityManager{
          "data/activity-state.json", [](Event event) {
              OneBotEventWorkflow::instance().enqueueActivityMessage(event.sessionId, std::move(event.text),
                std::move(event.name), std::move(event.mode), std::move(event.origin), std::move(event.attachments));
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
            m_uploads.clear();
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
                service->ready = service->synchronized = service->busy = false;
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
        service->ready = service->synchronized = service->busy = false;
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
            if (sequence == currentSequence)
                service.synchronized = true;
            if (!fromEvent) {
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
            service->ready = service->synchronized = service->busy = false;
            service->lastSeen = std::chrono::steady_clock::now();
        }
        if (previous) {
            disconnect(previous);
            previous->shutdown();
        }
        drogon::async_run([this, name, connection]() -> drogon::Task<> { co_await synchronize(name, connection); });
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
        }
        co_return co_await synchronize(name, std::move(connection));
    }

    auto ActivityManager::synchronize(std::string name, drogon::WebSocketConnectionPtr connection)
      -> drogon::Task<std::expected<void, std::string>> {
        std::shared_ptr<Service> service;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || found->second->connection != connection)
                co_return std::unexpected("活动连接已断开或更换");
            if (found->second->busy)
                co_return std::unexpected("活动正在处理请求，请稍后同步");
            service = found->second;
            service->busy = true;
        }
        bool sendReady = false;
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
            else if (!service->ready) {
                service->ready = true;
                sendReady = true;
            }
        }
        if (!error.empty()) {
            Logger::warn(std::nullopt, "Activity", "活动状态同步失败，等待有效推送或手动同步: " + name);
            co_return std::unexpected(error);
        } else if (sendReady)
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
                found->second->ready = found->second->synchronized = found->second->busy = false;
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
            if (operation != "details" &&
                (!result.contains("snapshot") || getStr(result["snapshot"], "run_id").empty() ||
                  !atOrNull(result["snapshot"], "sequence").is_number_integer() ||
                  !atOrNull(result["snapshot"], "status").is_string()))
                error = "成功的活动操作必须返回完整 snapshot";
            if (error.empty() && result.contains("snapshot")) {
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
            service->busy = false;
            if (!error.empty())
                service->synchronized = false;
        }
        if (!error.empty())
            co_return json{{"success", false}, {"confirmed", false},
              {"error", error + "；结果及状态未确认，等待服务推送或管理员立即同步，不要直接重复启动"}};
        co_return result;
    }

    void ActivityManager::expireUploadsLocked() {
        const auto current = std::chrono::steady_clock::now();
        std::erase_if(m_uploads, [&](const auto &entry) {
            const auto &[name, runId, id] = entry.first;
            const auto service = m_services.find(name);
            return entry.second.expires <= current || service == m_services.end() ||
                   !getBool(service->second->runtime, "running") || getStr(service->second->runtime, "run_id") != runId;
        });
    }

    auto ActivityManager::uploadFile(const std::string &name, const std::string &runId, std::string_view bytes)
      -> std::expected<json, std::string> {
        std::lock_guard lock(m_mutex);
        expireUploadsLocked();
        const auto found = m_services.find(name);
        if (m_stopping || found == m_services.end() || !found->second->connection ||
            !found->second->connection->connected() || !found->second->synchronized ||
            !getBool(found->second->runtime, "running") || getBool(found->second->runtime, "starting") ||
            runId.empty() || getStr(found->second->runtime, "run_id") != runId)
            return std::unexpected("服务未连接、未就绪或 run_id 已失效");
        const auto mime = Image::mimeType(bytes);
        if (bytes.empty() || bytes.size() > 24U * 1024U * 1024U)
            return std::unexpected("图片不能为空且不得超过 24 MiB");
        if (!mime || (*mime != "image/png" && *mime != "image/jpeg"))
            return std::unexpected("仅支持完整的 PNG 或 JPEG 图片");
        if (!validUploadImage(bytes, *mime))
            return std::unexpected("图片数据无效或像素超过 16 MiPixels");
        size_t total = 0;
        for (const auto &[key, upload]: m_uploads)
            total += upload.size;
        if (m_uploads.size() >= 128 || total + bytes.size() > 128U * 1024U * 1024U)
            return std::unexpected("活动上传缓存已满，请稍后重试");
        auto image = Image::fromBytes(bytes);
        if (!image)
            return std::unexpected("图片缓存失败");
        const auto id = image->id();
        m_uploads.insert_or_assign({name, runId, id},
          Upload{std::move(*image), bytes.size(), std::chrono::steady_clock::now() + std::chrono::minutes(10)});
        return json{{"success", true}, {"file_id", id}, {"media_type", *mime}};
    }

    auto ActivityManager::receiveEvent(const std::string &name, const drogon::WebSocketConnectionPtr &connection,
      const json &event) -> std::expected<void, std::string> {
        std::optional<Event> notification;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || !found->second->ready ||
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
            std::vector<Image> attachments;
            const auto &files = atOrNull(event, "attachments");
            if (event.contains("attachments") && (!files.is_array() || files.size() > 16))
                return std::unexpected("attachments 必须是最多 16 个文件的数组");
            const auto duplicate = getStr(event, "run_id") == getStr(service.runtime, "run_id") &&
                                   atOrNull(event, "sequence").is_number_integer() &&
                                   (event["sequence"] <= service.runtime.value("event_sequence", i64{-1}) ||
                                     event["sequence"] < service.runtime.value("sequence", i64{-1}));
            expireUploadsLocked();
            if (files.is_array()) {
                for (const auto &file: files) {
                    if (!file.is_object() || file.size() != 1 || !atOrNull(file, "file_id").is_string())
                        return std::unexpected("附件只能包含 file_id 字符串");
                    if (duplicate)
                        continue;
                    const auto upload = m_uploads.find({name, getStr(event, "run_id"), getStr(file, "file_id")});
                    if (upload == m_uploads.end())
                        return std::unexpected("附件不存在、已过期或不属于本活动运行");
                    attachments.push_back(upload->second.image);
                }
            }
            const auto hasContent = !text.empty() || !attachments.empty();
            const auto target = event.contains("target_session")
                                  ? getStr(event, "target_session")
                                  : getStr(atOrNull(service.runtime, "origin"), "session_id");
            const auto session = SessionId::parse(target);
            if (hasContent && !session)
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
            if (hasContent && getBool(service.config, "enabled"))
                notification = Event{*session, name, text, mode, std::move(attachments), runtime.at("origin")};
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
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping)
                return;
            expireUploadsLocked();
            const auto current = std::chrono::steady_clock::now();
            for (const auto &[name, service]: m_services) {
                if (!service->connection)
                    continue;
                if (current - service->lastSeen > std::chrono::seconds(45))
                    expired.push_back(service->connection);
            }
        }
        for (const auto &connection: expired) {
            disconnect(connection);
            connection->shutdown();
        }
    }

    void ActivityManager::start(trantor::EventLoop &loop) {
        std::lock_guard lock(m_mutex);
        if (m_loop)
            return;
        m_stopping = false;
        m_loop = &loop;
        m_timer = loop.runEvery(5.0, [this] { tick(); });
    }

    void ActivityManager::stop() {
        std::vector<drogon::WebSocketConnectionPtr> connections;
        {
            std::lock_guard lock(m_mutex);
            m_stopping = true;
            m_uploads.clear();
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
