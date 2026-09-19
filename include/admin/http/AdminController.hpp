/// @file AdminController.hpp
/// @brief 管理后台 REST API 控制器
/// @author donghao
/// @date 2026-04-02

#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>
#include <infrastructure/logging/LogBuffer.hpp>
#include <onebot/MessageService.hpp>

namespace insoulforge {
    /// @brief 管理后台 REST API 控制器
    /// @details 提供 Web 管理界面的所有 API 接口，包括：
    ///          - LLM 配置管理
    ///          - 提示词管理
    ///          - 表情库管理
    ///          - 管理员管理
    ///          - 启用群管理
    ///          - 知识库配置
    ///          - 聊天记录查看
    ///          - 记忆系统配置
    class AdminController : public drogon::HttpController<AdminController> {
    public:
        METHOD_LIST_BEGIN
        // LLM 配置
        ADD_METHOD_TO(AdminController::getLLMConfigs, "/admin/api/llm-configs", drogon::Get);

        ADD_METHOD_TO(AdminController::saveLLMConfig, "/admin/api/llm-config", drogon::Post);

        // 提示词
        ADD_METHOD_TO(AdminController::getPrompts, "/admin/api/prompts", drogon::Get);

        ADD_METHOD_TO(AdminController::savePrompt, "/admin/api/prompt", drogon::Post);

        // 表情包库（QQ 收藏表情）
        ADD_METHOD_TO(AdminController::getEmojis, "/admin/api/emojis", drogon::Get);

        ADD_METHOD_TO(AdminController::updateEmojiDesc, "/admin/api/emoji/desc", drogon::Post);

        // 用量统计
        ADD_METHOD_TO(AdminController::getUsage, "/admin/api/usage", drogon::Get);

        // 运行日志
        ADD_METHOD_TO(AdminController::getLogs, "/admin/api/logs", drogon::Get);

        // HTTP 请求调试（最近请求的完整请求/响应体）
        ADD_METHOD_TO(AdminController::getHttpTraces, "/admin/api/http-traces", drogon::Get);

        ADD_METHOD_TO(AdminController::clearHttpTraces, "/admin/api/http-traces", drogon::Delete);

        // 运行信息（启动时间/运行时长）
        ADD_METHOD_TO(AdminController::getSystemInfo, "/admin/api/system-info", drogon::Get);

        // 机器人运行状态
        ADD_METHOD_TO(AdminController::getBotStatus, "/admin/api/bot-status", drogon::Get);

        ADD_METHOD_TO(AdminController::setBotStatus, "/admin/api/bot-status", drogon::Post);

        // OneBot 运行状态
        ADD_METHOD_TO(AdminController::getOneBotStatus, "/admin/api/onebot-status", drogon::Get);

        // 管理员
        ADD_METHOD_TO(AdminController::getAdmins, "/admin/api/admins", drogon::Get);

        ADD_METHOD_TO(AdminController::addAdmin, "/admin/api/admin", drogon::Post);

        ADD_METHOD_TO(AdminController::removeAdmin, "/admin/api/admin/{qq}", drogon::Delete);

        // 启用群
        ADD_METHOD_TO(AdminController::getGroups, "/admin/api/groups", drogon::Get);

        ADD_METHOD_TO(AdminController::enableSession, "/admin/api/group", drogon::Post);

        ADD_METHOD_TO(AdminController::toggleSession, "/admin/api/group/{groupId}/toggle", drogon::Post);

        ADD_METHOD_TO(AdminController::removeSession, "/admin/api/group/{groupId}", drogon::Delete);

        ADD_METHOD_TO(AdminController::refreshSessionName, "/admin/api/group/{groupId}/refresh-name", drogon::Post);

        // 批量刷新群名
        ADD_METHOD_TO(AdminController::refreshAllSessionNames, "/admin/api/groups/refresh-names", drogon::Post);

        // 聊天记录
        ADD_METHOD_TO(AdminController::getChatSessions, "/admin/api/chat-groups", drogon::Get);

        ADD_METHOD_TO(AdminController::getChatRecords, "/admin/api/chat-records/{groupId}", drogon::Get);

        ADD_METHOD_TO(AdminController::updateChatRecord, "/admin/api/chat-record/{recordId}", drogon::Put);

        ADD_METHOD_TO(AdminController::deleteChatRecord, "/admin/api/chat-record/{recordId}", drogon::Delete);

        ADD_METHOD_TO(
          AdminController::clearSessionChatRecords, "/admin/api/chat-records/{groupId}/clear", drogon::Delete);

        // 群记忆
        ADD_METHOD_TO(AdminController::getSessionMemory, "/admin/api/memory/{groupId}", drogon::Get);

