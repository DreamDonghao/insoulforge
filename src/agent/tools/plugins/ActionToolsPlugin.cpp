/// @file ActionToolsPlugin.cpp
/// @brief 动作工具插件实现（ACTION，执行操作、产生副作用）

#include <stdexcept>

#include <regex>

#include <admin/access/AdminStore.hpp>
#include <admin/access/BlacklistStore.hpp>
#include <agent/ability/AsyncTaskManager.hpp>
#include <agent/runtime/ExecutorAgent.hpp>
#include <agent/tools/ToolArgument.hpp>
#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/plugins/ActionToolsPlugin.hpp>
#include <conversation/history/ChatRecordStore.hpp>
#include <conversation/message/MessageRecord.hpp>
#include <conversation/message/SessionId.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <include/agent/ability/TaskScheduler.hpp>
#include <include/agent/tools/ToolRegistry.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <media/CharacterImageStore.hpp>
#include <media/ImageDescriptionService.hpp>
#include <media/ImageGenerationService.hpp>
#include <onebot/messaging/MessageService.hpp>
#include <onebot/transport/OneBotClient.hpp>

namespace insoulforge {
    namespace {
        auto requestedImageSize(const json &args) -> std::optional<std::string> {
            auto size = trim(argString(args, "size"));
            if (size.empty()) {
                size = "2048x2048";
            }
            static const std::regex sizePattern{R"([1-9][0-9]{0,4}x[1-9][0-9]{0,4})"};
            return std::regex_match(size, sizePattern) ? std::optional{std::move(size)} : std::nullopt;
        }

        auto startImageTask(const u64 sessionId, std::string prompt, std::string size,
          std::optional<std::string> referenceUrl = std::nullopt,
          std::optional<std::string> referenceDataUrl = std::nullopt) -> std::string {
            const auto [status, taskId] =
              AsyncTaskManager::instance().start(sessionId, referenceUrl ? "编辑图片" : "生成图片",
                [sessionId, prompt = std::move(prompt), size = std::move(size), referenceUrl = std::move(referenceUrl),
                  referenceDataUrl = std::move(referenceDataUrl)]() mutable -> drogon::Task<AsyncTaskManager::Result> {
                    const bool useSelfReference = referenceDataUrl.has_value();
                    if (referenceUrl) {
                        referenceDataUrl = co_await ImageDescriptionService::referenceDataUrl(*referenceUrl, sessionId);
                        if (!referenceDataUrl) {
                            throw std::runtime_error("原图下载失败或格式不受支持，请重新发送静态图片");
                        }
                    }
                    std::string generationPrompt = prompt;
                    if (useSelfReference) {
                        generationPrompt = "请以输入的角色参考图片为人物形象依据，保持角色可辨认的外观特征。"
                                           "除非画面要求明确提出修改，否则发型、发色、耳朵、服饰等以参考图为准，"
                                           "不要根据文字重新设计角色。请按以下画面要求生成：" +
                                           prompt;
                    }
                    auto [message, source] = co_await ImageGenerationService::generate(
                      std::move(generationPrompt), size, sessionId, std::move(referenceDataUrl));
                    std::string description = "依据生成提示词：" + prompt;
                    try {
                        if (const auto recognized =
                              co_await ImageDescriptionService::describeGeneratedImage(std::move(source), sessionId)) {
                            description = recognized->description;
                        }
                    } catch (const std::exception &error) {
                        Logger::warn(sessionId, "ImageGeneration", fmt::format("生成图片识别失败: {}", error.what()));
                    }
                    co_return AsyncTaskManager::Result{
                      .content = std::move(message), .imageDescription = std::move(description)};
                });
            switch (status) {
                case AsyncTaskManager::StartResult::Status::Started:
                    return fmt::format("图片任务 #{} 已启动，完成后会直接发送到本会话。", taskId);
                case AsyncTaskManager::StartResult::Status::Busy:
                    return fmt::format("本会话已有异步任务 #{} 在执行，请等待完成。", taskId);
                case AsyncTaskManager::StartResult::Status::Stopping:
                    return "程序正在退出，无法启动图片任务。";
            }
            return "无法启动图片任务。";
        }
    } // namespace

    auto ActionToolsPlugin::id() const noexcept -> std::string_view { return "builtin.action"; }

