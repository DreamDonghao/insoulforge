/// @file UserId.cpp
/// @brief 用户身份校验及显式数字转换。
#include <charconv>

#include <infrastructure/UserId.hpp>

namespace insoulforge {
    UserId::UserId(const std::string_view value) : m_value(value) {
        if (value.empty() || value.find('\0') != std::string_view::npos)
            throw std::invalid_argument("用户 ID 不能为空或包含空字符");
    }

    auto UserId::parse(const std::string_view value) -> std::expected<UserId, std::string> {
        if (value.empty() || value.find('\0') != std::string_view::npos)
            return std::unexpected("用户 ID 不能为空或包含空字符");
        return UserId{value};
    }

    auto UserId::toU64() const -> std::expected<u64, std::string> {
        u64 result{};
        const auto [end, error] = std::from_chars(m_value.data(), m_value.data() + m_value.size(), result);
        if (error != std::errc{} || end != m_value.data() + m_value.size())
            return std::unexpected("用户 ID 无法转换为 64 位无符号整数");
        return result;
    }
} // namespace insoulforge
