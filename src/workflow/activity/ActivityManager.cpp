/// @file ActivityManager.cpp
/// @brief Activity 反向连接、状态缓存和串行请求转发；不管理业务生命周期。
#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <utility>
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
#include <workflow/EventWorkflow.hpp>
#include <workflow/activity/ActivityManager.hpp>

namespace insoulforge {
    namespace {
        auto now() -> i64 {
            return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
              .count();
        }
        auto sameToken(const std::string &expected, const std::string &actual) -> bool {
            return !expected.empty() && expected.size() == actual.size() &&
                   CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) == 0;
        }
        // Decode bounded pixels rather than trusting a filename, MIME header or magic bytes alone.
        auto validUploadImage(const std::string_view bytes, const std::string_view mime) -> bool {
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
                std::jmp_buf jump{};
                bool warning = false;
                std::vector<unsigned char> row;
                ~Decoder() { jpeg_destroy_decompress(&image); }
            };
            auto decoder = std::make_unique<Decoder>();
            decoder->image.err = jpeg_std_error(&decoder->error);
            decoder->image.client_data = decoder.get();
            decoder->error.error_exit = [](const j_common_ptr info) -> void {
                auto *state = static_cast<Decoder *>(info->client_data);
                std::longjmp(state->jump, 1);
            };
            decoder->error.emit_message = [](j_common_ptr info, int level) {
                if (level < 0) {
                    static_cast<Decoder *>(info->client_data)->warning = true;
                }
            };
            if (setjmp(decoder->jump)) {
                return false;
            }
            jpeg_create_decompress(&decoder->image);
            jpeg_mem_src(&decoder->image, reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
            if (jpeg_read_header(&decoder->image, TRUE) != JPEG_HEADER_OK || !decoder->image.image_width ||
                !decoder->image.image_height ||
                static_cast<size_t>(decoder->image.image_width) * decoder->image.image_height > kMaxPixels) {
                return false;
            }
            jpeg_start_decompress(&decoder->image);
            decoder->row.resize(static_cast<size_t>(decoder->image.output_width) * decoder->image.output_components);
            while (decoder->image.output_scanline < decoder->image.output_height) {
                auto *row = decoder->row.data();
                if (jpeg_read_scanlines(&decoder->image, &row, 1) != 1) {
                    return false;
                }
            }
            return jpeg_finish_decompress(&decoder->image) && !decoder->warning;
        }

