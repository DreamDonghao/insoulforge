/// @file ActivityController.cpp
/// @brief 管理请求由统一登录检查保护，服务回调由管理器验证独立密钥。
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
    void ActivityController::getConfig(
      const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        auto response = jsonResponse(ActivityManager::instance().configuration());
        response->addHeader("Cache-Control", "no-store");
        callback(response);
    }
    void ActivityController::saveConfig(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        const auto body = parseJsonBody(req);
        const auto result = body ? ActivityManager::instance().saveConfiguration(*body, Config::instance())
                                 : std::expected<void, std::string>{std::unexpected("无效的 JSON 配置")};
        callback(reply(result, drogon::k400BadRequest));
    }
    void ActivityController::event(
      const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const {
        if (req->body().size() > 128 * 1024) {
            callback(reply(std::unexpected("活动事件过大"), drogon::k413RequestEntityTooLarge));
            return;
        }
        const auto body = parseJsonBody(req);
        const auto authorization = req->getHeader("authorization");
        if (!body || !body->is_object() || !authorization.starts_with("Bearer ")) {
            callback(reply(std::unexpected("无效的活动事件或认证"), drogon::k401Unauthorized));
            return;
        }
        const auto result =
          ActivityManager::instance().receiveEvent(getStr(*body, "name"), authorization.substr(7), *body);
        const auto unauthorized = !result && result.error() == "活动认证失败";
        callback(reply(result, unauthorized ? drogon::k401Unauthorized : drogon::k400BadRequest));
    }
} // namespace insoulforge
