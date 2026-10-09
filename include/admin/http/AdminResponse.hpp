/// @file AdminResponse.hpp
/// @brief 管理后台 REST API 的 JSON 响应构造助手
#pragma once
#include <string>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/config/ConfigError.hpp>

namespace insoulforge::AdminResponse {
    /// @brief 成功响应 {"success":true[, "message": ...]}
    inline auto okJson(const std::string &message = {}) -> json {
        json resp;
        resp["success"] = true;
        if (!message.empty()) {
            resp["message"] = message;
        }
        return resp;
    }

    /// @brief 错误响应 {"error": ...}
    inline auto errorJson(const std::string &message) -> json {
        json resp;
        resp["error"] = message;
        return resp;
    }

    /// @brief 失败响应 {"success":false, "error": ...}
    inline auto failJson(const std::string &message) -> json {
        json resp;
        resp["success"] = false;
        resp["error"] = message;
        return resp;
    }

    /// @brief 将配置保存错误转换为管理接口的失败响应。
    inline auto configErrorResponse(const ConfigError &error) -> drogon::HttpResponsePtr {
        auto response = jsonResponse(failJson(error.message));
        response->setStatusCode(
          error.type == ConfigErrorType::InvalidArgument ? drogon::k400BadRequest : drogon::k500InternalServerError);
        return response;
    }
} // namespace insoulforge::AdminResponse
