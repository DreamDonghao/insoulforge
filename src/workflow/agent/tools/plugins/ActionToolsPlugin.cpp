/// @file ActionToolsPlugin.cpp
/// @brief 动作工具插件实现（ACTION，执行操作、产生副作用）

#include <set>
#include <stdexcept>
#include <vector>

#include <regex>

#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/image/Image.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <onebot/messaging/MessageFormatter.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/EventWorkflow.hpp>
#include <workflow/access/AdminStore.hpp>
#include <workflow/access/BlacklistStore.hpp>
#include <workflow/agent/ability/AsyncTaskManager.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/agent/tools/ToolArgument.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>
#include <workflow/agent/tools/image/CharacterImageStore.hpp>
#include <workflow/agent/tools/image/ImageGenerationService.hpp>
#include <workflow/agent/tools/image/ImageReferences.hpp>
#include <workflow/agent/tools/plugins/ActionToolsPlugin.hpp>
#include <workflow/history/ChatRecordStore.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/message/MessageService.hpp>
#include <workflow/message/image/ImageDescriptionService.hpp>
#include <workflow/scheduling/TaskScheduler.hpp>
#include <workflow/session/QQNameDirectory.hpp>
#include <workflow/session/SessionStore.hpp>

namespace insoulforge {
    namespace {
        auto messageTarget(const json &args, const SessionId &currentSession) -> std::expected<SessionId, std::string> {
            const bool privateTarget = args.contains("user_id");
            if (privateTarget && args.contains("group_id"))
                return std::unexpected("group_id 和 user_id 不能同时指定，请只选择一个发送目标");
            if (!privateTarget && !args.contains("group_id"))
                return currentSession;
            std::optional<SessionId> target;
            if (privateTarget) {
                const auto id = userIdArgument(args);
                if (!id)
                    return std::unexpected(id.error());
                if (BlacklistStore::contains(*id))
                    return std::unexpected(fmt::format("目标私聊 {} 在全局黑名单中，未发送消息", *id));
                target = SessionId::privateChat(*id);
            } else {
                const auto id = groupIdArgument(args);
                if (!id)
                    return std::unexpected(id.error());
                target = *id;
            }
            if (!SessionStore::isSessionEnabled(*target))
                return std::unexpected(fmt::format("目标会话 {} 未在后台启用，未发送消息", *target));
            return *target;
        }

        auto messageDestination(const json &args, const SessionId &target) -> std::string {
            if (args.contains("user_id"))
                return fmt::format("目标私聊 {}：", target.targetId());
            if (args.contains("group_id"))
                return fmt::format("目标群 {}：", target.targetId());
            return {};
        }

        auto requestedImageSize(const json &args) -> std::optional<std::string> {
            auto size = trim(argString(args, "size"));
            if (size.empty()) {
                size = "2048x2048";
            }
            static const std::regex sizePattern{R"([1-9][0-9]{0,4}x[1-9][0-9]{0,4})"};
            return std::regex_match(size, sizePattern) ? std::optional{std::move(size)} : std::nullopt;
        }

