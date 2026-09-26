/// @file ChatRecordStore.hpp
/// @brief 聊天记录存储
/// @details 表：chat_records（运行时消息列表的启动恢复副本）

#pragma once

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <optional>
#include <string>
#include <vector>


/// @brief 聊天记录存储
namespace insoulforge::ChatRecordStore {
    /// @brief 追加一条会话记录
    /// @param sessionId 统一会话 ID
    /// @param role user 或 assistant
    /// @param content 完整消息的 JSON 字符串
    void addChatRecord(u64 sessionId, const std::string &role, const std::string &content);

    /// @brief 获取最近的会话记录
    /// @return 最多 limit 条，按写入时间从早到晚排列；每条包含 role 和 content
    [[nodiscard]] auto getChatRecords(u64 sessionId, i32 limit = 50) -> std::vector<json>;

    /// @brief 获取带数据库记录 ID 的最近会话记录
    /// @return 最多 limit 条，按写入时间从晚到早排列；每条包含 id、role 和 content
    [[nodiscard]] auto getChatRecordsWithIds(u64 sessionId, i32 limit = 50) -> std::vector<json>;

    /// @brief 获取存在聊天记录的全部会话 ID
    /// @return 去重后的会话 ID 列表
    [[nodiscard]] auto getSessionIds() -> std::vector<u64>;

    /// @brief 按 OneBot 消息 ID 查找指定会话中的聊天记录内容
    /// @param sessionId 统一会话 ID
    /// @param messageId OneBot 消息 ID
    /// @return 匹配记录的 content JSON 字符串；找不到时返回空值
    /// @details 逐条解析内容以兼容未使用 SQLite JSON 扩展的部署环境。
    [[nodiscard]] auto findContentByMessageId(u64 sessionId, u64 messageId) -> std::optional<std::string>;

    /// @brief 更新聊天记录内容
    void updateChatRecord(i32 recordId, const std::string &content);

    /// @brief 删除聊天记录
    void deleteChatRecord(i32 recordId);

    /// @brief 清空指定会话的所有聊天记录
    void clearSessionChatRecords(u64 sessionId);
} // namespace insoulforge::ChatRecordStore
