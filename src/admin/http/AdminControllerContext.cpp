/// @file AdminControllerContext.cpp
/// @brief 上下文占用查询接口，仅返回统计及展示说明。
#include <admin/http/AdminController.hpp>
#include <admin/http/AdminResponse.hpp>
#include <workflow/agent/llm/prompts/ExecutorSystemPrompt.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>

namespace insoulforge {
    auto AdminController::getSystemTokens(drogon::HttpRequestPtr req,
      std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<> {
        const auto type = req->getParameter("type").empty() ? std::string{"group"} : req->getParameter("type");
        const auto id = req->getParameter("id");
        if (type != "group" && type != "private") {
            auto response = jsonResponse(AdminResponse::errorJson("无效的会话类型"));
            response->setStatusCode(drogon::k400BadRequest);
            callback(response);
            co_return;
        }
        try {
            const bool includeGroup = type == "group" && !id.empty();
            std::string warning;
            try {
                co_await ToolRuntime::fetchFavoriteEmojis();
            } catch (const std::exception &) {
                warning = "表情列表暂不可用，使用现有名称缓存；尚未加载时按空列表估算。";
            }
            const auto snapshot = includeGroup
                                    ? ExecutorSystemPrompt::instance().buildSnapshot(SessionId::group(id))
                                    : ExecutorSystemPrompt::instance().buildPublicSnapshot(type == "private");
            json result = {{"parts", json::array()}, {"total", 0}, {"warning", warning}};
            std::uint64_t total = 0;
            for (std::size_t i = 0; i < (includeGroup ? snapshot.parts.size() : snapshot.parts.size() - 1); ++i) {
                const auto &count = snapshot.parts[i].average;
                if (!count)
                    throw std::runtime_error(count.error());
                total += *count;
                result["parts"].push_back({{"id", std::string(ExecutorSystemPrompt::partInfo[i].id)},
                  {"name", std::string(ExecutorSystemPrompt::partInfo[i].name)}, {"tokens", *count},
                  {"description", std::string(ExecutorSystemPrompt::partInfo[i].description)}});
            }
            result["total"] = total;
            const auto tools = ToolRegistry::instance().getDefaultToolsTokenUsage(type == "private");
            if (!tools)
                throw std::runtime_error(tools.error());
            result["defaultTools"] = {{"count", tools->count}, {"tokens", tools->average}};
            result["combinedTotal"] = total + tools->average;
            callback(jsonResponse(result));
        } catch (const std::exception &error) {
            auto response = jsonResponse(AdminResponse::errorJson(error.what()));
            response->setStatusCode(drogon::k503ServiceUnavailable);
            callback(response);
        }
    }
} // namespace insoulforge
