/// @file SessionId.cpp
/// @brief 会话标识的校验、百分号编码和解析。
#include <utility>

#include <infrastructure/SessionId.hpp>

namespace insoulforge {
    namespace {
        // RFC 3986 非保留 ASCII 字节；其他字节逐字节编码，不依赖字符集或 locale。
        constexpr auto unreserved(unsigned char c) -> bool {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == '.' || c == '~';
        }
        auto encode(std::string_view target) -> std::string {
            constexpr std::string_view hex = "0123456789ABCDEF";
            std::string result;
            for (const unsigned char c: target) {
                if (unreserved(c))
                    result += static_cast<char>(c);
                else {
                    result += '%';
                    result += hex[c >> 4];
                    result += hex[c & 15];
                }
            }
            return result;
        }
        constexpr auto hexValue(char c) -> int {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }
    } // namespace

    SessionId::SessionId(Type type, std::string target) : m_type(type), m_target(std::move(target)) {
        if (m_target.empty() || m_target.find('\0') != std::string::npos)
            throw std::invalid_argument("会话目标不能为空或包含空字符");
        m_encoded = std::string(typeName()) + ':' + encode(m_target);
    }

    auto SessionId::group(std::string_view target) -> SessionId { return {Type::Group, std::string(target)}; }
    auto SessionId::privateChat(const UserId &user) -> SessionId { return {Type::Private, user.toString()}; }

    auto SessionId::parse(std::string_view encoded) -> std::expected<SessionId, std::string> {
        const auto separator = encoded.find(':');
        const auto kind = encoded.substr(0, separator);
        if (separator == std::string_view::npos || (kind != "private" && kind != "group"))
            return std::unexpected("会话 ID 必须使用 group: 或 private: 前缀");
        std::string target;
        const auto value = encoded.substr(separator + 1);
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '%') {
                if (i + 2 >= value.size())
                    return std::unexpected("会话 ID 百分号编码不完整");
                const auto high = hexValue(value[i + 1]), low = hexValue(value[i + 2]);
                if (high < 0 || low < 0)
                    return std::unexpected("会话 ID 百分号编码无效");
                target += static_cast<char>((high << 4) | low);
                i += 2;
            } else {
                if (!unreserved(static_cast<unsigned char>(value[i])))
                    return std::unexpected("会话 ID 特殊字符必须编码");
                target += value[i];
            }
        }
        if (target.empty() || target.find('\0') != std::string::npos)
            return std::unexpected("会话目标不能为空或包含空字符");
        SessionId result(kind == "private" ? Type::Private : Type::Group, std::move(target));
        // 重新编码校验，确保同一会话只有一种表示，例如拒绝用 %41 表示 A。
        if (result.toString() != encoded)
            return std::unexpected("会话 ID 不是规范编码");
        return result;
    }
} // namespace insoulforge
