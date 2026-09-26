/// @file AdminAccessToken.cpp
/// @brief 管理后台启动令牌与会话认证实现

#include <array>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

#include <arpa/inet.h>
#include <fmt/format.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <openssl/crypto.h>
#include <openssl/rand.h>

#include <admin/access/AdminAccessToken.hpp>
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    namespace {
        struct TokenState {
            std::string value;
            std::mutex mutex;
        };

        auto state() -> TokenState & {
            static TokenState instance;
            return instance;
        }

        auto encodeHex(const std::array<unsigned char, 32> &bytes) -> std::string {
            std::string value;
            value.reserve(bytes.size() * 2);
            for (const unsigned char byte: bytes) {
                constexpr char hexadecimal[] = "0123456789abcdef";
                value.push_back(hexadecimal[byte >> 4]);
                value.push_back(hexadecimal[byte & 0x0f]);
            }
            return value;
        }

        auto equalsToken(const std::string_view candidate, const std::string &expected) -> bool {
            return candidate.size() == expected.size() &&
                   CRYPTO_memcmp(candidate.data(), expected.data(), expected.size()) == 0;
        }

        auto localIpv4Addresses() -> std::vector<std::string> {
            std::vector<std::string> addresses{"127.0.0.1"};
            std::unordered_set<std::string> knownAddresses{addresses.begin(), addresses.end()};

            ifaddrs *interfaces = nullptr;
            if (getifaddrs(&interfaces) != 0) {
                return addresses;
            }

            for (const ifaddrs *interface = interfaces; interface; interface = interface->ifa_next) {
                if (!interface->ifa_addr || interface->ifa_addr->sa_family != AF_INET ||
                    !(interface->ifa_flags & IFF_UP) || (interface->ifa_flags & IFF_LOOPBACK)) {
                    continue;
                }

                char host[INET_ADDRSTRLEN]{};
                const auto *address = reinterpret_cast<const sockaddr_in *>(interface->ifa_addr);
                if (!inet_ntop(AF_INET, &address->sin_addr, host, sizeof(host))) {
                    continue;
                }
                if (knownAddresses.insert(host).second) {
                    addresses.emplace_back(host);
                }
            }
            freeifaddrs(interfaces);
            return addresses;
        }
    } // namespace

    void AdminAccessToken::initialize() {
        std::array<unsigned char, 32> bytes{};
        if (RAND_bytes(bytes.data(), static_cast<i32>(bytes.size())) != 1) {
            throw std::runtime_error("无法生成管理后台访问令牌");
        }

        auto &[value, mutex] = state();
        std::scoped_lock lock(mutex);
        value = encodeHex(bytes);
    }

    auto AdminAccessToken::loginUrls(const u16 port) -> std::vector<std::string> {
        std::vector<std::string> urls;
        for (const auto &address: localIpv4Addresses()) {
            urls.push_back(fmt::format("http://{}:{}/index.html#token={}", address, port, token()));
        }
        return urls;
    }

    auto AdminAccessToken::token() -> std::string {
        auto &[value, mutex] = state();
        std::scoped_lock lock(mutex);
        return value;
    }

    auto AdminAccessToken::isAuthorized(const drogon::HttpRequestPtr &request) -> bool {
        return request && matches(request->getCookie(std::string(cookieName_)));
    }

    auto AdminAccessToken::matches(const std::string_view token) -> bool {
        auto &[value, mutex] = state();
        std::scoped_lock lock(mutex);
        return !value.empty() && equalsToken(token, value);
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
