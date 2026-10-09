/// @file AdminAccessToken.hpp
/// @brief 管理后台启动令牌与会话认证

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon_callbacks.h>
#include <expected>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 管理后台的进程内访问令牌。
    /// @details 每次程序启动生成新的随机令牌，启动日志会输出令牌及自动登录链接。
    ///          令牌不写入配置或数据库；登录成功后以 HttpOnly Cookie 维持浏览器会话。
    class AdminAccessToken {
    public:
        /// @brief 生成本次进程有效的访问令牌。
        /// @return 随机数生成失败时返回原因，不替换已有令牌。
        [[nodiscard]] static auto initialize() -> std::expected<void, std::string>;

        /// @brief 构建当前令牌可直接登录的管理后台链接。
        /// @details 令牌放在 URL 片段中，浏览器不会将其发送到服务端；仅枚举当前进程网络命名空间内
        ///          启用的 IPv4 地址。Docker 默认桥接网络无法枚举宿主机的局域网地址。
        /// @param port 管理后台监听端口。
        /// @return 每个可用本机 IPv4 地址对应的自动登录链接。
        [[nodiscard]] static auto loginUrls(u16 port) -> std::vector<std::string>;

        /// @brief 获取当前进程有效的管理后台访问令牌副本。
        /// @warning 调用方不得将令牌写入配置、数据库或不受信任的输出通道。
        [[nodiscard]] static auto token() -> std::string;

        /// @brief 判断请求是否携带有效的管理后台会话 Cookie。
        [[nodiscard]] static auto isAuthorized(const drogon::HttpRequestPtr &request) -> bool;

        /// @brief 管理后台的路由前鉴权回调，供 Drogon 注册。
        /// @details 放行非管理请求和公开登录接口；其他管理 API 与 WebSocket 请求须携带有效 Cookie。
        /// 未授权时返回 HTTP 401，不继续路由。
        static void checkRequestAccess(
          const drogon::HttpRequestPtr &request, drogon::AdviceCallback &&callback, drogon::AdviceChainCallback &&next);

        /// @brief 校验用户输入的启动令牌。
        [[nodiscard]] static auto matches(std::string_view token) -> bool;

        /// @brief 在登录响应中写入 HttpOnly 会话 Cookie。
        static void grantSession(const drogon::HttpResponsePtr &response);

        /// @brief 使当前浏览器的管理后台会话失效。
        static void revokeSession(const drogon::HttpResponsePtr &response);

    private:
        static constexpr std::string_view cookieName_ = "insoulforge_admin_session";
    };
} // namespace insoulforge