        auto startImageTask(const SessionId &sessionId, std::string prompt, std::string size,
          std::vector<std::string> referenceUrls, std::optional<std::string> characterDataUrl) -> std::string {
            std::vector<Image> referenceHandles;
            for (const auto &source: referenceUrls) {
                if (auto image = Image::find(source); image)
                    referenceHandles.push_back(std::move(*image));
            }
            const auto [status, taskId] = AsyncTaskManager::instance().start(sessionId, "生成图片",
              [sessionId, prompt = std::move(prompt), size = std::move(size), referenceUrls = std::move(referenceUrls),
                characterDataUrl = std::move(characterDataUrl),
                referenceHandles = std::move(referenceHandles)]() mutable -> drogon::Task<AsyncTaskManager::Result> {
                  const bool useSelfReference = characterDataUrl.has_value();
                  std::vector<std::string> references;
                  if (characterDataUrl)
                      references.push_back(std::move(*characterDataUrl));
                  for (const auto &url: referenceUrls) {
                      auto dataUrl = co_await ImageDescriptionService::referenceDataUrl(url, sessionId);
                      if (!dataUrl) {
                          throw std::runtime_error("原图下载失败或格式不受支持，请重新发送静态图片");
                      }
                      references.push_back(std::move(*dataUrl));
                  }
                  std::string generationPrompt = prompt;
                  if (useSelfReference) {
                      generationPrompt =
                        "第一张参考图是你的角色形象图，请以它为人物形象依据，保持角色可辨认的外观特征。"
                        "除非画面要求明确提出修改，否则发型、发色、耳朵、服饰等以参考图为准，"
                        "不要根据文字重新设计角色。其余参考图按用户要求使用。请按以下画面要求生成：" +
                        prompt;
                  }
                  auto generated = co_await ImageGenerationService::generate(
                    std::move(generationPrompt), size, sessionId, std::move(references));
                  std::string description = "依据生成提示词：" + prompt;
                  try {
                      if (const auto recognized =
                            co_await ImageDescriptionService::describeGeneratedImage(generated.source, sessionId)) {
                          description = recognized->description;
                      }
                  } catch (const std::exception &error) {
                      Logger::warn(sessionId, "ImageGeneration", fmt::format("生成图片识别失败: {}", error.what()));
                  }
                  co_return AsyncTaskManager::Result{.content = std::move(generated.message),
                    .image = std::move(generated.image),
                    .imageDescription = std::move(description)};
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
    ActionToolsPlugin::ActionToolsPlugin(const std::string_view botName) : m_botName(botName) {}

    void ActionToolsPlugin::registerTools(ToolRegistry &registry) const {

        registry.registerTool(
          {
            .name = "send_qzone_msg",
            .description = "在机器人自己的 QQ 空间发布说说，在遇到高兴，离谱，无语的事的时候发送，不是发送聊天消息。"
                           "支持文字和图片 URL，默认所有人可见。"
                           "超时或无法确认结果时不要自动重试，以免重复发布。",
            .parameters = json{{"type", "object"},
              {"properties", {{"content", {{"type", "string"}, {"description", "说说正文，不能为空"}}},
                               {"images", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 9},
                                            {"description", "可选，最多 9 张图片的 HTTP/HTTPS URL；不要编造地址"}}},
                               {"ugc_right", {{"type", "integer"}, {"enum", {1, 4, 64}}, {"default", 1},
                                               {"description", "查看权限：1 所有人可见，4 好友可见，64 仅自己可见"}}}}},
              {"required", {"content"}}},
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto content = trim(argString(args, "content"));
                if (content.empty()) {
                    co_return std::string("请提供非空的说说正文。");
                }
                const auto &right = atOrNull(args, "ugc_right");
                if (!right.is_null() && (!right.is_number_integer() || (right != 1 && right != 4 && right != 64))) {
                    co_return std::string("ugc_right 必须为 1（所有人）、4（好友）或 64（仅自己）。");
                }
                json params = {{"content", content}, {"ugc_right", right.is_null() ? json(1) : right}};
                if (const auto &images = atOrNull(args, "images"); !images.is_null()) {
                    if (!images.is_array() || images.size() > 9) {
                        co_return std::string("images 必须为数组，最多 9 张图片。");
                    }
                    for (const auto &image: images) {
                        if (!image.is_string()) {
                            co_return std::string("图片地址必须为 HTTP/HTTPS URL。");
                        }
                        const auto &url = image.get_ref<const std::string &>();
                        if (const auto hostStart = url.find("://") + 3;
                          (!url.starts_with("http://") && !url.starts_with("https://")) ||
                          url.find_first_of(" \t\r\n\\") != std::string::npos ||
                          url.find_first_of("/?#", hostStart) == hostStart || url.size() == hostStart) {
                            co_return std::string("图片地址必须为包含主机名的 HTTP/HTTPS URL。");
                        }
                    }
                    params["images"] = images;
                }
                const auto result = co_await OneBotClient::sendQzoneMsg(std::move(params), ctx.requireSession());
                if (!result) {
                    co_return "空间说说发布未成功确认：" + result.error() + "。若请求超时，请先检查空间，勿自动重试。";
                }
                co_return fmt::format("空间说说已发布，tid={}。", *result);
            },
          },
          ToolCategory::ACTION);

        registry.registerTool(
          {
            .name = "generate_image",
            .description = "根据用户明确提出的画面要求异步生成一张图片。工具立即返回任务状态，图片生成完成后"
                           "会自动发送到当前会话，不要再调用 send_message_and_finish 发送图片。绘制你自己的角色形象时，"
                           "将 use_self_reference 设为 true，以后台上传的角色图为准；prompt 只描述用户要求的"
                           "场景、动作、表情等变化，不重复编造参考图中已有的外观设定。其他画面不要设置此参数。"
                           "修改或参考聊天图片时，用 reference_images 按顺序指定当前可见会话图片的 image_id，或旧的"
                           "message_id 和 image_index，不要传入或猜测图片 URL。可同时使用角色形象图和聊天参考图。"
                           "一次会话同时只能生成一张。",
            .parameters = json{{"type", "object"},
              {"properties",
                {{"prompt", {{"type", "string"},
                              {"description", "画面要求；使用角色参考图时只写场景、动作等变化，不重复描述原有外观"}}},
                  {"size",
                    {{"type", "string"}, {"description", "图片尺寸，格式为宽x高，如 1024x1024；默认 2048x2048"}}},
                  {"use_self_reference",
                    {{"type", "boolean"},
                      {"description", "画面主体是你自己时设为 true，引用后台上传的角色形象图；默认 false"}}},
                  {"reference_images", ImageReferences::schema()}}},
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
                std::vector<std::string> referenceUrls;
                if (const auto &images = atOrNull(args, "reference_images"); args.contains("reference_images")) {
                    auto resolved = ImageReferences::resolve(images, ctx);
                    if (!resolved)
                        co_return resolved.error();
                    referenceUrls = std::move(*resolved);
                }
                co_return startImageTask(
                  ctx.requireSession(), prompt, *size, std::move(referenceUrls), std::move(referenceDataUrl));
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
                  ctx.requireSession(), "演示延时任务", []() -> drogon::Task<AsyncTaskManager::Result> {
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
            .description = "获取QQ原生表情的CQ码。返回的CQ码必须复制到发送工具的content中，非必要不使用",
            .parameters = faceParams,
            .handler = [](const json args, ToolCallContext) -> drogon::Task<std::string> {
                const i32 id = getInt(args, "id", 1);
                co_return fmt::format("[CQ:face,id={}]", id);
            },
          },
          ToolCategory::INFORMATION);

        // send_image
        const json imageParams = {{"type", "object"},
          {"properties", {{"image_id", {{"type", "string"}, {"description", "当前可见会话中的图片 ID"}}}}},
          {"required", {"image_id"}}, {"additionalProperties", false}};
        registry.registerTool(
          {.name = "send_image",
            .description = "通过 image_id 将图片直接发送到当前会话，无需在 send_message_and_finish 中复制 CQ 码。",
            .parameters = imageParams,
            .handler = [](json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto id = argString(args, "image_id");
                if (id.empty() || args.contains("url"))
                    co_return "请仅提供当前可见会话中的 image_id。";
                const auto resolved = ImageReferences::resolve(json::array({json{{"image_id", id}}}), ctx);
                if (!resolved)
                    co_return resolved.error();
                const auto image = co_await Image::load(resolved->front());
                if (!image)
                    co_return image.error();
                const auto encoded = image->base64();
                if (!encoded)
                    co_return encoded.error();
                std::string description;
                const auto reuseDescription = [&](const json &entry) {
                    if (const auto status = getStr(entry, "recognition_status");
                      description.empty() && getStr(entry, "image_id") == id &&
                      (status == "succeeded" || status == "generated"))
                        description = trim(getStr(entry, "description"));
                };
                for (const auto &record: ctx.messageSnapshot) {
                    if (const auto session = getStr(record, "session_id");
                      !session.empty() && SessionId::parse(session) != ctx.sessionId)
                        continue;
                    const auto &assets = atOrNull(atOrNull(record, "assets"), "images");
                    if (assets.is_array())
                        for (const auto &asset: assets)
                            reuseDescription(asset);
                    if (const auto &segments = atOrNull(record, "segments"); segments.is_array())
                        for (const auto &segment: segments)
                            if (getStr(segment, "type") == "image")
                                reuseDescription(segment);
                }
                if (description.empty()) {
                    try {
                        if (const auto recognized = co_await ImageDescriptionService::describeGeneratedImage(
                              image->id(), ctx.requireSession()))
                            description = trim(recognized->description);
                    } catch (const std::exception &error) {
                        Logger::warn(ctx.requireSession(), "Media", fmt::format("发送图片识别失败: {}", error.what()));
                    } catch (...) {
                        Logger::warn(ctx.requireSession(), "Media", "发送图片识别失败：未知异常");
                    }
                }
                if (description.empty())
                    description = "图片描述不可用";
                const auto sent = co_await MessageService::sendToSession(
                  ctx.requireSession(), "[CQ:image,file=base64://" + *encoded + "]", description, {}, image->id());
                co_return sent ? "图片已发送。" : "图片发送失败：" + sent.error();
            }},
          ToolCategory::ACTION);

        // send_sticker - 直接把表情包作为独立消息发出（经 MessageService，发送后记入聊天记录）
        const json stickerParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "name": {
                        "type": "string",
                        "description": "表情名称（参见系统消息 available_stickers）"
                    },
                    "group_id": {
                        "type": "string",
                        "description": "可选目标群号，仅可发送到后台已启用的群；与 user_id 互斥"
                    },
                    "user_id": {
                        "type": "string",
                        "description": "可选目标私聊用户 ID，仅可发送到后台已启用的私聊；与 group_id 互斥。两者均省略则发送到当前会话"
                    }
                },
                "required": ["name"]
            })json");
        registry.registerTool(
          {
            .name = "send_sticker",
            .description =
              "中途发送一张QQ收藏表情包（独立消息直接发出，不经消息发送工具；发出后回合不结束，"
              "最终仍用send_message_and_finish/finish_turn收尾）。从系统消息 available_stickers 选择表情名。"
              "可选group_id指定目标群，或user_id指定私聊用户 ID，不能同时填写。目标必须在后台启用，"
              "不受跨会话共享开关限制；均省略则发送到当前会话。"
              "发送失败或超时由模型决定重发、继续处理或结束；超时不代表确定未送达。",
            .parameters = stickerParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto connection = Config::instance().oneBotSnapshot();
                std::string name = argString(args, "name");
                if (name.empty()) {
                    co_return std::string("请提供表情名称");
                }
                const auto target = messageTarget(args, ctx.requireSession());
                if (!target)
                    co_return target.error();
                const auto destination = messageDestination(args, *target);
                const json emoji = co_await ToolRuntime::findFavoriteEmoji(name, ctx.requireSession(), connection);
                if (emoji.is_null()) {
                    co_return fmt::format(
                      "{}表情'{}'不存在、名称不唯一或列表获取失败，请核对系统消息 available_stickers 中的名称",
                      destination, name);
                }

                const auto cqCode = MessageFormatter::favoriteEmojiToCQCode(emoji);
                if (!cqCode)
                    co_return fmt::format("{}表情'{}'{}", destination, name, cqCode.error());

                if (connection != Config::instance().oneBotSnapshot())
                    co_return std::string("OneBot 配置已切换，请刷新收藏列表后重试");
                // 消息由 MessageService 记入目标会话，当前调用方通过工具结果获知发送状态。
                if (const auto messageId =
                      co_await MessageService::sendToSession(*target, *cqCode, std::nullopt, {}, {}, connection);
                  !messageId) {
                    co_return fmt::format(
                      "{}表情发送未确认成功：{}。由模型决定重发、继续处理或结束；超时不代表确定未送达。", destination,
                      messageId.error());
                }
                co_return fmt::format("{}已发送表情「{}」", destination, name);
            },
          },
          ToolCategory::ACTION);

