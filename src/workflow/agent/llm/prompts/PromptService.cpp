/// @file PromptService.cpp
/// @brief 提示词服务 - 实现

#include <array>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <workflow/agent/llm/prompts/ExecutorSystemPrompt.hpp>
#include <workflow/agent/llm/prompts/PromptService.hpp>
#include <workflow/agent/llm/prompts/PromptStore.hpp>

namespace insoulforge {
    void PromptService::initialize(const Database &database) {
        ExecutorSystemPrompt::instance().invalidate();

        // 定义默认提示词
        const struct {
            std::string key;
            std::string content;
            std::string desc;
        } defaultPrompts[] = {// Executor - 角色系统提示词
          {.key = "executor_system",
            .content = R"(你是{botName}，在群聊中聊天的机器人。

说话风格：
- 像正常人网上聊天，不要端着
- 闲聊尽量在25字以内；学习、技术和求助按解决问题所需长度回答
- 本轮提供 response_requirements 时遵循其中的回复要求

【重要原则】
- 结合上下文选择值得回应的消息或话题，避免重复已发送的内容
- 不要说"关于xxx的问题...关于yyy的问题..."这种分点回复
- 自然地针对最后发言者回复即可
)",
            .desc = "Executor 角色系统提示词"},
          // Router - 消息路由决策提示词
          {.key = "router_system",
            .content =
              R"(你是{botName}的群聊路由决策器。结合当前聊天上下文判断是否仍需发言，并给出本轮整体回复策略。

{botName}是普通群友，发言有分寸，不必回应所有消息；不确定是否值得接话时优先 skip。
reply：仍有未覆盖的问题、值得回应的补充或反馈、感兴趣的分享，或有价值的话题可以自然接续。
skip：内容已经被回复覆盖，或只剩广告、刷屏、无实质内容的表情复读、与{botName}无关且不宜插入的群友互聊。

判断原则：
- 判断整份上下文当前是否仍有值得回应的内容，不指定某条消息。
- 先核对已经发送的内容是否覆盖问题、补充或话题；可回应的内容已全部覆盖则 skip，仍有未覆盖且值得回应的内容才 reply。
- 未交给 Executor 不等于尚未回答；新消息可能在上一轮生成期间到达，刚发送的内容可能已经覆盖它。
- 不因刚发过言就跳过，也不因出现提问就回复。activity.mode=silent 的通知仅作背景，不构成回复理由。

策略说明：
- tone: friendly（友好）/ serious（认真）/ casual（随意）。
- maxLength: 本轮整体回复的字数上限；简短接话 15～25，普通交流 50～100，详细解释 150～300，长文本最多 500。

输出严格 JSON，action 只能为 skip 或 reply：
{"action":"reply","reason":"当前仍需回复或无需回复的简短原因","strategy":{"tone":"friendly","maxLength":25}}
)",
            .desc = "Router 消息路由决策提示词"},
          // Executor - 私聊角色系统提示词
          {.key = "executor_private_system",
            .content = R"(你是{botName}，一个在QQ上和用户一对一私聊的机器人。

说话风格：
- 像正常人网上聊天，不要端着
- 用户的消息都是直接对你说的，正常回应即可，不要@对方
- 闲聊尽量在25字以内；学习、技术和求助按解决问题所需长度回答
- 本轮提供 response_requirements 时遵循其中的回复要求

【重要原则】
- 结合上下文选择值得回应的消息或话题，避免重复已发送的内容
- 不要说"关于xxx的问题...关于yyy的问题..."这种分点回复
- 私聊发送时省略 quote_message_id，直接发送即可
)",
            .desc = "Executor 私聊角色系统提示词"},
          // Router - 私聊消息路由决策提示词
          {.key = "router_private_system",
            .content =
              R"(你是{botName}的私聊路由决策器。结合当前上下文判断是否仍需回应，并给出本轮整体回复策略。

私聊中优先回应尚未覆盖的正常交流、补充或需要解释的通知。
内容已被回复覆盖，或只剩广告、刷屏和无意义重复时 skip。skip 表示本次不发言，不代表安排了稍后自动回复。

判断原则：
- 判断整份上下文当前是否仍有值得回应的内容，不指定某条消息。
- 先核对已经发送的内容是否覆盖问题、补充或话题；可回应的内容已全部覆盖则 skip，仍有未覆盖且值得回应的内容才 reply。
- 未交给 Executor 不等于尚未回答；新消息可能在上一轮生成期间到达，刚发送的内容可能已经覆盖它。
- 不因刚发过言就跳过，也不因出现提问就回复。activity.mode=silent 的通知仅作背景，不构成回复理由。

策略说明：
- tone: friendly（友好）/ serious（认真）/ casual（随意）。
- maxLength: 本轮整体回复的字数上限；简短接话 15～25，普通交流 50～100，详细解释 150～300，长文本最多 500。

输出严格 JSON，action 只能为 skip 或 reply：
{"action":"reply","reason":"当前仍需回复或无需回复的简短原因","strategy":{"tone":"friendly","maxLength":25}}
)",
            .desc = "Router 私聊消息路由决策提示词"}};

        // 插入默认提示词（如果不存在）
        for (const auto &[key, content, desc]: defaultPrompts) {
            if (!PromptStore::hasPrompt(database, key)) {
                PromptStore::setPrompt(database, key, content, desc);
                Logger::info(std::nullopt, "Prompt", fmt::format("插入默认提示词: {}", key));
            }
        }

        // 自愈: 早期版本 router_system 默认值带 fmt 转义残留(双花括号)，模型照抄导致 JSON 解析失败。
        // 若库中内容仍含损坏标记(未被用户编辑修复过)，覆盖为修复后的默认值
        if (const std::string stored = PromptStore::getPrompt(database, "router_system", "");
          stored.find("不要其他内容）：\n{{") != std::string::npos) {
            PromptStore::setPrompt(database, "router_system", defaultPrompts[1].content, defaultPrompts[1].desc);
            Logger::warn(std::nullopt, "Prompt", fmt::format("已自愈 router_system 默认值中的双花括号残留"));
        }

        // 只替换旧默认指令中的确定措辞，保留其余自定义内容。
        for (const auto *key:
          {"router_system", "router_private_system", "executor_system", "executor_private_system"}) {
            auto stored = PromptStore::getPrompt(database, key, "");
            auto updated = stored;
            for (const auto &[before, after]: std::array<std::pair<std::string_view, std::string_view>, 5>{
                   {{"只回复最新的一条消息或话题，不要同时回复多条消息",
                      "结合上下文选择值得回应的消息或话题，避免重复已发送的内容"},
                     {"只回复最新的一条消息或话题", "结合上下文选择值得回应的消息或话题，避免重复已发送的内容"},
                     {"是否应该回复最新一条消息", "根据当前聊天上下文是否仍需回复"},
                     {"1. 看最新一条消息的性质", "1. 结合新增消息、历史和已发送内容判断是否仍需回复"},
                     {"- 最后一条消息标记为 [用户]，总是用户发送的新消息",
                       "- 记录按分组提供，不保证末条是用户消息；结合已发送内容判断是否仍需回复"}}}) {
                if (const auto pos = updated.find(before); pos != std::string::npos) {
                    updated.replace(pos, before.size(), after);
                }
            }
            if (updated != stored) {
                PromptStore::setPrompt(database, key, updated);
            }
        }

        for (const auto *key: {"router_system", "router_private_system"}) {
            auto stored = PromptStore::getPrompt(database, key, "");
            auto updated = stored;
            for (const auto &[before, after]:
              std::array<std::pair<std::string_view, std::string_view>, 13>{
                {{"- [{botName}]: 机器人发送的消息",
                   "- 聊天记录使用 JSON，sender 是显示名，content.segments 是有序消息段"},
                  {"- [用户]: 用户发送的消息", "- reply_to 通过 message_id 表示引用关系"},
                  {"- 用户主动来私聊，就是想和{botName}说话，默认应该 reply",
                    "- 私聊优先回应尚未被已发送内容覆盖的正常交流"},
                  {"- 除 skip 场景外的一切正常对话", "- 尚未被已发送内容覆盖的正常对话"},
                  {"- 提问（技术、知识、生活问题都算）", "- 已发送内容尚未覆盖且值得回应的问题"},
                  {"- 纯表情、单字、感叹词（\"哈哈\"、\"好\"、\"嗯\"、\"6\"等）",
                    "- 仅剩无实质内容的表情、单字、感叹词"},
                  {"2. 看聊天记录中{botName}的发言位置：", "2. 核对已发送内容是否覆盖新增内容："},
                  {"   - 如果{botName}在最近1-2条内刚发过言，除非新消息明确针对它（追问、反驳、接它的话），否则 skip",
                    "   - 已覆盖则 skip；未覆盖且值得回应才 reply，不因刚发言就跳过"},
                  {"- {botName}刚回复过，新消息不是针对它", "- 已发送内容已经覆盖当前问题或话题"},
                  {"2. 如果{botName}刚发过言，且新消息是明确的补充或延续（如补发的后半句话），可以稍等话题完整后再回",
                    "2. 补充或延续仍未被回复覆盖时正常回应；skip 不安排稍后自动回复"},
                  {"3. 其余情况一律 reply", "3. 结合已发送内容判断是否还有值得回应的内容"},
                  {"- skip 仅用于明显的刷屏、广告、无意义的重复内容",
                    "- 内容已被回复覆盖，或只有刷屏、广告、无意义重复时 skip"},
                  {"判断{botName}根据当前聊天上下文是否仍需回复", "判断{botName}当前是否仍需回复"}}}) {
                if (const auto pos = updated.find(before); pos != std::string::npos) {
                    updated.replace(pos, before.size(), after);
                }
            }
            if (updated != stored) {
                PromptStore::setPrompt(database, key, updated);
            }
        }

        // 思考模式已改为 Executor 的 deep_think 工具：从 Router 提示词中移除 enableThinking 策略项
        // （仅删除包含 enableThinking 的行，不影响提示词其余部分的自定义内容）
        for (const char *key: {"router_system", "router_private_system"}) {
            std::string stored = PromptStore::getPrompt(database, key, "");
            if (stored.find("enableThinking") == std::string::npos) {
                continue;
            }
            std::string updated;
            for (size_t start = 0; start < stored.size();) {
                const size_t end = stored.find('\n', start);
                const size_t stop = end == std::string::npos ? stored.size() : end;
                if (const std::string line = stored.substr(start, stop - start);
                  line.find("enableThinking") == std::string::npos) {
                    updated += line;
                    updated.push_back('\n');
                }
                start = stop + 1;
            }
            PromptStore::setPrompt(database, key, updated);
            Logger::info(std::nullopt, "Prompt", fmt::format("已从 Router 提示词移除 enableThinking 策略项: {}", key));
        }

        Logger::info(std::nullopt, "Prompt", fmt::format("提示词服务初始化完成"));
    }

    auto PromptService::getPrompt(const std::string &key) -> std::string {
        std::string content = PromptStore::getPrompt(key, "");
        // 替换 {botName} 占位符
        if (content.find("{botName}") != std::string::npos) {
            const std::string &botName = Config::instance().botName;
            size_t pos = 0;
            while ((pos = content.find("{botName}", pos)) != std::string::npos) {
                content.replace(pos, 9, botName);
                pos += botName.length();
            }
        }
        return content;
    }

    void PromptService::setPrompt(const std::string &key, const std::string &content, const std::string &description) {
        PromptStore::setPrompt(key, content, description);
        ExecutorSystemPrompt::instance().invalidate();
        Logger::info(std::nullopt, "Prompt", fmt::format("提示词已更新: {}", key));
    }

    auto PromptService::getExecutorSystemPrompt() -> std::string { return getPrompt("executor_system"); }

    auto PromptService::getExecutorPrivateSystemPrompt() -> std::string { return getPrompt("executor_private_system"); }

    auto PromptService::getRouterSystemPrompt() -> std::string { return getPrompt("router_system"); }

    auto PromptService::getRouterPrivateSystemPrompt() -> std::string { return getPrompt("router_private_system"); }
} // namespace insoulforge
