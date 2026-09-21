/// @file AdminAccessToken.hpp
/// @brief 管理后台启动令牌与会话认证

#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace insoulforge {
    /// @brief 管理后台的进程内访问令牌。
    /// @details 每次程序启动生成新的高熵令牌。令牌仅输出至服务日志中的自动登录链接，不写入配置、
    ///          数据库或前端；登录成功后以 HttpOnly Cookie 维持当前浏览器会话。
    class AdminAccessToken {
    public:
        /// @brief 生成本次进程有效的访问令牌。
        /// @throws std::runtime_error 随机数生成失败时抛出。
        static void initialize();

        /// @brief 构建当前令牌可直接登录的管理后台链接。
        /// @details 令牌放在 URL 片段中，浏览器不会将其发送到服务端；仅枚举当前进程网络命名空间内
        ///          启用的 IPv4 地址。Docker 默认桥接网络无法枚举宿主机的局域网地址。
        /// @param port 管理后台监听端口。
        /// @return 每个可用本机 IPv4 地址对应的自动登录链接。
        [[nodiscard]] static std::vector<std::string> loginUrls(uint16_t port);

        /// @brief 获取当前进程有效的管理后台访问令牌副本。
        /// @warning 调用方不得将令牌写入配置、数据库或不受信任的输出通道。
        [[nodiscard]] static std::string token();

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