        auto failure(const std::string &message) -> json { return {{"success", false}, {"error", message}}; }
        auto serviceDefinition(const json &value) -> json {
            return {{"name", getStr(value, "name")}, {"description", getStr(value, "description")},
              {"enabled", atOrNull(value, "enabled")}};
        }
        auto validState(const json &value) -> bool {
            return atOrNull(value, "active").is_boolean() && atOrNull(value, "status").is_string() &&
                   getStr(value, "status").size() <= 4096;
        }
    } // namespace
    ActivityManager::ActivityManager(EventSink sink) : m_sink(std::move(sink)), m_timer(0) {}
    auto ActivityManager::instance() -> ActivityManager & {
        static auto manager = ActivityManager{[](Event event) {
            EventWorkflow::instance().enqueueActivityMessage(
              event.sessionId, std::move(event.text), std::move(event.name), std::move(event.mode), event.attachments);
        }};
        return manager;
    }
    auto ActivityManager::validateConfiguration(const json &config) -> std::expected<void, std::string> {
        if (!config.is_object() || !atOrNull(config, "servers").is_array()) {
            return std::unexpected("活动配置需要 servers 数组");
        }
        if (const auto token = getStr(config, "token");
          (config.contains("token") && !config["token"].is_string()) ||
          (!token.empty() && (token.size() < 16 || token.find_first_of("\r\n") != std::string::npos))) {
            return std::unexpected("接入 Token 至少需要 16 个字符且不能包含换行");
        }
        auto names = std::set<std::string>{};
        for (const auto &server: config["servers"]) {
            if (const auto name = getStr(server, "name");
              !server.is_object() || name.empty() || name.size() > 80 ||
              std::ranges::any_of(name, [](const unsigned char c) -> bool { return c < 32; }) ||
              !names.insert(name).second) {
                return std::unexpected("活动名称必须非空、唯一且不含控制字符（最多 80 字节）");
            }
            if (!atOrNull(server, "description").is_string() || !atOrNull(server, "enabled").is_boolean() ||
                getStr(server, "description").size() > 4096) {
                return std::unexpected("活动需要描述和启用状态，描述最多 4096 字节");
            }
        }
        return {};
    }


    auto ActivityManager::initialize(json config) -> std::expected<void, std::string> {
        if (auto valid = validateConfiguration(config); !valid) {
            return valid;
        }
        std::lock_guard lock(m_mutex);
        if (!m_connections.empty()) {
            return std::unexpected("不能在连接期间重新初始化 Activity");
        }
        m_services.clear();
        m_uploads.clear();
        for (const auto &definition: config["servers"]) {
            auto service = std::make_shared<Service>();
            service->config = serviceDefinition(definition);
            m_services.emplace(getStr(definition, "name"), std::move(service));
        }
        m_token = getStr(config, "token");
        m_stopping = false;
        return {};
    }
    auto ActivityManager::configurationLocked() const -> json {
        auto servers = json::array();
        for (const auto &[name, service]: m_services) {
            servers.push_back(service->config);
        }
        return {{"token", m_token}, {"servers", std::move(servers)}};
    }
    auto ActivityManager::configuration() const -> json {
        std::lock_guard lock(m_mutex);
        auto result = configurationLocked();
        for (auto &item: result["servers"]) {
            const auto &service = *m_services.at(getStr(item, "name"));
            item["instructions"] = service.instructions;
            item["connected"] = service.connection && service.connection->connected();
            item["active"] = service.active;
            item["status"] = service.status;
            item["busy"] = !service.requests.empty();
            item["updatedAt"] = service.updatedAt;
        }
        return result;
    }
    auto ActivityManager::saveToken(std::string token, Config &store) -> std::expected<void, std::string> {
        if (!token.empty() && (token.size() < 16 || token.find_first_of("\r\n") != std::string::npos)) {
            return std::unexpected("接入 Token 至少需要 16 个字符且不能包含换行");
        }
        std::vector<drogon::WebSocketConnectionPtr> connections;
        {
            std::lock_guard lock(m_mutex);
            auto previous = std::exchange(m_token, std::move(token));
            if (auto saved = store.saveActivitiesConfig(configurationLocked()); !saved) {
                m_token = std::move(previous);
                return std::unexpected(saved.error().message);
            }
            if (previous == m_token) {
                return {};
            }
            ++m_authGeneration;
            for (const auto &[connection, registration]: m_connections) {
                connections.push_back(connection);
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
        if (found == m_services.end()) {
            return std::unexpected("Activity 不存在");
        }
        auto &definition = found->second->config;
        const auto previous = getBool(definition, "enabled");
        definition["enabled"] = enabled;
        if (auto saved = store.saveActivitiesConfig(configurationLocked()); !saved) {
            definition["enabled"] = previous;
            return std::unexpected(saved.error().message);
        }
        return {};
    }
    auto ActivityManager::removeService(const std::string &name, Config &store) -> std::expected<void, std::string> {
        std::unique_lock lock(m_mutex);
        const auto found = m_services.find(name);
        if (found == m_services.end()) {
            return std::unexpected("Activity 不存在");
        }
        const auto service = found->second;
        m_services.erase(found);
        if (auto saved = store.saveActivitiesConfig(configurationLocked()); !saved) {
            m_services.emplace(name, service);
            return std::unexpected(saved.error().message);
        }
        const auto connection = service->connection;
        auto requests = std::move(service->requests);
        if (connection) {
            const auto registration = m_connections.find(connection);
            if (registration != m_connections.end()) {
                registration->second.loop->invalidateTimer(registration->second.timer);
                m_connections.erase(registration);
            }
        }
        std::erase_if(m_uploads, [&](const auto &entry) { return entry.first.first == name; });
        lock.unlock();
        for (const auto &pending: requests) {
            finish(
              pending, name, failure(pending->sent ? "Activity 已移除，结果未确认" : "Activity 已移除，请求未执行"));
        }
        if (connection) {
            connection->shutdown();
        }
        return {};
    }
    auto ActivityManager::directory() const -> json {
        std::lock_guard lock(m_mutex);
        auto result = json::array();
        for (const auto &[name, service]: m_services) {
            if (getBool(service->config, "enabled") && service->connection && service->connection->connected()) {
                result.push_back({{"name", name}, {"description", getStr(service->config, "description")},
                  {"instructions", service->instructions}});
            }
        }
        return result;
    }
    auto ActivityManager::activeActivities() const -> json {
        std::lock_guard lock(m_mutex);
        auto result = json::array();
        for (const auto &[name, service]: m_services) {
            if (getBool(service->config, "enabled") && service->connection && service->connection->connected() &&
                service->active) {
                result.push_back({{"name", name}, {"status", service->status}});
            }
        }
        return result;
    }
    auto ActivityManager::authorized(const std::string &token) const -> bool {
        std::lock_guard lock(m_mutex);
        return !m_stopping && sameToken(m_token, token);
    }

    auto ActivityManager::connect(const std::string &token, drogon::WebSocketConnectionPtr connection) -> bool {
        std::lock_guard lock(m_mutex);
        if (m_stopping || !sameToken(m_token, token)) {
            return false;
        }
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
        m_connections.emplace(connection, Registration{"", {}, m_authGeneration, loop, timer});
        return true;
    }


    auto ActivityManager::registerService(const drogon::WebSocketConnectionPtr &connection, const json &message)
      -> std::expected<void, std::string> {
        const auto name = getStr(message, "name");
        auto definition = json{{"name", name}, {"description", atOrNull(message, "description")}, {"enabled", true}};
        if (auto valid = validateConfiguration({{"servers", json::array({definition})}}); !valid) {
            return valid;
        }
        if (message.size() != 6 || !validState(message) || !atOrNull(message, "instructions").is_string() ||
            getStr(message, "instructions").size() > 65536) {
            return std::unexpected("注册需要 active、status 和 instructions");
        }
        drogon::WebSocketConnectionPtr previous;
        {
            std::lock_guard lock(m_mutex);
            const auto registration = m_connections.find(connection);
            if (m_stopping || registration == m_connections.end() || !registration->second.name.empty() ||
                registration->second.generation != m_authGeneration) {
                return std::unexpected("Activity 连接无法注册");
            }
            const auto found = m_services.find(name);
            const auto isNew = found == m_services.end();
            auto service = std::make_shared<Service>();
            if (!isNew) {
                definition["enabled"] = getBool(found->second->config, "enabled");
                previous = found->second->connection;
            }
            service->config = definition;
            auto oldService = isNew ? nullptr : found->second;
            m_services[name] = service;
            if (auto saved = Config::instance().saveActivitiesConfig(configurationLocked()); !saved) {
                if (isNew) {
                    m_services.erase(name);
                } else {
                    m_services[name] = oldService;
                }
                return std::unexpected(saved.error().message);
            }
            registration->second.name = name;
            registration->second.service = service;
            registration->second.loop->invalidateTimer(registration->second.timer);
            service->connection = connection;
            service->instructions = getStr(message, "instructions");
            service->active = getBool(message, "active");
            service->status = getStr(message, "status");
            service->updatedAt = now();
            service->lastSeen = std::chrono::steady_clock::now();
            connection->send(dumpJson(json{{"type", "ready"}}));
        }
        if (previous) {
            disconnect(previous);
            previous->shutdown();
        }
        return {};
    }
    ActivityManager::ResponseAwaiter::ResponseAwaiter(
      ActivityManager &manager, std::string name, drogon::WebSocketConnectionPtr connection, json parameters) :
        m_manager(manager), m_name(std::move(name)), m_connection(std::move(connection)),
        m_parameters(std::move(parameters)) {}
    auto ActivityManager::ResponseAwaiter::await_suspend(std::coroutine_handle<> continuation) -> bool {
        auto *loop = trantor::EventLoop::getEventLoopOfCurrentThread();
        if (!loop) {
            loop = drogon::app().getLoop();
        }
        loop->queueInLoop([this, loop, continuation] -> void {
            m_manager.enqueueRequest(std::move(m_name), std::move(m_connection), std::move(m_parameters), loop,
              [this, continuation](json value) -> void {
                  setValue(std::move(value));
                  continuation.resume();
              });
        });
        return true;
    }
    auto ActivityManager::invoke(std::string name, json parameters) -> drogon::Task<json> {
        drogon::WebSocketConnectionPtr connection;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (m_stopping || found == m_services.end() || !getBool(found->second->config, "enabled") ||
                !found->second->connection || !found->second->connection->connected()) {
                co_return failure("Activity 未启用或未连接，请求未执行");
            }
            if (!parameters.is_object()) {
                co_return failure("parameters 必须是对象");
            }
            connection = found->second->connection;
        }
        co_return co_await ResponseAwaiter(*this, std::move(name), std::move(connection), std::move(parameters));
    }
    void ActivityManager::enqueueRequest(std::string name, drogon::WebSocketConnectionPtr connection, json parameters,
      trantor::EventLoop *loop, std::function<void(json)> callback) {
        std::lock_guard lock(m_mutex);
        const auto found = m_services.find(name);
        if (m_stopping || found == m_services.end() || found->second->connection != connection ||
            !connection->connected() || !getBool(found->second->config, "enabled")) {
            loop->queueInLoop(
              [callback = std::move(callback)] -> void { callback(failure("Activity 不可用，请求未执行")); });
            return;
        }
        const auto pending = std::make_shared<Pending>();
        pending->loop = loop;
        pending->callback = std::move(callback);
        pending->message = {{"type", "request"}, {"parameters", std::move(parameters)}};
        found->second->requests.push_back(pending);
        if (found->second->requests.size() == 1) {
            dispatchLocked(name, *found->second);
        }
    }
    void ActivityManager::dispatchLocked(const std::string &name, Service &service) {
        const auto pending = service.requests.front();
        const auto connection = service.connection;
        pending->loop->queueInLoop([this, name, pending, connection] -> void {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (found == m_services.end() || found->second->connection != connection ||
                found->second->requests.empty() || found->second->requests.front() != pending) {
                return;
            }
            if (!getBool(found->second->config, "enabled")) {
                pending->loop->queueInLoop([this, name, connection] -> void {
                    completeRequest(name, connection, failure("Activity 已禁用，请求未执行"));
                });
                return;
            }
            pending->sent = true;
            pending->timer = pending->loop->runAfter(10, [this, pending, connection] -> void {
                if (disconnectCurrent(connection, pending)) {
                    connection->shutdown();
                }
            });
            try {
                connection->send(dumpJson(pending->message));
            } catch (const std::exception &) {
                pending->loop->queueInLoop([this, connection] -> void {
                    disconnect(connection);
                    connection->shutdown();
                });
            }
        });
    }
    void ActivityManager::finish(const std::shared_ptr<Pending> &pending, const std::string &name, json response) {
        if (pending->sent) {
            pending->loop->invalidateTimer(pending->timer);
        }
        HttpTrace::instance().append({.tag = "Activity",
          .method = "WS",
          .url = "/activities/ws?name=" + drogon::utils::urlEncodeComponent(name) + "#request",
          .requestBody = dumpJson(pending->message),
          .status = getBool(response, "success") ? 200 : 0,
          .responseBody = dumpJson(response)});
        pending->loop->queueInLoop(
          [pending, response = std::move(response)]() mutable -> void { pending->callback(std::move(response)); });
    }
    void ActivityManager::completeRequest(
      const std::string &name, const drogon::WebSocketConnectionPtr &connection, json response) {
        std::shared_ptr<Pending> pending;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (found == m_services.end() || found->second->connection != connection ||
                found->second->requests.empty()) {
                return;
            }
            pending = found->second->requests.front();
            found->second->requests.pop_front();
            if (!found->second->requests.empty()) {
                dispatchLocked(name, *found->second);
            }
        }
        finish(pending, name, std::move(response));
    }
    void ActivityManager::disconnect(const drogon::WebSocketConnectionPtr &connection) {
        disconnectCurrent(connection, {});
    }
    auto ActivityManager::disconnectCurrent(
      const drogon::WebSocketConnectionPtr &connection, const std::shared_ptr<Pending> &expected) -> bool {
        std::deque<std::shared_ptr<Pending>> requests;
        std::string name;
        {
            std::lock_guard lock(m_mutex);
            const auto registration = m_connections.find(connection);
            if (registration == m_connections.end()) {
                return false;
            }
            name = registration->second.name;
            const auto service = registration->second.service;
            if (expected && (!service || service->requests.empty() || service->requests.front() != expected)) {
                return false;
            }
            registration->second.loop->invalidateTimer(registration->second.timer);
            m_connections.erase(registration);
            if (service && service->connection == connection) {
                requests.swap(service->requests);
                service->connection.reset();
                service->active = false;
                service->status.clear();
            }
        }
        for (const auto &pending: requests) {
            finish(pending, name,
              failure(
                pending->sent ? "Activity 连接断开或请求超时，结果未确认" : "Activity 连接已更换或断开，请求未执行"));
        }
        return true;
    }

    auto ActivityManager::attachmentsLocked(const std::string &name, const json &message)
      -> std::expected<std::vector<Image>, std::string> {
        std::vector<Image> images;
        if (!message.contains("attachments")) {
            return images;
        }
        const auto &files = message["attachments"];
        if (!files.is_array() || files.size() > 16) {
            return std::unexpected("附件最多 16 项");
        }
        expireUploadsLocked();
        for (const auto &file: files) {
            if (!file.is_object() || file.size() != 1 || !atOrNull(file, "file_id").is_string()) {
                return std::unexpected("附件需要 file_id");
            }
            const auto found = m_uploads.find({name, getStr(file, "file_id")});
            if (found == m_uploads.end()) {
                return std::unexpected("附件不存在、已过期或不属于本服务");
            }
            images.push_back(found->second.image);
        }
        return images;
    }
    void ActivityManager::receive(
      const drogon::WebSocketConnectionPtr &connection, const std::string &message, drogon::WebSocketMessageType type) {
        std::string name;
        {
            std::lock_guard lock(m_mutex);
            const auto registration = m_connections.find(connection);
            if (m_stopping || registration == m_connections.end() ||
                registration->second.generation != m_authGeneration) {
                return;
            }
            name = registration->second.name;
            if (!name.empty()) {
                const auto found = m_services.find(name);
                if (found == m_services.end() || found->second->connection != connection) {
                    return;
                }
                found->second->lastSeen = std::chrono::steady_clock::now();
            }
        }
        if (type == drogon::WebSocketMessageType::Ping || type == drogon::WebSocketMessageType::Pong ||
            type == drogon::WebSocketMessageType::Close) {
            return;
        }
        const auto reject = [&] -> void {
            disconnect(connection);
            connection->shutdown(drogon::CloseCode::kViolation, "Invalid Activity message");
        };
        json value;
        if (type != drogon::WebSocketMessageType::Text || message.size() > 1024 * 1024 ||
            !tryParseJson(message, value) || !value.is_object()) {
            reject();
            return;
        }
        if (name.empty()) {
            if (getStr(value, "type") != "register" || message.size() > 128 * 1024 ||
                !registerService(connection, value)) {
                reject();
            }
            return;
        }
        const auto kind = getStr(value, "type");
        if (kind == "response" && std::ranges::any_of(value.items(), [](const auto &item) -> auto {
                return item.key() != "type" && item.key() != "success" && item.key() != "result" &&
                       item.key() != "error" && item.key() != "attachments";
            })) {
            reject();
            return;
        }

        bool valid = false;
        std::optional<Event> notification;
        {
            std::lock_guard lock(m_mutex);
            const auto found = m_services.find(name);
            if (found == m_services.end() || found->second->connection != connection) {
                return;
            }
            auto &service = *found->second;
            if (kind == "status" && value.size() == 3 && message.size() <= 128 * 1024 && validState(value)) {
                service.active = getBool(value, "active");
                service.status = getStr(value, "status");
                service.updatedAt = now();
                valid = true;
            } else if (kind == "response" && !service.requests.empty() && service.requests.front()->sent &&
                       atOrNull(value, "success").is_boolean()) {
                valid = getBool(value, "success") ? atOrNull(value, "result").is_object()
                                                  : atOrNull(value, "error").is_string();
                if (valid) {
                    auto images = attachmentsLocked(name, value);
                    valid = images.has_value();
                    if (valid && value.contains("attachments")) {
                        value["attachments"] = json::array();
                        for (const auto &image: *images) {
                            value["attachments"].push_back({{"image_id", image.id()}});
                        }
                    }
                }
            } else if (kind == "notify" && message.size() <= 128 * 1024) {
                const auto session = SessionId::parse(getStr(value, "target_session"));
                const auto mode = getStr(value, "mode", "silent");
                const auto text = getStr(value, "text");
                auto images = attachmentsLocked(name, value);
                valid = session.has_value() && (!value.contains("mode") || atOrNull(value, "mode").is_string()) &&
                        (mode == "silent" || mode == "judge" || mode == "force") &&
                        (!value.contains("text") || atOrNull(value, "text").is_string()) && text.size() <= 65536 &&
                        images.has_value() && (!text.empty() || !images->empty());
                if (valid && getBool(service.config, "enabled")) {
                    notification = Event{.sessionId = *session,
                      .name = name,
                      .text = text,
                      .mode = mode,
                      .attachments = std::move(*images)};
                }
            }
        }
        if (!valid) {
            reject();
            return;
        }
        if (kind == "response") {
            completeRequest(name, connection, std::move(value));
        } else {
            HttpTrace::instance().append({.tag = "Activity",
              .method = "WS",
              .url = "/activities/ws?name=" + drogon::utils::urlEncodeComponent(name) + "#" + kind,
              .requestBody = message,
              .status = 200});
            if (notification && m_sink) {
                try {
                    m_sink(std::move(*notification));
                } catch (const std::exception &error) {
                    Logger::warn(std::nullopt, "Activity", error.what());
                }
            }
        }
    }
    void ActivityManager::expireUploadsLocked() {
        const auto current = std::chrono::steady_clock::now();
        std::erase_if(m_uploads, [&](const auto &entry) -> auto {
            return entry.second.expires <= current || !m_services.contains(entry.first.first);
        });
    }
    auto ActivityManager::uploadFile(const std::string &name, std::string_view bytes)
      -> std::expected<json, std::string> {
        std::lock_guard lock(m_mutex);
        expireUploadsLocked();
        if (const auto found = m_services.find(name); m_stopping || found == m_services.end() ||
                                                      !found->second->connection ||
                                                      !found->second->connection->connected()) {
            return std::unexpected("Activity 未连接或未注册");
        }
        const auto mime = Image::mimeType(bytes);
        if (bytes.empty() || bytes.size() > 24U * 1024U * 1024U) {
            return std::unexpected("图片不能为空且不得超过 24 MiB");
        }
        if (!mime || (*mime != "image/png" && *mime != "image/jpeg")) {
            return std::unexpected("仅支持完整的 PNG 或 JPEG 图片");
        }
        if (!validUploadImage(bytes, *mime)) {
            return std::unexpected("图片数据无效或像素超过 16 MiPixels");
        }
        auto image = Image::fromBytes(bytes);
        if (!image) {
            return std::unexpected("图片缓存失败");
        }
        const auto id = image->id();
        const auto existing = m_uploads.find({name, id});
        size_t total = 0;
        for (const auto &upload: m_uploads | std::views::values) {
            total += upload.size;
        }
        if (const auto previousSize = existing == m_uploads.end() ? 0 : existing->second.size;
          (existing == m_uploads.end() && m_uploads.size() >= 128) ||
          total - previousSize + bytes.size() > 128U * 1024U * 1024U) {
            return std::unexpected("Activity 上传缓存已满，请稍后重试");
        }
        m_uploads.insert_or_assign(
          {name, id}, Upload{.image = std::move(*image),
                        .size = bytes.size(),
                        .expires = std::chrono::steady_clock::now() + std::chrono::minutes(10)});
        return json{{"success", true}, {"file_id", id}, {"media_type", *mime}};
    }

    void ActivityManager::tick() {
        std::vector<drogon::WebSocketConnectionPtr> expired;
        {
            std::lock_guard lock(m_mutex);
            if (m_stopping) {
                return;
            }
            expireUploadsLocked();
            const auto current = std::chrono::steady_clock::now();
            for (const auto &service: m_services | std::views::values) {
                if (!service->connection) {
                    continue;
                }
                if (current - service->lastSeen > std::chrono::seconds(45)) {
                    expired.push_back(service->connection);
                }
            }
        }
        for (const auto &connection: expired) {
            disconnect(connection);
            connection->shutdown();
        }
    }

    void ActivityManager::start(trantor::EventLoop &loop) {
        std::lock_guard lock(m_mutex);
        if (m_loop) {
            return;
        }
        m_stopping = false;
        m_loop = &loop;
        m_timer = loop.runEvery(5.0, [this] -> void { tick(); });
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
            for (const auto &connection: m_connections | std::views::keys) {
                connections.push_back(connection);
            }
        }
        for (const auto &connection: connections) {
            disconnect(connection);
            connection->shutdown();
        }
    }
} // namespace insoulforge
