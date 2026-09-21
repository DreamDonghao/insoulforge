/// @file AdminAccessToken.hpp
/// @brief 管理后台启动令牌与会话认证

#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <string_view>

namespace insoulforge {
    /// @brief 管理后台的进程内访问令牌。
    /// @details 每次程序启动生成新的高熵令牌。令牌仅输出至服务日志，不写入配置、数据库或前端；
    ///          登录成功后以 HttpOnly Cookie 维持当前浏览器会话。
    class AdminAccessToken {
    public:
        /// @brief 生成本次进程有效的访问令牌。
        /// @throws std::runtime_error 随机数生成失败时抛出。
        static void initialize();

        /// @brief 判断请求是否携带有效的管理后台会话 Cookie。
        [[nodiscard]] static bool isAuthorized(const drogon::HttpRequestPtr &request);

        /// @brief 校验用户输入的启动令牌。
        [[nodiscard]] static bool matches(std::string_view token);

        /// @brief 在登录响应中写入 HttpOnly 会话 Cookie。
        static void grantSession(const drogon::HttpResponsePtr &response);

        /// @brief 使当前浏览器的管理后台会话失效。
        static void revokeSession(const drogon::HttpResponsePtr &response);

    private:
        static constexpr std::string_view cookieName_ = "insoulforge_admin_session";
    };
} // namespace insoulforge