        ADD_METHOD_TO(AdminController::updateSessionMemory, "/admin/api/memory/{groupId}", drogon::Put);

        // 好感度
        ADD_METHOD_TO(AdminController::getSessionAffinity, "/admin/api/affinity/{sessionId}", drogon::Get);

        // 定时任务
        ADD_METHOD_TO(AdminController::getScheduledTasks, "/admin/api/scheduled-tasks/{sessionId}", drogon::Get);

        ADD_METHOD_TO(AdminController::cancelScheduledTask, "/admin/api/scheduled-task/{id}", drogon::Delete);

        // 记忆配置
        ADD_METHOD_TO(AdminController::getMemoryConfig, "/admin/api/memory-config", drogon::Get);

        ADD_METHOD_TO(AdminController::saveMemoryConfig, "/admin/api/memory-config", drogon::Post);

        // 长期记忆
        ADD_METHOD_TO(AdminController::getLongTermMemories, "/admin/api/long-term-memory", drogon::Get);

        ADD_METHOD_TO(AdminController::deleteLongTermMemory, "/admin/api/long-term-memory/{id}", drogon::Delete);

        // QQ Bot 配置
        ADD_METHOD_TO(AdminController::getQQConfig, "/admin/api/qq-config", drogon::Get);

        ADD_METHOD_TO(AdminController::saveQQConfig, "/admin/api/qq-config", drogon::Post);

        // 自定义工具
        ADD_METHOD_TO(AdminController::getCustomTools, "/admin/api/custom-tools", drogon::Get);

        ADD_METHOD_TO(AdminController::addCustomTool, "/admin/api/custom-tool", drogon::Post);

        ADD_METHOD_TO(AdminController::updateCustomTool, "/admin/api/custom-tool/{id}", drogon::Put);

        ADD_METHOD_TO(AdminController::deleteCustomTool, "/admin/api/custom-tool/{id}", drogon::Delete);

        ADD_METHOD_TO(AdminController::toggleCustomTool, "/admin/api/custom-tool/{id}/toggle", drogon::Post);

        ADD_METHOD_TO(AdminController::reloadCustomTools, "/admin/api/custom-tools/reload", drogon::Post);

        ADD_METHOD_TO(AdminController::testCustomTool, "/admin/api/custom-tool/test", drogon::Post);

        // 自定义工具导入导出
        ADD_METHOD_TO(AdminController::exportCustomTool, "/admin/api/custom-tool/{id}/export", drogon::Get);

        ADD_METHOD_TO(AdminController::importCustomTool, "/admin/api/custom-tool/import", drogon::Post);

        // 自定义工具配置
        ADD_METHOD_TO(AdminController::getCustomToolConfig, "/admin/api/custom-tool-config", drogon::Get);

        ADD_METHOD_TO(AdminController::saveCustomToolConfig, "/admin/api/custom-tool-config", drogon::Post);

        METHOD_LIST_END

        // ============== LLM 配置 ==============

