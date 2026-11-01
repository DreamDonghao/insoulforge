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
    /// @brief 按原始字符串保存的用户身份，不要求 ID 为数字。
    /// @note 不裁剪空白、不改变大小写、不移除前导零；例如 "01" 与 "1" 是不同身份。
    /// 不提供默认构造或隐式类型转换；协议层的数字限制由调用方单独校验。
    class UserId {
    public:
        /// @brief 复制原始 ID，允许除空字符串和内嵌 NUL 外的内容。
        /// @throws std::invalid_argument ID 为空或包含 NUL。
        explicit UserId(std::string_view value);

        /// @brief 将非负整数转为十进制字符串后构造，不接受 bool。
        /// @throws std::invalid_argument 传入负整数。
        template<std::integral T>
            requires(!std::same_as<T, bool>)
        explicit UserId(T value) : UserId(integerString(value)) {}

        /// @brief 校验外部输入，身份规则与字符串构造函数相同。
        /// @return 有效 ID；输入为空或包含 NUL 时返回错误说明。
        [[nodiscard]] static auto parse(std::string_view value) -> std::expected<UserId, std::string>;

        /// @brief 返回未经规范化的原始 ID；引用有效期不超过当前对象且受移动、赋值影响。
        [[nodiscard]] auto toString() const noexcept -> const std::string & { return m_value; }

        /// @brief 为数字协议适配或历史数据迁移进行显式转换，不改变当前身份。
        /// @return 十进制数字对应的 u64；含符号、空白、非数字或数值溢出时返回错误说明。
        /// @note 允许前导零和数值 0；转换成功不代表满足具体接口的 ID 限制。
        [[nodiscard]] auto toU64() const -> std::expected<u64, std::string>;

        /// @brief 按原始字符串比较，而非按数值比较。
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