        // send_message - 发送消息后继续处理，最终由 send_message_and_finish/finish_turn 收尾
        const json continueParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "content": {
                        "type": "string",
                        "description": "要发送的消息内容"
                    },
                    "group_id": {
                        "type": "string",
                        "description": "可选目标群号，仅可发送到后台已启用的群；与 user_id 互斥"
                    },
                    "user_id": {
                        "type": "string",
                        "description": "可选目标私聊用户 ID，仅可发送到后台已启用的私聊；与 group_id 互斥。两者均省略则发送到当前会话"
                    }
                },
                "required": ["content"]
            })json");
        registry.registerTool(
          {
            .name = "send_message",
            .description = "发送一条消息并继续本轮处理，用于过程通知、连续发送或跨会话发送。"
                           "发送后即可结束时，使用 send_message_and_finish。"
                           "目标默认为当前会话；group_id 与 user_id 互斥，指定目标须在后台启用，不受共享开关限制。"
                           "quote_message_id 可选，仅支持引用当前会话可见消息。"
                           "返回发送结果；失败或结果未知时本轮不结束，由模型决定重发、继续处理或 finish_turn。"
                           "超时不代表确定未送达。",
            .parameters = [&] -> json {
                auto params = continueParams;
                params["properties"]["quote_message_id"] = {
                  {"type", "string"}, {"description", "可选，当前目标为本会话时可引用可见消息的 message_id"}};
                return params;
            }(),
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                std::string content = ExecutorAgent::cleanReplyContent(argString(args, "content"));
                if (content.empty()) {
                    co_return std::string("请提供要发送的消息内容");
                }

                // 与 send_sticker 相同：经 MessageService 发送，成功后记入会话列表并推送 WebSocket
                const auto target = messageTarget(args, ctx.requireSession());
                if (!target)
                    co_return target.error();
                const auto quote = argString(args, "quote_message_id");
                if (!quote.empty()) {
                    bool found = false;
                    if (*target == ctx.requireSession())
                        for (const auto &record: ctx.messageSnapshot)
                            if (getStr(record, "message_id") == quote)
                                found = true;
                    if (!found)
                        co_return std::string("引用目标必须来自当前会话可见消息");
                    content = "[CQ:reply,id=" + quote + "]" + content;
                }
                const auto destination = messageDestination(args, *target);
                if (const auto messageId = co_await MessageService::sendToSession(*target, content); !messageId) {
                    co_return fmt::format(
                      "{}消息发送未确认成功：{}"
                      "。请继续处理，不要认为该消息已送达，由模型决定重发、继续处理或结束；超时不代表确定未送达。",
                      destination, messageId.error());
                }
                co_return destination + "消息已发送，本轮继续";
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
                const auto connection = Config::instance().oneBotSnapshot();
                const auto sessionId = ctx.requireSession();
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
                if (getStr(atOrNull(record, "sender"), "user_id") == "self") {
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
                if (const auto path = co_await OneBotClient::getImage(file, sessionId, connection)) {
                    containerPath = *path;
                }

                // 获取路径失败时，通过 URL 将图片下载到容器内。
                if (containerPath.empty()) {
                    if (url.empty()) {
                        co_return std::string("获取图片失败，请确认图片仍可访问");
                    }
                    if (const auto path = co_await OneBotClient::downloadFile(url, sessionId, connection)) {
                        containerPath = *path;
                    }
                    if (containerPath.empty()) {
                        co_return std::string("获取图片失败，可能是图片链接已过期，请让对方重新发送后立即保存");
                    }
                }

                // 记录已有表情的 res_id，以便保存后找出新表情。
                ToolRuntime::invalidateFavoriteEmojiCache();
                std::set<std::string> beforeIds;
                for (const auto beforeEmojis = co_await ToolRuntime::fetchFavoriteEmojis(sessionId, connection);
                  const auto &e: beforeEmojis) {
                    if (const std::string rid = getStr(e, "res_id"); !rid.empty()) {
                        beforeIds.insert(rid);
                    }
                }

                // 将图片保存为 QQ 收藏表情。
                const bool added = co_await OneBotClient::addCustomFace(containerPath, sessionId, connection);
                ToolRuntime::invalidateFavoriteEmojiCache();
                if (!added)
                    co_return std::string("收藏请求失败或结果未确认，请刷新收藏列表核对");

                // 找到新增的表情并设置描述；查询失败也不能抹掉已经收藏的事实。
                json afterEmojis;
                try {
                    afterEmojis = co_await ToolRuntime::fetchFavoriteEmojis(sessionId, connection);
                } catch (const std::exception &) {
                    co_return std::string("已保存为收藏表情，但未确认名称设置成功，请刷新收藏列表后修改名称");
                }
                json newItem;
                for (const auto &e: afterEmojis) {
                    if (const std::string rid = getStr(e, "res_id"); !rid.empty() && !beforeIds.contains(rid)) {
                        if (!newItem.is_null()) {
                            newItem = nullptr;
                            break;
                        }
                        newItem = e;
                    }
                }
                bool named = false;
                if (!newItem.is_null()) {
                    if (const std::string emojiId =
                          getStr(newItem, "emoji_id").empty() ? "0" : getStr(newItem, "emoji_id");
                      co_await OneBotClient::setCustomFaceDesc(
                        emojiId, getStr(newItem, "res_id"), getStr(newItem, "md5"), name, sessionId, connection)) {
                        named = true;
                    } else {
                        Logger::warn(
                          sessionId, "Sticker", fmt::format("设置表情描述失败: {}", getStr(newItem, "res_id")));
                    }
                }

                ToolRuntime::invalidateFavoriteEmojiCache();
                if (!named)
                    co_return std::string("已保存为收藏表情，但未确认名称设置成功，请刷新收藏列表后修改名称");
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
                        "description": "要改名的表情当前名称（参见系统消息 available_stickers）"
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
                           "描述。仅在用户明确要求给表情改名时使用。从系统消息 available_stickers 选择"
                           "当前名称，再把新名称传给new_name。",
            .parameters = renameParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto connection = Config::instance().oneBotSnapshot();
                const std::string name = argString(args, "name");
                const std::string newName = argString(args, "new_name");
                if (name.empty())
                    co_return std::string("请提供表情当前名称(name)");
                if (newName.empty())
                    co_return std::string("请提供新名称(new_name)");

                const auto &sessionId = ctx.requireSession();
                const json emoji = co_await ToolRuntime::findFavoriteEmoji(name, sessionId, connection);
                if (emoji.is_null()) {
                    co_return fmt::format(
                      "表情'{}'不存在、名称不唯一或列表获取失败，请核对系统消息 available_stickers 中的名称", name);
                }

                const bool renamed = co_await OneBotClient::setCustomFaceDesc(
                  getStr(emoji, "emoji_id").empty() ? "0" : getStr(emoji, "emoji_id"), getStr(emoji, "res_id"),
                  getStr(emoji, "md5"), newName, sessionId, connection);
                ToolRuntime::invalidateFavoriteEmojiCache();
                if (!renamed) {
                    co_return std::string("改名请求失败或结果未确认，请核对收藏列表后再试");
                }

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
                        "description": "要删除的表情名称（参见系统消息 available_stickers）"
                    }
                },
                "required": ["name"]
            })json");
        registry.registerTool(
          {
            .name = "delete_sticker",
            .description = "从QQ收藏表情中删除表情。仅在用户明确要求删除表情时使用，删除前先确认名称无误。从系统消息 "
                           "available_stickers 查看名称；重名时不执行。",
            .parameters = delParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto connection = Config::instance().oneBotSnapshot();
                std::string name = argString(args, "name");
                if (name.empty())
                    co_return std::string("请提供表情名称(name)");

                const auto &sessionId = ctx.requireSession();
                const json emoji = co_await ToolRuntime::findFavoriteEmoji(name, sessionId, connection);
                if (emoji.is_null()) {
                    co_return fmt::format(
                      "表情'{}'不存在、名称不唯一或列表获取失败，请核对系统消息 available_stickers 中的名称", name);
                }

                if (const auto result =
                      co_await ToolRuntime::deleteFavoriteEmoji(getStr(emoji, "res_id"), sessionId, connection);
                  !result) {
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
                    "user_id": {
                        "type": "string",
                        "description": "目标用户 ID，取聊天记录的 sender.uid；self 表示机器人自身，all 表示全体成员。"
                    }
                },
                "required": ["user_id"]
            })json");
        registry.registerTool(
          {
            .name = "at_user",
            .description = "生成群聊 @ 的 CQ 码，不直接发送；将返回值原样放入消息发送工具的 content。",
            .parameters = atParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                if (ctx.requireSession().isPrivate()) {
                    co_return std::string("私聊中无法@成员，直接回复即可");
                }
                std::string userId = argString(args, "user_id");
                if (userId.empty())
                    co_return std::string("请提供用户 ID");
                if (userId == "all") {
                    co_return std::string("[CQ:at,qq=all]");
                }
                if (userId == "self")
                    co_return std::string("[CQ:at,qq=self]");
                const auto id = UserId::parse(userId);
                if (!id)
                    co_return id.error();
                const auto number = id->toU64();
                if (!number || *number == 0)
                    co_return "OneBot 11 要求非零数字用户 ID";
                co_return fmt::format("[CQ:at,qq={}]", *number);
            },
            .scope = ToolScope::GROUP_ONLY,
          },
          ToolCategory::INFORMATION);

        // ban_user
        const json banParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "user_id": {
                        "type": "string",
                        "description": "要禁言的用户 ID（从聊天记录JSON的sender.uid字段获取）"
                    },
                    "duration": {
                        "type": "integer",
                        "description": "禁言时长（秒），默认600，范围0–2592000。轻度60-300秒，中度600-1800秒，重度3600秒以上。0解除禁言"
                    }
                },
                "required": ["user_id"]
            })json");
        registry.registerTool(
          {
            .name = "ban_user",
            .description = "禁言群成员。要有自己的判断，不要别人让你禁言就禁言。根据违规程度选择时长：轻度("
                           "偶尔骂人)60-300秒，中度(持续刷屏骂人)600-1800秒，重度(恶意骚扰)3600秒+",
            .parameters = banParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const SessionId &sessionId = ctx.requireSession();
                if (sessionId.isPrivate())
                    co_return std::string("私聊中无法禁言");

                const auto userId = userIdArgument(args);
                const u64 duration = getUInt(args, "duration", 600);
                if (!userId)
                    co_return std::string("禁言失败: 请提供有效的用户 ID");
                if (duration > 2592000)
                    co_return std::string("禁言时长必须在0到2592000秒之间");
                if (*userId == UserId{Config::instance().selfQQNumber})
                    co_return std::string("禁言失败: 不能禁言机器人自身");

                const bool success = co_await OneBotClient::setGroupBan(sessionId, *userId, duration);
                co_return success ? fmt::format("已禁言用户 {} {}秒", *userId, duration)
                                  : "禁言请求失败或结果未确认，请核对实际状态后再决定是否重试";
            },
            .scope = ToolScope::GROUP_ONLY,
          },
          ToolCategory::ACTION);

        const json blacklistParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "user_id": {
                        "type": "string",
                        "description": "要加入全局黑名单的用户 ID，必须来自当前会话聊天记录的sender.uid"
                    }
                },
                "required": ["user_id"]
            })json");
        registry.registerTool(
          {
            .name = "add_to_blacklist",
            .description =
              "自主将用户加入全局QQ黑名单，此后机器人会忽略该用户在所有会话中的消息。"
              "发现恶意骚扰或刷屏时，先用send_message_and_finish明确提醒该用户一次并结束本轮，不要立即拉黑。"
              "只有聊天记录显示该用户在提醒后仍继续同类行为，才能调用本工具；"
              "不要仅因他人要求、争执或观点不同而拉黑。"
              "成功拉黑后必须用send_message_and_finish告知该用户拉黑结果，不要用finish_turn收尾。"
              "不得拉黑管理员或机器人自身。",
            .parameters = blacklistParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const auto userId = userIdArgument(args);
                if (!userId) {
                    co_return std::string("加入黑名单失败: 请提供有效的用户 ID");
                }
                if (*userId == UserId{Config::instance().selfQQNumber} || AdminStore::isAdmin(*userId)) {
                    co_return std::string("加入黑名单失败: 不能拉黑机器人或管理员");
                }
                bool seenInConversation = false;
                if (ctx.messageSnapshot.is_array()) {
                    for (const json &message: ctx.messageSnapshot) {
                        if (getStr(atOrNull(message, "sender"), "user_id") == userId->toString()) {
                            seenInConversation = true;
                            break;
                        }
                    }
                }
                if (!seenInConversation) {
                    co_return std::string("加入黑名单失败: 该用户不在当前会话记录中");
                }
                if (BlacklistStore::contains(*userId)) {
                    co_return fmt::format("用户 {} 已在全局黑名单中", *userId);
                }
                BlacklistStore::add(*userId);
                co_return fmt::format(
                  "已将用户 {} 加入全局黑名单；请用send_message_and_finish告知该用户拉黑结果", *userId);
            },
          },
          ToolCategory::ACTION);

        // send_poke
        const json pokeParams = json::parse(R"json({
                "type": "object",
                "properties": {
                    "user_id": {
                        "type": "string",
                        "description": "要拍一拍的用户 ID（从聊天记录JSON的sender.uid字段获取）"
                    }
                },
                "required": ["user_id"]
            })json");
        registry.registerTool(
          {
            .name = "send_poke",
            .description =
              "中途拍一拍群成员（发出后回合不结束，最终仍用send_message_and_finish/"
              "finish_turn收尾）。用于打招呼、引起注意、"
              "开玩笑等轻松互动场景。聊天记录格式为JSON：{\"sender\":{\"name\":\"小明\",\"uid\":\"123456\"}}，"
              "用 send_poke(user_id=\"123456\") 来拍他。",
            .parameters = pokeParams,
            .handler = [](const json args, const ToolCallContext ctx) -> drogon::Task<std::string> {
                const SessionId &sessionId = ctx.requireSession();
                if (sessionId.isPrivate()) {
                    co_return std::string("私聊中不支持拍一拍，直接回复即可");
                }

                const auto userId = userIdArgument(args);
                if (!userId)
                    co_return std::string("拍一拍失败: 请提供有效的用户 ID");

                if (const bool success = co_await OneBotClient::sendPoke(sessionId, *userId); !success) {
                    co_return std::string("拍一拍失败: 权限不足或用户不存在");
                }

                // 拍一拍不是文字消息（无 message_id），以单一语义段记录。
                const std::string targetName = QQNameDirectory::getName(*userId);
                json target{{"user_id", userId->toString()}};
                if (targetName != "未知") {
                    target["name"] = targetName;
                }
                const json poke{{"type", "poke"}, {"target", std::move(target)}, {"direction", "outbound"}};
                json msgJson;
                msgJson["time"] = currentDateTime();
                msgJson["sender"]["name"] = Config::instance().botName + "(我)";
                msgJson["sender"]["user_id"] = "self";
                msgJson["segments"] = json::array({poke});
                EventWorkflow::instance().appendDeliveredAssistantMessage(sessionId, std::move(msgJson));
                co_return fmt::format("已拍一拍用户 {}", *userId);
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

                const auto &sessionId = ctx.requireSession();
                const bool success = co_await OneBotClient::deleteMsg(messageId, sessionId);
                co_return success ? fmt::format("已撤回消息 {}", messageId)
                                  : "撤回请求失败或结果未确认，请核对消息状态后再决定是否重试";
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
                        "description": "触发时间，必须是 YYYY-MM-DD HH:MM:SS 格式的绝对时间。以 execution_state.current_time 为当前时间（服务端本地时区），同一时区解析，把用户说的『明天6点』『一小时后』换算成完整日期时间再传入"
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
          "user_id）、要做什么事、以及创建时对话里的相关背景。到点后你会看到这段备忘并结合当时的聊天上下文自行组织回复",
          m_botName);
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
                    co_return std::string("提醒内容过长（最多500 UTF-8字节），请精简");

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

                const SessionId &sessionId = ctx.requireSession();
                const bool isPrivateSession = sessionId.isPrivate();
                const bool isDaily = getBool(args, "daily");

                TaskStore::ScheduledTask task{.sessionId = sessionId};
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