        /// @brief 获取所有 LLM 配置
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getLLMConfigs(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 保存指定 LLM 配置
        /// @param req HTTP 请求，body 包含配置 JSON
        /// @param callback HTTP 响应回调
        drogon::Task<> saveLLMConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 提示词 ==============

        /// @brief 获取所有提示词
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getPrompts(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 保存提示词
        /// @param req HTTP 请求，body 包含 key 和 content
        /// @param callback HTTP 响应回调
        drogon::Task<> savePrompt(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 表情库 ==============

        /// @brief 获取表情包库（QQ 收藏表情列表，含预览图 URL）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getEmojis(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 修改收藏表情描述（调用 NapCat set_custom_face_desc）
        /// @param req body: {emoji_id, res_id, md5, desc}
        drogon::Task<> updateEmojiDesc(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 获取 Token 用量统计
        /// @param req query: days（可选，默认30）
        drogon::Task<> getUsage(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 查询运行日志
        drogon::Task<> getLogs(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 查询最近的 HTTP 请求记录（含完整请求/响应体）
        /// @param req query: afterId / limit（可选）
        drogon::Task<> getHttpTraces(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 清空 HTTP 请求记录
        drogon::Task<> clearHttpTraces(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 获取运行信息（启动时间、运行时长）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getSystemInfo(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 获取机器人运行状态
        drogon::Task<> getBotStatus(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 设置机器人运行状态
        drogon::Task<> setBotStatus(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 获取当前生效的 OneBot 传输方式及连接状态
        drogon::Task<> getOneBotStatus(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 管理员 ==============

        /// @brief 获取管理员列表
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getAdmins(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 添加管理员
        /// @param req HTTP 请求，body 包含 qq
        /// @param callback HTTP 响应回调
        drogon::Task<> addAdmin(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 删除管理员
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param qq 管理员 QQ 号
        drogon::Task<> removeAdmin(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &qq) const;

        // ============== 启用群 ==============

        /// @brief 获取启用群列表
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getGroups(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 启用群
        /// @param req HTTP 请求，body 包含 sessionId
        /// @param callback HTTP 响应回调
        drogon::Task<> enableSession(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 切换群启用/禁用状态
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> toggleSession(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        /// @brief 删除群（从数据库移除）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> removeSession(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        /// @brief 刷新群名称
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> refreshSessionName(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        /// @brief 批量刷新所有群名称
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> refreshAllSessionNames(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 聊天记录 ==============

        /// @brief 获取所有有聊天记录的群列表
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getChatSessions(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 获取群聊天记录
        /// @param req HTTP 请求，可选 limit 参数
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> getChatRecords(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        /// @brief 更新聊天记录
        /// @param req HTTP 请求，body 包含 content
        /// @param callback HTTP 响应回调
        /// @param recordId 记录ID
        drogon::Task<> updateChatRecord(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &recordId) const;

        /// @brief 删除聊天记录
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param recordId 记录ID
        drogon::Task<> deleteChatRecord(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &recordId) const;

        /// @brief 清空群的所有聊天记录
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> clearSessionChatRecords(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        // ============== 群记忆 ==============

        /// @brief 获取群短期记忆
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> getSessionMemory(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        /// @brief 更新群记忆
        /// @param req HTTP 请求，body 包含 memory
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> updateSessionMemory(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        // ============== 好感度 ==============

        /// @brief 获取会话成员好感度列表（按分数降序）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> getSessionAffinity(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        // ============== 定时任务 ==============

        /// @brief 获取指定会话待触发的定时任务（按触发时间升序）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        drogon::Task<> getScheduledTasks(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const;

        /// @brief 取消待触发的定时任务
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 任务 ID
        drogon::Task<> cancelScheduledTask(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const;

        // ============== 记忆配置 ==============

        /// @brief 获取记忆系统配置
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getMemoryConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 保存记忆系统配置
        /// @param req HTTP 请求，body 包含配置 JSON
        /// @param callback HTTP 响应回调
        drogon::Task<> saveMemoryConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 分页查询长期记忆
        /// @param req HTTP 请求，可选 sessionId / limit / offset 参数
        /// @param callback HTTP 响应回调
        drogon::Task<> getLongTermMemories(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 删除一条长期记忆
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 记录 ID
        drogon::Task<> deleteLongTermMemory(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const;

        // ============== QQ Bot 配置 ==============

        /// @brief 获取 QQ Bot 配置
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getQQConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 保存 QQ Bot 配置
        /// @param req HTTP 请求，body 包含配置 JSON
        /// @param callback HTTP 响应回调
        drogon::Task<> saveQQConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 自定义工具 ==============

        /// @brief 获取所有自定义工具
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getCustomTools(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 添加自定义工具
        /// @param req HTTP 请求，body 包含工具配置 JSON
        /// @param callback HTTP 响应回调
        drogon::Task<> addCustomTool(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 更新自定义工具
        /// @param req HTTP 请求，body 包含工具配置 JSON
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        drogon::Task<> updateCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const;

        /// @brief 删除自定义工具
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        drogon::Task<> deleteCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const;

        /// @brief 切换自定义工具启用状态
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        drogon::Task<> toggleCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const;

        /// @brief 重载自定义工具（从数据库重新加载到 ToolRegistry）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> reloadCustomTools(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 测试自定义工具
        /// @param req HTTP 请求，body 包含 toolId 和 testArgs
        /// @param callback HTTP 响应回调
        drogon::Task<> testCustomTool(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 自定义工具配置 ==============

        /// @brief 获取自定义工具配置（Python解释器路径等）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        drogon::Task<> getCustomToolConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        /// @brief 保存自定义工具配置
        /// @param req HTTP 请求，body 包含 pythonPath
        /// @param callback HTTP 响应回调
        drogon::Task<> saveCustomToolConfig(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;

        // ============== 自定义工具导入导出 ==============

        /// @brief 导出工具为 JSON 文件
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        drogon::Task<> exportCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const;

        /// @brief 导入工具 JSON 文件
        /// @param req HTTP 请求，body 包含工具 JSON
        /// @param callback HTTP 响应回调
        drogon::Task<> importCustomTool(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const;
    };
} // namespace insoulforge
