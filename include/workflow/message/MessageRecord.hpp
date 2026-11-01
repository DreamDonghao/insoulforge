/// @file MessageRecord.hpp
/// @brief 聊天记录富内容的构造与投影
/// @details 持久化记录以 `segments` 保存唯一的内容顺序，图片传输来源仅存于 `assets.images`。
///          历史格式在数据库升级时转换；运行时仅处理当前结构，避免重复暴露图片 URL。

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/UserId.hpp>

namespace insoulforge::MessageRecord {
    /// @brief 图片的 OneBot 获取信息
    /// @details 仅供服务端工具使用，绝不能直接投影到 LLM 上下文。
    struct ImageSource {
        std::string file; ///< OneBot 图片缓存文件名
        std::string url; ///< OneBot 图片下载地址
    };

    [[nodiscard]] auto getSessionId(const json &record) -> SessionId;

    /// @brief 将机器人尝试发送的 CQ 内容转换为统一聊天记录
    /// @param senderName 机器人的显示名称
    /// @param messageId OneBot 返回的消息 ID；未确认发送成功时为空，不写入该字段。
    /// @param content 尝试发送的 CQ 内容
    /// @return 不含重复 text 字段的富内容记录
    /// @details 收藏表情仅记录 `sticker.name`；普通 CQ 图片只保留图片动作，不保存外部 URL。
    [[nodiscard]] auto createAssistantRecord(
      std::string senderName, std::optional<u64> messageId, const std::string &content) -> json;

    /// @brief 创建生成图片记录，只保存图片描述，不保存传输用 URL/base64；失败记录不带消息 ID。
    [[nodiscard]] auto createAssistantGeneratedImageRecord(
      std::string senderName, std::optional<u64> messageId, const std::string &description) -> json;

    /// @brief 构造内部执行记录，不生成 QQ 消息 ID，也不表示向用户发送了消息。
    [[nodiscard]] auto createAssistantExecutionRecord(
      std::string senderName, json toolHistory, std::string status, std::string reason = {}) -> json;

    /// @brief 保存单次工具名称、脱敏且限长的参数和处理状态，不复制完整工具结果。
    [[nodiscard]] auto createToolHistoryEntry(std::string name, const json &arguments, std::string status) -> json;

    /// @brief 按段出现顺序拼接记录中的文本内容
    /// @param record 统一聊天记录
    /// @return 所有 `text` 段拼接后的文本
    [[nodiscard]] auto extractText(const json &record) -> std::string;

    /// @brief 判断记录是否包含指定类型的内容段
    /// @param record 统一聊天记录
    /// @param type 段类型，例如 `at`、`face` 或 `poke`
    [[nodiscard]] auto hasSegmentType(const json &record, std::string_view type) -> bool;

    /// @brief 判断记录是否 @ 指定用户 ID
    /// @details 目标身份读取 `target.user_id`。
    /// @param record 统一聊天记录
    /// @param userId 目标用户 ID
    [[nodiscard]] auto mentions(const json &record, const UserId &userId) -> bool;

    /// @brief 将已确认机器人账号对应的发送者、@/拍一拍目标及成员事件操作者规范化为 self。
    /// @note 只处理结构化身份字段，不修改正文、会话目标或工具历史；返回是否发生修改。
    [[nodiscard]] auto normalizeSelfReferences(json &record, const UserId &selfId) -> bool;

    /// @brief 判断记录是否由机器人发送
    [[nodiscard]] auto isAssistant(const json &record) -> bool;

    /// @brief 判断记录是否由系统账号发送
    [[nodiscard]] auto isSystem(const json &record) -> bool;

    /// @brief 生成供 Router、Executor 与记忆任务使用的精简内容投影
    /// @param record 已解析的持久化聊天记录
    /// @return 保留时间、发送者、引用和有序语义段的记录，不含图片来源
    /// @details 图片来源只从 `assets.images` 读取。
    /// @param includeToolHistory 仅 Executor 读取工具历史；其他调用方默认不包含。
    /// @details 群名片按 本群设置 决定是否发送，原始记录不受影响。
    [[nodiscard]] auto projectForAgent(
      const json &record, bool includeToolHistory = false, bool includeGroupCard = false) -> json;

    /// @brief 提取用于消息级向量召回的语义文本
    /// @param record 已解析的持久化聊天记录
    /// @return 文本段与成功识别的图片描述拼接后的查询文本
    [[nodiscard]] auto extractRecallText(const json &record) -> std::string;

    /// @brief 获取记录中指定图片的服务端来源
    /// @param record 已解析的持久化聊天记录
    /// @param imageIndex 图片在消息内按出现顺序从 0 开始的索引
    /// @return 图片存在且至少具有 file 或 url 时返回来源，否则返回空值
    /// @details 来源统一存于 `assets.images[].source`。
    [[nodiscard]] auto findImageSource(const json &record, size_t imageIndex) -> std::optional<ImageSource>;
} // namespace insoulforge::MessageRecord
