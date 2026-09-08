#include <onebot/http/ProcessQQMessages.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>

drogon::Task<> insoulforge::ProcessQQMessages::receiveOneBotEvent(
  const drogon::HttpRequestPtr req, const std::function<void(const drogon::HttpResponsePtr &)> callback) {
    auto body = parseJsonBody(req);
    if (!body) {
        spdlog::warn("OneBot 上报被拒绝：请求体不是 JSON 对象");
        const auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setBody("Invalid JSON or not an object");
        callback(resp);
        co_return;
    }
    json respJson;
    respJson["status"] = "ok";
    callback(jsonResponse(respJson));

    spdlog::debug(
      "OneBot 上报已确认: post_type={}, message_id={}", getStr(*body, "post_type"), getStr(*body, "message_id"));

    OneBotEventWorkflow::instance().enqueueOneBotEvent(std::move(*body));
    co_return;
}