    /// @brief 注册动作执行工具（ACTION，执行操作、产生副作用）
    void ActionToolsPlugin::registerTools(ToolRegistry &registry) const {

        registry.registerTool(
          {
            .name = "generate_image",
            .description = "根据用户明确提出的画面要求异步生成一张图片。工具立即返回任务状态，图片生成完成后"
                           "会自动发送到当前会话，不要再调用 reply 发送图片。绘制你自己的角色形象时，"
                           "将 use_self_reference 设为 true，以后台上传的角色图为准；prompt 只描述用户要求的"
                           "场景、动作、表情等变化，不重复编造参考图中已有的外观设定。其他画面不要设置此参数。"
                           "一次会话同时只能生成一张。",
            .parameters = json{{"type", "object"},
              {"properties",
                {{"prompt", {{"type", "string"},
                              {"description", "画面要求；使用角色参考图时只写场景、动作等变化，不重复描述原有外观"}}},
                  {"size",
                    {{"type", "string"}, {"description", "图片尺寸，格式为宽x高，如 1024x1024；默认 2048x2048"}}},
                  {"use_self_reference",
                    {{"type", "boolean"},
                      {"description", "画面主体是你自己时设为 true，引用后台上传的角色形象图；默认 false"}}}}},
              {"required", {"prompt"}}},
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto prompt = trim(argString(args, "prompt"));
                if (prompt.empty() || prompt.size() > 4000) {
                    co_return std::string("图片描述不能为空且不得超过 4000 字节。");
                }
                const auto size = requestedImageSize(args);
                if (!size) {
                    co_return std::string("图片尺寸格式应为宽x高，例如 1024x1024。");
                }
                if (const auto &api = Config::instance().imageGeneration;
                  api.apiKey.empty() || api.baseUrl.empty() || api.path.empty() || api.model.empty()) {
                    co_return std::string("图片生成接口尚未配置完整。");
                }
                std::optional<std::string> referenceDataUrl;
                if (getBool(args, "use_self_reference")) {
                    const auto image = CharacterImageStore::load();
                    if (!image) {
                        co_return std::string("尚未在管理后台上传角色形象图，无法按自身形象生图。请先上传后重试。");
                    }
                    referenceDataUrl = ImageDescriptionService::staticImageDataUrl(image->bytes);
                    if (!referenceDataUrl) {
                        co_return std::string("角色形象图格式无效，请在管理后台重新上传。");
                    }
                }
                co_return startImageTask(ctx.sessionId, prompt, *size, std::nullopt, std::move(referenceDataUrl));
            },
          },
          ToolCategory::ACTION);

