/// @file SessionId.hpp
/// @brief 群聊与私聊的统一会话标识，用于会话状态索引、消息归属和持久化。
#pragma once

#include <functional>
#include <string>
#include <string_view>

#include <compare>
#include <expected>

#include <infrastructure/UserId.hpp>

namespace insoulforge {
    /// @brief 由会话类型和原始目标 ID 共同确定的会话身份。
    /// @details 群聊以群 ID 为目标，私聊以对方用户 ID 为目标；工作流据此管理各会话的状态，
    /// 消息发送层据此选择群消息或私聊消息接口。构造标识不会创建会话记录或启用会话。
    /// @note 同一目标的群聊和私聊是不同会话。目标按字符串保存，保留大小写、空白和前导零。
    /// 不提供默认构造或数字转换；无会话状态应使用 std::optional<SessionId> 表达。
    class SessionId {
    public:
        /// @brief 会话类别，不通过目标 ID 的数值或最高位推断。
        enum class Type {
            Group, ///< 以原始群 ID 为目标的群聊会话。
            Private ///< 以原始用户 ID 为目标的私聊会话。
        };

        /// @brief 根据原始群 ID 构造群聊会话标识。
        /// @param target 群 ID 字符串，不带 group: 前缀；按原样复制，不做百分号解码。
        /// @throws std::invalid_argument 目标为空或包含 NUL。
        /// @note 不检查群是否存在，也不校验 OneBot 所需的非零数字格式；数字限制由协议层检查。
        [[nodiscard]] static auto group(std::string_view target) -> SessionId;

        /// @brief 根据私聊对方的用户身份构造会话标识，复制其原始用户 ID 作为目标。
        [[nodiscard]] static auto privateChat(const UserId &user) -> SessionId;

        /// @brief 将数据库或接口中的规范会话字符串还原为会话标识。
        /// @return 有效会话；类型、目标或编码不合法时返回错误说明。
        /// @note 输入格式与 toString() 一致，不接受旧版数字会话 ID，也不查询会话是否存在或启用。
        /// 仅接受 group: / private: 前缀和大写十六进制百分号编码；
        /// 未编码的特殊字节、对非保留字符的多余转义以及空目标、NUL 均被拒绝。
        [[nodiscard]] static auto parse(std::string_view encoded) -> std::expected<SessionId, std::string>;

        /// @brief 判断是否为私聊会话，供消息发送和工作流选择私聊处理分支。
        [[nodiscard]] auto isPrivate() const noexcept -> bool { return m_type == Type::Private; }

        /// @brief 判断是否为群聊会话；群消息协议接口据此拒绝私聊目标。
        [[nodiscard]] auto isGroup() const noexcept -> bool { return m_type == Type::Group; }

        /// @brief 返回构造时确定的会话类别，与目标 ID 的内容无关。
        [[nodiscard]] auto type() const noexcept -> Type { return m_type; }

        /// @brief 返回 group 或 private，用于规范字符串前缀和后台接口的 sessionType 字段。
        [[nodiscard]] auto typeName() const noexcept -> std::string_view { return isPrivate() ? "private" : "group"; }

        /// @brief 返回原始群 ID 或私聊对方的用户 ID，供协议接口寻址及上下文中的目标信息使用。
        /// @note 不含会话类型前缀和百分号编码，不能替代 toString() 作为跨群聊、私聊的会话键。
        /// @note 返回引用的有效期不超过当前对象且受移动、赋值影响。
        [[nodiscard]] auto targetId() const noexcept -> const std::string & { return m_target; }

        /// @brief 返回用于存储、日志和接口传输的规范字符串，例如 group:123 或 private:user%3Aabc。
        /// @note 目标中除 ASCII 字母、数字及 -._~ 外的字节均按 %HH 编码。
        /// 嵌入 URL 路径时仍需对整个字符串进行 URL 编码；返回引用的生命周期同 targetId()。
        [[nodiscard]] auto toString() const noexcept -> const std::string & { return m_encoded; }

        /// @brief 按类型和原始目标区分身份；排序不代表数字大小或业务优先级。
        auto operator<=>(const SessionId &) const = default;

    private:
        /// @brief 接管原始目标字符串，校验后生成并缓存带类型前缀的规范字符串。
        /// @throws std::invalid_argument 目标为空或包含 NUL。
        SessionId(Type type, std::string target);
        Type m_type;
        std::string m_target;
        std::string m_encoded; ///< 构造时缓存规范字符串，避免日志输出和哈希时重复编码。
    };
} // namespace insoulforge

/// @brief 支持以会话标识为键的哈希容器，供工作流索引会话状态。
template<>
struct std::hash<insoulforge::SessionId> {
    /// @brief 使用缓存的规范字符串计算哈希，无需重新编码目标 ID。
    auto operator()(const insoulforge::SessionId &value) const noexcept -> size_t {
        return std::hash<std::string>{}(value.toString());
    }
};

/// @brief 使用规范字符串格式化会话身份，支持字符串格式说明符。
template<>
struct fmt::formatter<insoulforge::SessionId> : fmt::formatter<std::string_view> {
    /// @brief 将缓存的规范字符串写入格式化上下文。
    auto format(const insoulforge::SessionId &value, format_context &ctx) const {
        return fmt::formatter<std::string_view>::format(value.toString(), ctx);
    }
};
