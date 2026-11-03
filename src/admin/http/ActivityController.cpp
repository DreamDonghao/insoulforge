/// @file ActivityController.cpp
/// @brief 管理请求由统一登录检查保护，活动通信使用独立的 WebSocket 入口。
#include <drogon/MultiPart.h>

#include <admin/events/ActivityWebSocket.hpp>
#include <admin/http/ActivityController.hpp>
#include <infrastructure/config/Config.hpp>
#include <workflow/activity/ActivityManager.hpp>

namespace insoulforge {
    namespace {
        auto reply(const std::expected<void, std::string> &result, const drogon::HttpStatusCode failureCode)
          -> drogon::HttpResponsePtr {
            auto response =
              jsonResponse(result ? json{{"success", true}} : json{{"success", false}, {"error", result.error()}});
            if (!result)
                response->setStatusCode(failureCode);
            response->addHeader("Cache-Control", "no-store");
            return response;
        }
    } // namespace
    void ActivityController::uploadFile(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        if (req->body().size() > 25U * 1024U * 1024U) {
            callback(reply(std::unexpected("上传请求过大"), drogon::k413RequestEntityTooLarge));
            return;
        }
        drogon::MultiPartParser parser;
        if (parser.parse(req) != 0 || parser.getFiles().size() != 1 || parser.getFiles()[0].getItemName() != "file" ||
            parser.getParameters().size() != 2 || !parser.getParameters().contains("name") ||
            !parser.getParameters().contains("run_id")) {
            callback(reply(std::unexpected("需要 multipart 字段 name、run_id 和一个 file"), drogon::k400BadRequest));
            return;
        }
        const auto bytes = parser.getFiles()[0].fileContent();
        if (bytes.size() > 24U * 1024U * 1024U) {
            callback(reply(std::unexpected("图片不得超过 24 MiB"), drogon::k413RequestEntityTooLarge));
            return;
        }
        const auto result = ActivityManager::instance().uploadFile(
          parser.getParameters().at("name"), parser.getParameters().at("run_id"), bytes);
        if (!result) {
            callback(reply(std::unexpected(result.error()), drogon::k400BadRequest));
            return;
        }
        auto response = jsonResponse(*result);
        response->addHeader("Cache-Control", "no-store");
        callback(response);
    }
    void ActivityController::getConfig(
      const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        auto response = jsonResponse(ActivityManager::instance().configuration());
        response->addHeader("Cache-Control", "no-store");
        callback(response);
    }
    void ActivityController::saveConfig(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        const auto body = parseJsonBody(req);
        const auto result = body && body->is_object() && body->size() == 1 && atOrNull(*body, "token").is_string()
                              ? ActivityManager::instance().saveToken(getStr(*body, "token"), Config::instance())
                              : std::expected<void, std::string>{std::unexpected("请求只能包含 token 字符串")};
        callback(reply(result, drogon::k400BadRequest));
    }
    void ActivityController::setEnabled(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        const auto body = parseJsonBody(req);
        const auto result =
          body && body->is_object() && body->size() == 2 && atOrNull(*body, "name").is_string() &&
              atOrNull(*body, "enabled").is_boolean()
            ? ActivityManager::instance().setEnabled(
                getStr(*body, "name"), getBool(*body, "enabled"), Config::instance())
            : std::expected<void, std::string>{std::unexpected("请求只能包含 name 字符串和 enabled 布尔值")};
        callback(reply(result, drogon::k400BadRequest));
    }
    void ActivityController::removeService(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        const auto body = parseJsonBody(req);
        const auto valid =
          body && body->is_object() && atOrNull(*body, "name").is_string() &&
          (body->contains("force") ? body->size() == 2 && (*body)["force"].is_boolean() : body->size() == 1);
        const auto result =
          valid ? ActivityManager::instance().removeService(
                    getStr(*body, "name"), Config::instance(), getBool(*body, "force"))
                : std::expected<void, std::string>{std::unexpected("请求需要 name 字符串和可选的 force 布尔值")};
        callback(reply(result, drogon::k400BadRequest));
    }
    void ActivityController::synchronize(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        const auto body = parseJsonBody(req);
        if (!body || !body->is_object() || body->size() != 1 || !atOrNull(*body, "name").is_string()) {
            callback(reply(std::unexpected("请求只能包含 name 字符串"), drogon::k400BadRequest));
            return;
        }
        drogon::async_run([name = getStr(*body, "name"), callback = std::move(callback)]() mutable -> drogon::Task<> {
            const auto result = co_await ActivityManager::instance().refresh(name);
            callback(reply(result, drogon::k400BadRequest));
        });
    }
} // namespace insoulforge