        registry.registerTool(
          {
            .name = "edit_image",
            .description =
              "用户明确要求修改或参照聊天中的一张图片时调用。用同会话的 message_id 和 image_index 指定原图，"
              "prompt 说明期望的修改。原图在后台按需读取，任务完成后自动发送图片；不要传入或猜测图片 URL。",
            .parameters = json{{"type", "object"},
              {"properties",
                {{"message_id", {{"type", "string"}, {"description", "原图所在消息的 message_id"}}},
                  {"image_index",
                    {{"type", "integer"}, {"minimum", 0}, {"description", "原图在消息中的索引，从 0 开始"}}},
                  {"prompt", {{"type", "string"}, {"description", "对原图的修改要求"}}},
                  {"size", {{"type", "string"}, {"description", "输出尺寸，格式为宽x高；默认 2048x2048"}}}}},
              {"required", {"message_id", "image_index", "prompt"}}},
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto prompt = trim(argString(args, "prompt"));
                if (prompt.empty() || prompt.size() > 4000) {
                    co_return std::string("图片修改要求不能为空且不得超过 4000 字节。");
                }
                const auto size = requestedImageSize(args);
                if (!size) {
                    co_return std::string("图片尺寸格式应为宽x高，例如 1024x1024。");
                }
                const u64 messageId = parseUInt64(argString(args, "message_id"));
                const i32 imageIndex = getInt(args, "image_index", -1);
                if (messageId == 0 || imageIndex < 0) {
                    co_return std::string("请提供有效的图片消息 ID 和从 0 开始的图片索引。");
                }
                if (const auto &api = Config::instance().imageGeneration;
                  api.apiKey.empty() || api.baseUrl.empty() || api.path.empty() || api.model.empty()) {
                    co_return std::string("图片生成接口尚未配置完整。");
                }

                json record;
                for (const json &message: ctx.messageSnapshot) {
                    if (parseUInt64(getStr(message, "message_id")) == messageId) {
                        record = message;
                        break;
                    }
                }
                if (record.is_null()) {
                    const auto content = ChatRecordStore::findContentByMessageId(ctx.sessionId, messageId);
                    if (!content || !tryParseJson(*content, record)) {
                        co_return std::string("未找到该会话中的图片消息。");
                    }
                }
                const auto source = MessageRecord::findImageSource(record, static_cast<size_t>(imageIndex));
                if (!source || source->url.empty()) {
                    co_return std::string("该消息没有可下载的原图，请让用户重新发送静态图片。");
                }
                co_return startImageTask(ctx.sessionId, prompt, *size, source->url);
            },
          },
          ToolCategory::ACTION);

        // 演示工具只验证后台启动、会话互斥和独立发送；实际能力需提供自己的处理器。
        registry.registerTool(
          {
            .name = "demo_async_task",
            .description = "仅供明确测试异步任务功能时使用。启动一个约3秒后向当前会话发送完成回执的演示任务；"
                           "不是生图或其它实际业务能力。已有任务运行时返回其编号。",
            .parameters = json{{"type", "object"}, {"properties", json::object()}},
            .handler = [](const json, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto [status, taskId] = AsyncTaskManager::instance().start(
                  ctx.sessionId, "演示延时任务", []() -> drogon::Task<AsyncTaskManager::Result> {
                      co_await drogon::sleepCoro(drogon::app().getLoop(), 3.0);
                      co_return AsyncTaskManager::Result{.content = "异步演示任务已完成。"};
                  });
                switch (status) {
                    case AsyncTaskManager::StartResult::Status::Started:
                        co_return fmt::format("演示任务 #{} 已启动，完成后会直接发送到本会话。", taskId);
                    case AsyncTaskManager::StartResult::Status::Busy:
                        co_return fmt::format("本会话已有异步任务 #{} 在执行，请等待完成。", taskId);
                    case AsyncTaskManager::StartResult::Status::Stopping:
                        co_return std::string("程序正在退出，无法启动异步任务。");
                }
                co_return std::string("无法启动异步任务。");
            },
          },
          ToolCategory::ACTION);

        // send_face
        const json faceParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "id": {
                        "type": "integer",
                        "description": "表情ID，常用: 1-发呆, 2-撇嘴, 3-色, 4-发呆, 5-得意, 6-流泪, 7-害羞, 8-闭嘴, 9-睡, 10-大哭, 11-尴尬, 12-发怒, 13-调皮, 14-呲牙, 15-惊讶, 16-难过, 17-酷, 18-冷汗, 19-抓狂, 20-吐, 21-偷笑, 22-可爱, 23-白眼, 24-傲慢, 25-饥饿, 26-困, 27-惊恐, 28-流汗, 29-憨笑, 30-大兵, 31-奋斗, 32-咒骂, 33-疑问, 34-嘘, 35-晕, 36-折磨, 37-衰, 38-骷髅, 39-敲打, 40-再见"
                    }
                },
                "required": ["id"]
            })json");
        registry.registerTool(
          {
            .name = "send_face",
            .description = "获取QQ原生表情的CQ码。返回的CQ码必须复制到reply的content中，非必要不使用",
            .parameters = faceParams,
            .handler = [](const json args, ToolCallContext) -> drogon::Task<std::string> {
                const i32 id = getInt(args, "id", 1);
                co_return fmt::format("[CQ:face,id={}]", id);
            },
          },
          ToolCategory::ACTION);

        // send_image
        const json imageParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "url": {
                        "type": "string",
                        "description": "图片URL地址"
                    }
                },
                "required": ["url"]
            })json");
        registry.registerTool(
          {
            .name = "send_image",
            .description = "获取网络图片的CQ码。提供图片URL。返回的CQ码必须复制到reply的content中。",
            .parameters = imageParams,
            .handler = [](const json args, ToolCallContext) -> drogon::Task<std::string> {
                std::string url = argString(args, "url");
                if (url.empty()) {
                    co_return std::string("请提供图片URL");
                }
                co_return fmt::format("[CQ:image,file={}]", url);
            },
          },
          ToolCategory::ACTION);

        // send_sticker - 直接把表情包作为独立消息发出（经 MessageService，发送后记入聊天记录）
        const json stickerParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "description": "表情名称（先调list_stickers查看可用名称）"
                    }
                },
                "required": ["name"]
            })json");
        registry.registerTool(
          {
            .name = "send_sticker",
            .description = "中途发送一张QQ收藏表情包（独立消息直接发出，不经reply；发出后回合不结束，"
                           "最终仍用reply/no_reply收尾）。先调list_stickers查看可用表情名。",
            .parameters = stickerParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                std::string name = argString(args, "name");
                if (name.empty()) {
                    co_return std::string("请提供表情名称");
                }
                const auto sessionId = ctx.sessionId;
                const json emoji = co_await ToolRuntime::findFavoriteEmoji(name, sessionId);
                if (emoji.is_null()) {
                    co_return fmt::format("表情'{}'不存在，先调list_stickers查看可用表情", name);
                }

                // 商城表情（字段齐全）走 mface；个人收藏表情走 image+sub_type=1（QQ 渲染为表情）
                std::string cqCode;
                if (getBool(emoji, "is_mark_face") && !getStr(emoji, "emoji_id").empty() &&
                    !getStr(emoji, "key").empty()) {
                    cqCode = fmt::format("[CQ:mface,summary={},emoji_id={},emoji_package_id={},key={}]",
                      getStr(emoji, "summary"), getStr(emoji, "emoji_id"), getStr(emoji, "emoji_package_id"),
                      getStr(emoji, "key"));
                } else if (const std::string url = getStr(emoji, "url"); !url.empty()) {
                    cqCode = fmt::format("[CQ:image,file={},sub_type=1,summary={}]", url, getStr(emoji, "summary"));
                } else {
                    co_return fmt::format("表情'{}'缺少图片地址，无法发送", name);
                }

                // 走 MessageService 发送：成功后自动记入会话列表并推送 WebSocket，
                // 后续轮次模型能从记录中看到自己发过这张表情
                std::optional<u64> messageId;
                if (SessionId::isPrivate(sessionId)) {
                    messageId = co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), cqCode);
                } else {
                    messageId = co_await MessageService::sendGroupMsg(sessionId, cqCode);
                }
                if (!messageId) {
                    co_return std::string("表情发送失败，请改用文字回复或稍后重试");
                }
                co_return fmt::format("已发送表情「{}」", name);
            },
          },
          ToolCategory::ACTION);

        // reply_and_continue - 发送过程消息，回合不结束，最终回复仍由 reply/no_reply 收尾
        const json continueParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "content": {
                        "type": "string",
                        "description": "过程消息内容，简短一句话，如：稍等，我去查一下"
                    }
                },
                "required": ["content"]
            })json");
        registry.registerTool(
          {
            .name = "reply_and_continue",
            .description = "中途发送一条文字消息（发出后回合不结束，最终仍用reply/no_reply收尾）。两个场景："
                           "1) 接下来要执行耗时操作（搜索、深度思考、查资料等用户需要等待的事），先发一句"
                           "「稍等，我去查一下」，再调用耗时工具，拿到结果后用 reply 给出最终回复"
                           "（操作失败也要 reply 说明）；2) 想在正式回复前先发其他内容（连续多条消息）。"
                           "日常单条回复直接用 reply。",
            .parameters = continueParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const std::string content = ExecutorAgent::cleanReplyContent(argString(args, "content"));
                if (content.empty()) {
                    co_return std::string("请提供要发送的过程消息内容");
                }

                // 与 send_sticker 相同：经 MessageService 发送，成功后记入会话列表并推送 WebSocket
                const auto sessionId = ctx.sessionId;
                std::optional<u64> messageId;
                if (SessionId::isPrivate(sessionId)) {
                    messageId = co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), content);
                } else {
                    messageId = co_await MessageService::sendGroupMsg(sessionId, content);
                }
                if (!messageId) {
                    co_return std::string("过程消息发送失败，请直接继续完成最终回复");
                }
                co_return std::string(
                  "过程消息已发送，请继续后续处理；最终回复仍用 reply 工具给出，操作失败也要 reply 告知用户");
            },
          },
          ToolCategory::ACTION);

        // save_sticker - 保存别人发的表情为QQ收藏表情
        const json saveParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "message_id": {
                        "type": "string",
                        "description": "含目标图片的聊天记录消息ID"
                    },
                    "image_index": {
                        "type": "integer",
                        "minimum": 0,
                        "description": "图片在该消息中的从0开始索引"
                    },
                    "name": {
                        "type": "string",
                        "description": "给表情起的简短名字（根据图片内容），如: 摸头、猫猫惊讶"
                    }
                },
                "required": ["message_id", "image_index", "name"]
            })json");
        registry.registerTool(
          {
            .name = "save_sticker",
            .description = "把用户消息中的图片保存为自己的QQ收藏表情并设置描述名称。仅在用户明确要求保存表情时使用。"
                           "传入聊天记录的message_id和图片在该消息中的image_index；不要猜测或传入图片URL。"
                           "name必须起一个能体现图片内容的名字，方便以后用send_sticker引用。",
            .parameters = saveParams,
            .handler = [](json args, ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto sessionId = ctx.sessionId;
                const u64 messageId = parseUInt64(argString(args, "message_id"));
                const i32 imageIndex = getInt(args, "image_index", -1);
                const std::string name = argString(args, "name");
                if (messageId == 0)
                    co_return std::string("请提供有效的图片消息ID(message_id)");
                if (imageIndex < 0)
                    co_return std::string("请提供从0开始的图片索引(image_index)");
                if (name.empty())
                    co_return std::string("请提供表情名称(name)");

                json record;
                for (const json &message: ctx.messageSnapshot) {
                    if (parseUInt64(getStr(message, "message_id")) == messageId) {
                        record = message;
                        break;
                    }
                }
                if (record.is_null()) {
                    const auto content = ChatRecordStore::findContentByMessageId(sessionId, messageId);
                    if (!content) {
                        co_return std::string("未找到该会话中的图片消息，无法保存表情");
                    }
                    if (!tryParseJson(*content, record)) {
                        co_return std::string("图片消息记录损坏，无法保存表情");
                    }
                }
                if (getStr(atOrNull(record, "sender"), "qq") == "self") {
                    co_return std::string("不能将机器人主动发送的图片保存为收藏表情");
                }
                const auto source = MessageRecord::findImageSource(record, static_cast<size_t>(imageIndex));
                if (!source) {
                    co_return std::string("该消息不存在指定图片，或图片来源已不可用");
                }
                const std::string &file = source->file;
                const std::string &url = source->url;

                Logger::info(sessionId, "Sticker",
                  fmt::format("save_sticker: message_id={} image_index={} file={}", messageId, imageIndex, file));

                // 优先获取容器内的图片路径；商城表情可能无法通过 get_image 获取。
                std::string containerPath;
                if (const auto path = co_await OneBotClient::getImage(file, sessionId)) {
                    containerPath = *path;
                }

                // 获取路径失败时，通过 URL 将图片下载到容器内。
                if (containerPath.empty()) {
                    if (url.empty()) {
                        co_return std::string("获取图片失败，请确认图片仍可访问");
                    }
                    if (const auto path = co_await OneBotClient::downloadFile(url, sessionId)) {
                        containerPath = *path;
                    }
                    if (containerPath.empty()) {
                        co_return std::string("获取图片失败，可能是图片链接已过期，请让对方重新发送后立即保存");
                    }
                }

                // 记录已有表情的 res_id，以便保存后找出新表情。
                ToolRuntime::invalidateFavoriteEmojiCache();
                std::set<std::string> beforeIds;
                for (const auto &e: co_await ToolRuntime::fetchFavoriteEmojis(sessionId)) {
                    if (const std::string rid = getStr(e, "res_id"); !rid.empty()) {
                        beforeIds.insert(rid);
                    }
                }

                // 将图片保存为 QQ 收藏表情。
                if (!co_await OneBotClient::addCustomFace(containerPath, sessionId)) {
                    co_return std::string("保存为收藏表情失败");
                }

                // 找到新增的表情并设置描述。
                ToolRuntime::invalidateFavoriteEmojiCache();
                json newItem;
                for (const auto &e: co_await ToolRuntime::fetchFavoriteEmojis(sessionId)) {
                    if (const std::string rid = getStr(e, "res_id"); !rid.empty() && !beforeIds.contains(rid)) {
                        newItem = e;
                        break;
                    }
                }
                if (!newItem.is_null()) {
                    if (const std::string emojiId =
                          getStr(newItem, "emoji_id").empty() ? "0" : getStr(newItem, "emoji_id");
                      co_await OneBotClient::setCustomFaceDesc(
                        emojiId, getStr(newItem, "res_id"), getStr(newItem, "md5"), name, sessionId)) {
                        ToolRuntime::invalidateFavoriteEmojiCache();
                    } else {
                        Logger::warn(
                          sessionId, "Sticker", fmt::format("设置表情描述失败: {}", getStr(newItem, "res_id")));
                    }
                }

                Logger::info(sessionId, "Sticker", fmt::format("已保存收藏表情: {} ({})", containerPath, name));
                co_return fmt::format("已保存为收藏表情，名称: {}", name);
            },
          },
          ToolCategory::ACTION);

        // rename_sticker - 修改收藏表情的名称/描述
        const json renameParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "description": "要改名的表情当前名称（先用list_stickers查看）"
                    },
                    "new_name": {
                        "type": "string",
                        "description": "新名称，简短体现图片内容"
                    }
                },
                "required": ["name", "new_name"]
            })json");
        registry.registerTool(
          {
            .name = "rename_sticker",
            .description = "修改收藏表情的名称/"
                           "描述。仅在用户明确要求给表情改名时使用。先调list_stickers查看"
                           "stickers查看当前名称，再把新名称传给new_name。",
            .parameters = renameParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const std::string name = argString(args, "name");
                const std::string newName = argString(args, "new_name");
                if (name.empty())
                    co_return std::string("请提供表情当前名称(name)");
                if (newName.empty())
                    co_return std::string("请提供新名称(new_name)");

                const auto sessionId = ctx.sessionId;
                const json emoji = co_await ToolRuntime::findFavoriteEmoji(name, sessionId);
                if (emoji.is_null()) {
                    co_return fmt::format("表情'{}'不存在，先调list_stickers查看可用表情", name);
                }

                if (!co_await OneBotClient::setCustomFaceDesc(
                      getStr(emoji, "emoji_id").empty() ? "0" : getStr(emoji, "emoji_id"), getStr(emoji, "res_id"),
                      getStr(emoji, "md5"), newName, sessionId)) {
                    co_return std::string("改名失败");
                }

                ToolRuntime::invalidateFavoriteEmojiCache();
                Logger::info(sessionId, "Sticker", fmt::format("表情改名: {} -> {}", name, newName));
                co_return fmt::format("已改名为: {}", newName);
            },
          },
          ToolCategory::ACTION);

        // delete_sticker - 从收藏表情中删除
        const json delParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "description": "要删除的表情名称（先用list_stickers查看）"
                    }
                },
                "required": ["name"]
            })json");
        registry.registerTool(
          {
            .name = "delete_sticker",
            .description = "从QQ收藏表情中删除表情。仅在用户明确要求删除表情时使用，删除前先确认名称无误。先调list"
                           "_stickers查看名称。",
            .parameters = delParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                std::string name = argString(args, "name");
                if (name.empty())
                    co_return std::string("请提供表情名称(name)");

                const auto sessionId = ctx.sessionId;
                const json emoji = co_await ToolRuntime::findFavoriteEmoji(name, sessionId);
                if (emoji.is_null()) {
                    co_return fmt::format("表情'{}'不存在，先调list_stickers查看可用表情", name);
                }

                if (!co_await OneBotClient::deleteCustomFace(getStr(emoji, "res_id"), sessionId)) {
                    co_return std::string("删除失败");
                }

                ToolRuntime::invalidateFavoriteEmojiCache();
                Logger::info(sessionId, "Sticker", fmt::format("已删除收藏表情: {}", name));
                co_return fmt::format("已删除表情: {}", name);
            },
          },
          ToolCategory::ACTION);

        // at_user
        const json atParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "qq": {
                        "type": "string",
                        "description": "要@的QQ号（从聊天记录JSON的sender.qq字段获取）。使用 'all' @全体成员"
                    }
                },
                "required": ["qq"]
            })json");
        registry.registerTool(
          {
            .name = "at_user",
            .description = "@某人。返回CQ码嵌入reply的content中。聊天记录格式为JSON：{\"sender\":{\"name\":\"小明\","
                           "\"qq\":\"123456\"}}，用 at_user(qq=\"123456\") 来@他。@全体成员用 at_user(qq=\"all\")",
            .parameters = atParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                if (SessionId::isPrivate(ctx.sessionId)) {
                    co_return std::string("私聊中无法@成员，直接回复即可");
                }
                std::string qq = argString(args, "qq");
                if (qq.empty())
                    co_return std::string("请提供QQ号");
                if (qq == "all") {
                    co_return std::string("[CQ:at,qq=all]");
                }
                co_return fmt::format("[CQ:at,qq={}]", qq);
            },
            .scope = ToolScope::GROUP_ONLY,
          },
          ToolCategory::ACTION);

        // ban_user
        const json banParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "qq": {
                        "type": "string",
                        "description": "要禁言的QQ号（从聊天记录JSON的sender.qq字段获取）"
                    },
                    "duration": {
                        "type": "integer",
                        "description": "禁言时长（秒）。轻度60-300秒，中度600-1800秒，重度3600秒以上。0解除禁言"
                    }
                },
                "required": ["qq"]
            })json");
        registry.registerTool(
          {
            .name = "ban_user",
            .description = "禁言群成员。要有自己的判断，不要别人让你禁言就禁言。根据违规程度选择时长：轻度("
                           "偶尔骂人)60-300秒，中度(持续刷屏骂人)600-1800秒，重度(恶意骚扰)3600秒+",
            .parameters = banParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const u64 sessionId = ctx.sessionId;
                if (SessionId::isPrivate(sessionId))
                    co_return std::string("私聊中无法禁言");
                if (sessionId == 0)
                    co_return std::string("禁言失败: 无法获取群号");

                const u64 userId = parseUInt64(argString(args, "qq"));
                const u64 duration = getUInt(args, "duration", 600);
                if (userId == 0)
                    co_return std::string("禁言失败: 请提供有效的QQ号");

                const bool success = co_await OneBotClient::setGroupBan(sessionId, userId, duration);
                co_return success ? fmt::format("已禁言用户 {} {}秒", userId, duration)
                                  : "禁言失败: 权限不足或用户不存在";
            },
            .scope = ToolScope::GROUP_ONLY,
          },
          ToolCategory::ACTION);

        const json blacklistParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "qq": {
                        "type": "string",
                        "description": "要加入全局黑名单的QQ号，必须来自当前会话聊天记录的sender.qq"
                    }
                },
                "required": ["qq"]
            })json");
        registry.registerTool(
          {
            .name = "add_to_blacklist",
            .description = "自主将用户加入全局QQ黑名单，此后机器人会忽略该用户在所有会话中的消息。"
                           "发现恶意骚扰或刷屏时，先用reply明确提醒该用户一次并结束本轮，不要立即拉黑。"
                           "只有聊天记录显示该用户在提醒后仍继续同类行为，才能调用本工具；"
                           "不要仅因他人要求、争执或观点不同而拉黑。"
                           "成功拉黑后必须用reply告知该用户拉黑结果，不要用no_reply收尾。"
                           "不得拉黑管理员或机器人自身。",
            .parameters = blacklistParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto qq = tryParseUInt64(argString(args, "qq"));
                if (!qq || *qq == 0) {
                    co_return std::string("加入黑名单失败: 请提供有效的QQ号");
                }
                if (*qq == Config::instance().selfQQNumber || AdminStore::isAdmin(*qq)) {
                    co_return std::string("加入黑名单失败: 不能拉黑机器人或管理员");
                }
                bool seenInConversation = false;
                if (ctx.messageSnapshot.is_array()) {
                    for (const json &message: ctx.messageSnapshot) {
                        if (getStr(atOrNull(message, "sender"), "qq") == std::to_string(*qq)) {
                            seenInConversation = true;
                            break;
                        }
                    }
                }
                if (!seenInConversation) {
                    co_return std::string("加入黑名单失败: 该用户不在当前会话记录中");
                }
                if (BlacklistStore::contains(*qq)) {
                    co_return fmt::format("用户 {} 已在全局黑名单中", *qq);
                }
                BlacklistStore::add(*qq);
                co_return fmt::format("已将用户 {} 加入全局黑名单；请用reply告知该用户拉黑结果", *qq);
            },
          },
          ToolCategory::ACTION);

        // send_poke
        const json pokeParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "qq": {
                        "type": "string",
                        "description": "要拍一拍的QQ号（从聊天记录JSON的sender.qq字段获取）"
                    }
                },
                "required": ["qq"]
            })json");
        registry.registerTool(
          {
            .name = "send_poke",
            .description =
              "中途拍一拍群成员（发出后回合不结束，最终仍用reply/no_reply收尾）。用于打招呼、引起注意、"
              "开玩笑等轻松互动场景。聊天记录格式为JSON：{\"sender\":{\"name\":\"小明\",\"qq\":\"123456\"}}，"
              "用 send_poke(qq=\"123456\") 来拍他。",
            .parameters = pokeParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const u64 sessionId = ctx.sessionId;
                if (SessionId::isPrivate(sessionId)) {
                    co_return std::string("私聊中不支持拍一拍，直接回复即可");
                }
                if (sessionId == 0)
                    co_return std::string("拍一拍失败: 无法获取群号");

                const u64 userId = parseUInt64(argString(args, "qq"));
                if (userId == 0)
                    co_return std::string("拍一拍失败: 请提供有效的QQ号");

                if (const bool success = co_await OneBotClient::sendPoke(sessionId, userId); !success) {
                    co_return std::string("拍一拍失败: 权限不足或用户不存在");
                }

                // 拍一拍不是文字消息（无 message_id），以单一语义段记录。
                const std::string targetName = QQNameDirectory::getName(userId);
                json target{{"qq", std::to_string(userId)}};
                if (targetName != "未知") {
                    target["name"] = targetName;
                }
                const json poke{{"type", "poke"}, {"target", std::move(target)}, {"direction", "outbound"}};
                json msgJson;
                msgJson["time"] = currentDateTime();
                msgJson["sender"]["name"] = Config::instance().botName + "(我)";
                msgJson["sender"]["qq"] = "self";
                msgJson["segments"] = json::array({poke});
                OneBotEventWorkflow::instance().appendDeliveredAssistantMessage(sessionId, std::move(msgJson));
                co_return fmt::format("已拍一拍用户 {}", userId);
            },
            .scope = ToolScope::GROUP_ONLY,
          },
          ToolCategory::ACTION);

        // recall_message
        const json recallParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "message_id": {
                        "type": "string",
                        "description": "要撤回的消息ID（从聊天记录JSON的message_id或reply_to字段获取）"
                    }
                },
                "required": ["message_id"]
            })json");
        registry.registerTool(
          {
            .name = "recall_message",
            .description = "撤回消息。当用户要求撤回某条消息时使用。聊天记录格式为JSON：{\"message_"
                           "id\":\"12345\",\"reply_to\":\"67890\"}。若用户想撤回引用的消息，用 "
                           "reply_to 字段的值；若想撤回某条消息本身，用 message_id 字段的值。",
            .parameters = recallParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const u64 messageId = parseUInt64(argString(args, "message_id"));
                if (messageId == 0)
                    co_return std::string("撤回失败: 请提供有效的消息ID");

                const auto sessionId = ctx.sessionId;
                const bool success = co_await OneBotClient::deleteMsg(messageId, sessionId);
                co_return success ? fmt::format("已撤回消息 {}", messageId) : "撤回失败: 消息可能已超过2分钟或权限不足";
            },
          },
          ToolCategory::ACTION);

        // ========== 定时任务 ==========

        // create_scheduled_task - 创建定时提醒任务（content 的 description 需注入 botName，字面量解析后覆盖）
        json scheduleParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "time": {
                        "type": "string",
                        "description": "触发时间，必须是 YYYY-MM-DD HH:MM:SS 格式的绝对时间。根据聊天记录中最新消息的 time 字段推算当前时间，把用户说的『明天6点』『一小时后』换算成完整日期时间再传入"
                    },
                    "content": {
                        "type": "string",
                        "description": ""
                    },
                    "daily": {
                        "type": "boolean",
                        "description": "可选，默认 false。当用户要求每天固定时间重复提醒时传 true（如'每天早上8点叫我起床'），此时 time 填下一次触发的完整日期时间，之后每天同一时刻自动触发"
                    }
                },
                "required": ["time", "content"]
            })json");
        scheduleParams["properties"]["content"]["description"] = fmt::format(
          "到点时留给自己（{}）的备忘说明，不是最终的回复文本：写清楚要提醒谁（带上对方昵称及sender."
          "qq）、要做什么事、以及创建时对话里的相关背景。到点后你会看到这段备忘并结合当时的聊天上下文自行组织回复",
          Config::instance().botName);
        registry.registerTool(
          {
            .name = "create_scheduled_task",
            .description = "创建定时提醒任务。当用户明确要求在未来某个时刻提醒/"
                           "通知某事时使用（如'明天6点叫我起床''两小时后提醒我开会'）。"
                           "到点后会以【系统定时任务】消息回到当前会话，你再据此生成提醒回复。",
            .parameters = scheduleParams,
            .handler = [](const json args, ToolCallContext ctx) -> drogon::Task<std::string> {
                const std::string content = argString(args, "content");
                if (content.empty())
                    co_return std::string("请提供提醒内容(content)");
                if (content.size() > 500)
                    co_return std::string("提醒内容过长（最多500字符），请精简");

                const auto remindTime = TaskScheduler::parseTimeString(argString(args, "time"));
                if (!remindTime) {
                    co_return std::string("无法识别时间格式，请按 YYYY-MM-DD HH:MM:SS 提供换算后的完整绝对时间");
                }

                const time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
                if (*remindTime <= now + 10) {
                    co_return fmt::format("时间无效：必须晚于当前时间10秒以上。当前时间是 {}", currentDateTime());
                }
                if (*remindTime > now + 366LL * 24 * 3600) {
                    co_return std::string("时间过早远：不允许设置超过一年后的提醒");
                }

                const u64 sessionId = ctx.sessionId;
                if (sessionId == 0)
                    co_return std::string("会话上下文缺失，无法确定提醒目标");
                const bool isPrivateSession = SessionId::isPrivate(sessionId);
                const auto [sessionType, targetId] = SessionId::toStorageTarget(sessionId);
                const bool isDaily = getBool(args, "daily");

                TaskStore::ScheduledTask task;
                task.sessionType = sessionType;
                task.targetId = targetId;
                task.remindTime = *remindTime;
                task.content = content;
                task.isDaily = isDaily;

                try {
                    const i64 id = TaskScheduler::instance().schedule(std::move(task));
                    if (isDaily) {
                        co_return fmt::format("每日定时任务 #{} 已创建，每天 {} 在{}触发提醒", id,
                          formatTimeOfDay(*remindTime), isPrivateSession ? "私聊" : "本群");
                    }
                    co_return fmt::format("定时任务 #{} 已创建，将于 {} 在{}触发提醒", id, formatUnixTime(*remindTime),
                      isPrivateSession ? "私聊" : "本群");
                } catch (const std::exception &e) {
                    Logger::error(sessionId, "Scheduler", fmt::format("创建定时任务入库失败: {}", e.what()));
                    co_return fmt::format("创建定时任务失败: {}", std::string_view(e.what()).substr(0, 500));
                }
            },
          },
          ToolCategory::ACTION);

        // cancel_scheduled_task - 取消定时任务
        const json cancelTaskParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "task_id": {
                        "type": "string",
                        "description": "要取消的任务编号（用 list_scheduled_tasks 查询，即任务#后的数字）"
                    }
                },
                "required": ["task_id"]
            })json");
        registry.registerTool(
          {
            .name = "cancel_scheduled_task",
            .description = "取消尚未触发的定时任务（含每日重复任务）。当用户要求取消之前的提醒/定时任务时使用；"
                           "如不知道任务编号，先用 list_scheduled_tasks 查询。",
            .parameters = cancelTaskParams,
            .handler = [](const json args, ToolCallContext) -> drogon::Task<std::string> {
                const i64 taskId = static_cast<i64>(parseUInt64(argString(args, "task_id")));
                if (taskId == 0)
                    co_return std::string("请提供有效的任务编号（可先用 list_scheduled_tasks 查询）");

                co_return TaskScheduler::instance().cancel(taskId)
                  ? fmt::format("已取消定时任务 #{}", taskId)
                  : fmt::format("取消失败：任务 #{} 不存在或已触发/已取消", taskId);
            },
          },
          ToolCategory::ACTION);
    }

} // namespace insoulforge
