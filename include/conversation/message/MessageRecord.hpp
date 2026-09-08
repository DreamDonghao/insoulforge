/// @file MessageRecord.hpp
/// @brief 聊天记录富内容的构造、兼容与投影
/// @details 持久化记录以 `segments` 保存唯一的内容顺序，图片传输来源仅存于 `assets.images`。
///          面向 Agent、记忆和工具的读取逻辑统一在此处兼容旧记录，避免重复暴露图片 URL。

#pragma once
#include <infrastructure/JsonUtil.hpp>
#include <optional>
#include <string>
#include <string_view>

namespace insoulforge::MessageRecord {
    /// @brief 图片的 OneBot 获取信息
    /// @details 仅供服务端工具使用，绝不能直接投影到 LLM 上下文。
    struct ImageSource {
        std::string file; ///< OneBot 图片缓存文件名
        std::string url; ///< OneBot 图片下载地址
    };

    [[nodiscard]] uint64_t getSessionId(const json &record);

    /// @brief 将机器人实际发送的 CQ 内容转换为统一聊天记录
    /// @param senderName 机器人的显示名称
    /// @param messageId OneBot 返回的消息 ID
    /// @param content 已发送的 CQ 内容
    /// @return 不含重复 text 字段的富内容记录
    /// @details 收藏表情仅记录 `sticker.name`；普通 CQ 图片只保留图片动作，不保存外部 URL。
    [[nodiscard]] json createAssistantRecord(std::string senderName, uint64_t messageId, const std::string &content);

    /// @brief 按段出现顺序拼接记录中的文本内容
    /// @param record 统一聊天记录
    /// @return 所有 `text` 段拼接后的文本；无段时兼容旧版 `text` 字段
    [[nodiscard]] std::string extractText(const json &record);

    /// @brief 判断记录是否包含指定类型的内容段
    /// @param record 统一聊天记录
    /// @param type 段类型，例如 `at`、`face` 或 `poke`
    [[nodiscard]] bool hasSegmentType(const json &record, std::string_view type);

    /// @brief 判断记录是否 @ 指定 QQ 号
    /// @details 同时兼容新结构 `target.qq` 与旧结构 `qq`。
    /// @param record 统一聊天记录
    /// @param qqNumber 目标 QQ 号
    [[nodiscard]] bool mentions(const json &record, uint64_t qqNumber);

    /// @brief 判断记录是否由机器人发送
    [[nodiscard]] bool isAssistant(const json &record);

    /// @brief 判断记录是否由系统账号发送
    [[nodiscard]] bool isSystem(const json &record);

    /// @brief 生成供 Router、Executor 与记忆任务使用的精简内容投影
    /// @param record 已解析的持久化聊天记录
    /// @return 保留时间、发送者、引用和有序语义段的记录，不含图片来源
    /// @details 同时兼容旧版 `text`、`images`、`faces` 与 `notifications` 字段。
    [[nodiscard]] json projectForAgent(const json &record);

    /// @brief 提取用于消息级向量召回的语义文本
    /// @param record 已解析的持久化聊天记录
    /// @return 文本段与成功识别的图片描述拼接后的查询文本
    [[nodiscard]] std::string extractRecallText(const json &record);

    /// @brief 获取记录中指定图片的服务端来源
    /// @param record 已解析的持久化聊天记录
    /// @param imageIndex 图片在消息内按出现顺序从 0 开始的索引
    /// @return 图片存在且至少具有 file 或 url 时返回来源，否则返回空值
    /// @details 兼容新结构 `assets.images` 和旧结构 `images`。
    [[nodiscard]] std::optional<ImageSource> findImageSource(const json &record, size_t imageIndex);
} // namespace insoulforge::MessageRecord
