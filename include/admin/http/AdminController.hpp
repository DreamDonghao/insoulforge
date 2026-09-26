/// @file AdminController.hpp
/// @brief 管理后台 REST API 控制器

#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

#include <infrastructure/logging/LogBuffer.hpp>

namespace insoulforge {
    /// @brief 管理后台 REST API 控制器
    /// @details 提供管理后台的认证、配置、会话、工具、用量和运行诊断接口。
    class AdminController : public drogon::HttpController<AdminController> {
    public:
        METHOD_LIST_BEGIN
        // 管理后台认证
        ADD_METHOD_TO(AdminController::getAuthStatus, "/admin/api/auth/status", drogon::Get);

        ADD_METHOD_TO(AdminController::login, "/admin/api/auth/login", drogon::Post);

        ADD_METHOD_TO(AdminController::logout, "/admin/api/auth/logout", drogon::Post);

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

        // 黑名单
        ADD_METHOD_TO(AdminController::getBlacklist, "/admin/api/blacklist", drogon::Get);

        ADD_METHOD_TO(AdminController::addBlacklistEntry, "/admin/api/blacklist", drogon::Post);

        ADD_METHOD_TO(AdminController::removeBlacklistEntry, "/admin/api/blacklist/{qq}", drogon::Delete);

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

