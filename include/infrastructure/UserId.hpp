/// @file UserId.hpp
/// @brief 不依赖平台数字编码的用户身份。
#pragma once

#include <concepts>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <compare>
#include <expected>
#include <fmt/format.h>
#include <type_traits>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class UserId {
    public:
        explicit UserId(std::string_view value);

        template<std::integral T>
            requires(!std::same_as<T, bool>)
        explicit UserId(T value) : UserId(integerString(value)) {}

        [[nodiscard]] static auto parse(std::string_view value) -> std::expected<UserId, std::string>;
        [[nodiscard]] auto toString() const noexcept -> const std::string & { return m_value; }
        /// @brief 仅供数字协议和现有私聊会话编码使用；转换失败不会返回替代 ID。
        [[nodiscard]] auto toU64() const -> std::expected<u64, std::string>;
        auto operator<=>(const UserId &) const = default;

    private:
        template<std::integral T>
        static auto integerString(T value) -> std::string {
            if constexpr (std::is_signed_v<T>) {
                if (value < 0) {
                    throw std::invalid_argument("用户 ID 不能由负整数构造");
                }
            }
            return std::to_string(value);
        }

        std::string m_value;
    };
} // namespace insoulforge

template<>
struct std::hash<insoulforge::UserId> {
    auto operator()(const insoulforge::UserId &value) const noexcept -> size_t {
        return std::hash<std::string>{}(value.toString());
    }
};

template<>
struct fmt::formatter<insoulforge::UserId> : fmt::formatter<std::string_view> {
    auto format(const insoulforge::UserId &value, format_context &ctx) const {
        return fmt::formatter<std::string_view>::format(value.toString(), ctx);
    }
};
