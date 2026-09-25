#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/http/ProcessQQMessages.hpp>

auto insoulforge::ProcessQQMessages::receiveOneBotEvent(const drogon::HttpRequestPtr req,
  const std::function<void(const drogon::HttpResponsePtr &)> callback) -> drogon::Task<> {
    if (Config::instance().oneBotTransport != "http") {
        // OneBot 实现可能在切换期间继续投递 HTTP 上报。成功确认但不处理，避免其将 4xx 识别为快速操作失败。
        Logger::debug(0, "OneBot", fmt::format("忽略 OneBot HTTP 上报：当前传输方式为 WebSocket"));
        json response;
        response["status"] = "ok";
        callback(jsonResponse(response));
        co_return;
    }
    auto body = parseJsonBody(req);
    if (!body) {
        Logger::warn(0, "OneBot", fmt::format("OneBot 上报被拒绝：请求体不是 JSON 对象"));
        const auto resp = drogon::HttpResponse::newHttpResponse();
        resp->setStatusCode(drogon::k400BadRequest);
        resp->setBody("Invalid JSON or not an object");
        callback(resp);
        co_return;
    }
    json respJson;
    respJson["status"] = "ok";
    callback(jsonResponse(respJson));

    Logger::debug(0, "OneBot",
      fmt::format("上报已确认: post_type={}, message_id={}", getStr(*body, "post_type"), getStr(*body, "message_id")));

    OneBotEventWorkflow::instance().enqueueOneBotEvent(std::move(*body));
    co_return;
}