        /// @brief 获取当前浏览器的管理后台登录状态。
        auto getAuthStatus(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 使用本次启动生成的令牌创建管理后台会话。
        auto login(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 清除当前浏览器的管理后台会话。
        auto logout(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        // ============== LLM 配置 ==============

        /// @brief 获取所有 LLM 配置
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getLLMConfigs(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 保存指定 LLM 配置
        /// @param req HTTP 请求，body 包含配置 JSON
        /// @param callback HTTP 响应回调
        auto saveLLMConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        // ============== 提示词 ==============

        /// @brief 获取所有提示词
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getPrompts(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 保存提示词
        /// @param req HTTP 请求，body 包含 key 和 content
        /// @param callback HTTP 响应回调
        auto savePrompt(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        // ============== 表情库 ==============

        /// @brief 获取表情包库（QQ 收藏表情列表，含预览图 URL）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getEmojis(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 修改收藏表情描述（调用 NapCat set_custom_face_desc）
        /// @param req body: {emoji_id, res_id, md5, desc}
        /// @param callback HTTP 响应回调
        auto updateEmojiDesc(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 获取 Token 用量统计
        /// @param req HTTP 请求，days 查询参数可选，默认 30 天
        /// @param callback HTTP 响应回调
        auto getUsage(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 查询运行日志
        auto getLogs(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 查询最近的 HTTP 请求记录（含完整请求/响应体）
        /// @param req HTTP 请求，可选 afterId 和 limit 查询参数
        /// @param callback HTTP 响应回调
        auto getHttpTraces(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 清空 HTTP 请求记录
        auto clearHttpTraces(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 获取运行信息（启动时间、运行时长）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getSystemInfo(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 获取机器人运行状态
        auto getBotStatus(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 设置机器人运行状态
        auto setBotStatus(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 获取当前生效的 OneBot 传输方式及连接状态
        auto getOneBotStatus(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        // ============== 管理员 ==============

        /// @brief 获取管理员列表
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getAdmins(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 添加管理员
        /// @param req HTTP 请求，body 包含 qq
        /// @param callback HTTP 响应回调
        auto addAdmin(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 删除管理员
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param qq 管理员 QQ 号
        auto removeAdmin(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &qq) const -> drogon::Task<>;

        /// @brief 获取全局 QQ 黑名单。
        auto getBlacklist(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 添加全局 QQ 黑名单项。
        /// @param req HTTP 请求，body 包含 qq。
        /// @param callback HTTP 响应回调
        auto addBlacklistEntry(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 移除全局 QQ 黑名单项。
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param qq 要移除的 QQ 号。
        auto removeBlacklistEntry(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &qq) const -> drogon::Task<>;

        // ============== 会话启用状态 ==============

        /// @brief 获取已登记的会话及其启用状态
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getGroups(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) const
          -> drogon::Task<>;

        /// @brief 登记并启用会话
        /// @param req HTTP 请求，body 包含 sessionId
        /// @param callback HTTP 响应回调
        auto enableSession(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 切换会话启用状态
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto toggleSession(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &sessionId) const -> drogon::Task<>;

        /// @brief 从启用列表移除会话
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto removeSession(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &sessionId) const -> drogon::Task<>;

        /// @brief 刷新会话名称
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto refreshSessionName(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const
          -> drogon::Task<>;

        /// @brief 批量刷新已登记会话的名称
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto refreshAllSessionNames(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        // ============== 聊天记录 ==============

        /// @brief 获取有聊天记录的会话列表
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getChatSessions(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 获取指定会话的聊天记录
        /// @param req HTTP 请求，可选 limit 参数
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto getChatRecords(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &sessionId) const -> drogon::Task<>;

        /// @brief 更新聊天记录
        /// @param req HTTP 请求，body 包含 content
        /// @param callback HTTP 响应回调
        /// @param recordId 记录ID
        auto updateChatRecord(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &recordId) const -> drogon::Task<>;

        /// @brief 删除聊天记录
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param recordId 记录ID
        auto deleteChatRecord(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &recordId) const -> drogon::Task<>;

        /// @brief 清空指定会话的聊天记录
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto clearSessionChatRecords(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const
          -> drogon::Task<>;

        // ============== 会话记忆 ==============

        /// @brief 获取会话短期记忆
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto getSessionMemory(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &sessionId) const -> drogon::Task<>;

        /// @brief 更新会话短期记忆
        /// @param req HTTP 请求，body 包含 memory
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto updateSessionMemory(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const
          -> drogon::Task<>;

        // ============== 好感度 ==============

        /// @brief 获取会话成员好感度列表（按分数降序）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto getSessionAffinity(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const
          -> drogon::Task<>;

        // ============== 定时任务 ==============

        /// @brief 获取指定会话待触发的定时任务（按触发时间升序）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param sessionId 会话 ID（私聊会话带标志位）
        auto getScheduledTasks(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &sessionId) const
          -> drogon::Task<>;

        /// @brief 取消待触发的定时任务
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 任务 ID
        auto cancelScheduledTask(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const -> drogon::Task<>;

        // ============== 记忆配置 ==============

        /// @brief 获取记忆系统配置
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getMemoryConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 保存记忆系统配置
        /// @param req HTTP 请求，body 包含配置 JSON
        /// @param callback HTTP 响应回调
        auto saveMemoryConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 分页查询长期记忆
        /// @param req HTTP 请求，可选 sessionId / limit / offset 参数
        /// @param callback HTTP 响应回调
        auto getLongTermMemories(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 删除一条长期记忆
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 记录 ID
        auto deleteLongTermMemory(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback, const std::string &id) const -> drogon::Task<>;

        // ============== QQ Bot 配置 ==============

        /// @brief 获取 QQ Bot 配置
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getQQConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 保存 QQ Bot 配置
        /// @param req HTTP 请求，body 包含配置 JSON
        /// @param callback HTTP 响应回调
        auto saveQQConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        // ============== 自定义工具 ==============

        /// @brief 获取所有自定义工具
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getCustomTools(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 添加自定义工具
        /// @param req HTTP 请求，body 包含工具配置 JSON
        /// @param callback HTTP 响应回调
        auto addCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 更新自定义工具
        /// @param req HTTP 请求，body 包含工具配置 JSON
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        auto updateCustomTool(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &id) const -> drogon::Task<>;

        /// @brief 删除自定义工具
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        auto deleteCustomTool(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &id) const -> drogon::Task<>;

        /// @brief 切换自定义工具启用状态
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        auto toggleCustomTool(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &id) const -> drogon::Task<>;

        /// @brief 重载自定义工具（从数据库重新加载到 ToolRegistry）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto reloadCustomTools(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 测试自定义工具
        /// @param req HTTP 请求，body 包含 toolId 和 testArgs
        /// @param callback HTTP 响应回调
        auto testCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        // ============== 自定义工具配置 ==============

        /// @brief 获取自定义工具配置（Python解释器路径等）
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        auto getCustomToolConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        /// @brief 保存自定义工具配置
        /// @param req HTTP 请求，body 包含 pythonPath
        /// @param callback HTTP 响应回调
        auto saveCustomToolConfig(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;

        // ============== 自定义工具导入导出 ==============

        /// @brief 导出工具为 JSON 文件
        /// @param req HTTP 请求
        /// @param callback HTTP 响应回调
        /// @param id 工具ID
        auto exportCustomTool(drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback,
          const std::string &id) const -> drogon::Task<>;

        /// @brief 导入工具 JSON 文件
        /// @param req HTTP 请求，body 包含工具 JSON
        /// @param callback HTTP 响应回调
        auto importCustomTool(drogon::HttpRequestPtr req,
          std::function<void(const drogon::HttpResponsePtr &)> callback) const -> drogon::Task<>;
    };
} // namespace insoulforge
