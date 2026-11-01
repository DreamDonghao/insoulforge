/// @file SessionId.hpp
/// @brief 会话类型与不透明目标身份；持久化时使用可逆的规范字符串。
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <compare>
#include <expected>

#include <infrastructure/UserId.hpp>

namespace insoulforge {
    /// @brief 由会话类型和原始目标 ID 共同确定的会话身份。
    /// @note 同一目标的群聊和私聊是不同会话。目标按字符串保存，保留大小写、空白和前导零。
    /// 不提供默认构造或数字转换；无会话状态应使用 std::optional<SessionId> 表达。
    class SessionId {
    public:
        /// @brief 会话类别，不通过目标 ID 的数值或最高位推断。
        enum class Type { Group, Private };

        /// @brief 从原始群 ID 构造，参数不带 group: 前缀，也不做百分号解码。
        /// @throws std::invalid_argument 目标为空或包含 NUL。
        [[nodiscard]] static auto group(std::string_view target) -> SessionId;

        /// @brief 从用户身份构造私聊会话，复制其原始字符串作为目标。
        [[nodiscard]] static auto privateChat(const UserId &user) -> SessionId;

        /// @brief 解析 toString() 生成的规范字符串，不接受旧版数字会话 ID。
        /// @return 有效会话；类型、目标或编码不合法时返回错误说明。
        /// @note 仅接受 group: / private: 前缀和大写十六进制百分号编码；
        /// 未编码的特殊字节、对非保留字符的多余转义以及空目标、NUL 均被拒绝。
        [[nodiscard]] static auto parse(std::string_view encoded) -> std::expected<SessionId, std::string>;
        [[nodiscard]] auto isPrivate() const noexcept -> bool { return m_type == Type::Private; }
        [[nodiscard]] auto isGroup() const noexcept -> bool { return m_type == Type::Group; }
        [[nodiscard]] auto type() const noexcept -> Type { return m_type; }

        /// @brief 返回稳定的序列化类型名 group 或 private。
        [[nodiscard]] auto typeName() const noexcept -> std::string_view { return isPrivate() ? "private" : "group"; }

        /// @brief 返回不带类型前缀、未编码的原始群 ID 或用户 ID，供协议适配层使用。
        /// @note 返回引用的有效期不超过当前对象且受移动、赋值影响。
        [[nodiscard]] auto targetId() const noexcept -> const std::string & { return m_target; }

        /// @brief 返回用于存储、日志和接口传输的规范字符串，例如 group:123 或 private:user%3Aabc。
        /// @note 目标中除 ASCII 字母、数字及 -._~ 外的字节均按 %HH 编码。
        /// 嵌入 URL 路径时仍需对整个字符串进行 URL 编码；返回引用的生命周期同 targetId()。
        [[nodiscard]] auto toString() const noexcept -> const std::string & { return m_encoded; }

        /// @brief 按类型和原始目标区分身份；排序不代表数字大小或业务优先级。
        auto operator<=>(const SessionId &) const = default;

    private:
        SessionId(Type type, std::string target);
        Type m_type;
        std::string m_target;
        std::string m_encoded; ///< 构造时缓存规范字符串，避免日志输出和哈希时重复编码。
    };
} // namespace insoulforge

template<>
struct std::hash<insoulforge::SessionId> {
    auto operator()(const insoulforge::SessionId &value) const noexcept -> size_t {
        return std::hash<std::string>{}(value.toString());
    }
};

template<>
struct fmt::formatter<insoulforge::SessionId> : fmt::formatter<std::string_view> {
    auto format(const insoulforge::SessionId &value, format_context &ctx) const {
        return fmt::formatter<std::string_view>::format(value.toString(), ctx);
    }
};
