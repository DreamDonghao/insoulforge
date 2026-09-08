/// @file ChatRecordStore.hpp
/// @brief 聊天记录存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：chat_records（运行时消息列表的启动恢复副本）

#pragma once
#include <infrastructure/JsonUtil.hpp>
#include <optional>
#include <string>
#include <vector>


/// @brief 聊天记录存储
namespace insoulforge::ChatRecordStore {
    void addChatRecord(uint64_t sessionId, const std::string &role, const std::string &content);

    [[nodiscard]] std::vector<json> getChatRecords(uint64_t sessionId, int limit = 50);

    [[nodiscard]] std::vector<json> getChatRecordsWithIds(uint64_t sessionId, int limit = 50);

    /// @brief 获取存在聊天记录的全部会话 ID
    /// @return 去重后的会话 ID 列表
    [[nodiscard]] std::vector<uint64_t> getSessionIds();

    /// @brief 按 OneBot 消息 ID 查找指定会话中的聊天记录内容
    /// @param sessionId 统一会话 ID
    /// @param messageId OneBot 消息 ID
    /// @return 匹配记录的 content JSON 字符串；找不到时返回空值
    /// @details 逐条解析内容以兼容未使用 SQLite JSON 扩展的部署环境。
    [[nodiscard]] std::optional<std::string> findContentByMessageId(uint64_t sessionId, uint64_t messageId);

    /// @brief 更新聊天记录内容
    void updateChatRecord(int recordId, const std::string &content);

    /// @brief 删除聊天记录
    void deleteChatRecord(int recordId);

    /// @brief 清空群的所有聊天记录
    void clearSessionChatRecords(uint64_t sessionId);
} // namespace insoulforge::ChatRecordStore
