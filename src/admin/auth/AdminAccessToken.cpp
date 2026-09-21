/// @file AdminAccessToken.cpp
/// @brief 管理后台启动令牌与会话认证实现

#include <admin/auth/AdminAccessToken.hpp>

#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <spdlog/spdlog.h>

#include <array>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace insoulforge {
    namespace {
        struct TokenState {
            std::string value;
            std::mutex mutex;
        };

        TokenState &state() {
            static TokenState instance;
            return instance;
        }

        std::string encodeHex(const std::array<unsigned char, 32> &bytes) {
            constexpr char hexadecimal[] = "0123456789abcdef";
            std::string value;
            value.reserve(bytes.size() * 2);
            for (const unsigned char byte: bytes) {
                value.push_back(hexadecimal[byte >> 4]);
                value.push_back(hexadecimal[byte & 0x0f]);
            }
            return value;
        }

        bool equalsToken(const std::string_view candidate, const std::string &expected) {
            return candidate.size() == expected.size() &&
                   CRYPTO_memcmp(candidate.data(), expected.data(), expected.size()) == 0;
        }
    } // namespace

    void AdminAccessToken::initialize() {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
            throw std::runtime_error("无法生成管理后台访问令牌");
        }

        auto &tokenState = state();
        std::scoped_lock lock(tokenState.mutex);
        tokenState.value = encodeHex(bytes);
        spdlog::info("管理后台访问令牌（重启后失效）: {}", tokenState.value);
    }

    bool AdminAccessToken::isAuthorized(const drogon::HttpRequestPtr &request) {
        return request && matches(request->getCookie(std::string(cookieName_)));
    }

    bool AdminAccessToken::matches(const std::string_view token) {
        auto &tokenState = state();
        std::scoped_lock lock(tokenState.mutex);
        return !tokenState.value.empty() && equalsToken(token, tokenState.value);
    }

    void AdminAccessToken::grantSession(const drogon::HttpResponsePtr &response) {
        drogon::Cookie cookie{std::string(cookieName_), state().value};
        cookie.setPath("/");
        cookie.setHttpOnly(true);
        cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
        response->addCookie(std::move(cookie));
    }

    void AdminAccessToken::revokeSession(const drogon::HttpResponsePtr &response) {
        drogon::Cookie cookie{std::string(cookieName_), ""};
        cookie.setPath("/");
        cookie.setHttpOnly(true);
        cookie.setSameSite(drogon::Cookie::SameSite::kStrict);
        cookie.setMaxAge(0);
        response->addCookie(std::move(cookie));
    }
} // namespace insoulforge
