# 内置工具参考

Executor（`ExecutorAgent`）在单个 Agent 循环中通过工具调用生成回复。所有内置工具经 `ToolRegistry`
的进程内插件注册，按类别拆分在 `src/agent/tools/plugins/`：

| 类别          | 插件实现文件                          | 语义                         |
|---------------|---------------------------------------|------------------------------|
| `REPLY`       | `tools/plugins/ReplyToolsPlugin.cpp`  | 回复工具，调用即结束回合     |
| `INFORMATION` | `tools/plugins/InfoToolsPlugin.cpp`   | 查询数据、获取答案，无副作用 |
| `ACTION`      | `tools/plugins/ActionToolsPlugin.cpp` | 执行操作、产生副作用         |

管理后台“执行配置”中的最大迭代轮数控制整个 Executor 工具循环，配置字段为 `execution.maxToolRounds`，
默认 8、允许 1～100。一次模型请求及其工具处理算一轮，同轮多个工具不增加轮数，生成最终回复也占一轮。
保存后对新回复流程生效；达到上限仍未生成回复时，记录错误并结束，不额外生成兜底回复。

每轮请求在上下文末尾附带当前轮次及后续剩余轮数，执行状态只用于当前请求，不写入聊天记录或后续轮次历史。
最后一轮只开放 REPLY 类的 `reply`、`reply_with_quote`、`no_reply`，不会执行查询或动作工具。
若最大轮数设为 1，第一轮就是仅回复模式。工具结果日志仍保留。

内置三组分别归属 `builtin.reply`、`builtin.info`、`builtin.action` 插件，由 `ToolPluginCatalog`
显式加载；自定义工具（Lua / Python / HTTP）归属 `custom` 插件并统一注册为 `INFORMATION`。同名工具不能跨插件覆盖；刷新自定义工具只会替换
`custom` 的工具。

注入顺序固定为“类别 → `promptOrder` → 工具名”，避免重启或刷新后顺序漂移。私聊注入时会排除仅群聊的 `at_user`、`ban_user`、
`send_poke`；工具本身仍保留会话校验作为防线。

> `REPLY` 类工具的 handler 只是占位：它们在 `ExecutorAgent::processToolCalls` 内被拦截执行（需要改写回复决策而非返回工具结果），不经
> `ToolRegistry::executeTool`。

## REPLY（回复，3 个）

| 工具               | 参数                    | 说明                                                                |
|--------------------|-------------------------|---------------------------------------------------------------------|
| `no_reply`         | 无                      | 决定不回复：话题已参与过、没人问、刚说过话、纯表情刷屏时使用        |
| `reply`            | `content`               | 普通回复，回复文本（可内嵌 CQ 码）                                  |
| `reply_with_quote` | `content`, `message_id` | 引用特定消息回复；`message_id` 取聊天记录 JSON 的 `message_id` 字段 |

## INFORMATION（查询，4 个）

| 工具                   | 参数       | 说明                                                                                                                                                                                                                                                                            |
|------------------------|------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `list_stickers`        | 无         | 列出 QQ 收藏表情名称（`ToolRuntime::fetchFavoriteEmojis`，带缓存）                                                                                                                                                                                                              |
| `recall_memory`        | `query`    | 长期记忆检索：`LongTermMemory::searchMemory` 按 Brute-Force 余弦相似度（阈值 0.3）取 top-3，返回"回忆起：…"或"想不起来"                                                                                                                                                         |
| `deep_think`           | `question` | 深度思考：调用 `executorThinking` 配置的模型求解。上下文由 Executor 经 `ToolCallContext.conversationContext` 传入（system 之后的完整消息列表，含已获取的工具结果），拼上专家求解 system prompt（只产出问题答案，不组织聊天回复）；模型 content 为空时兜底取 `reasoning_content` |
| `list_scheduled_tasks` | 无         | 列出当前会话待触发的定时任务（编号 / 触发时间 / 备忘内容），取消前查询用                                                                                                                                                                                                        |

## ACTION（动作，15 个）

### 过程消息

| 工具                 | 参数      | 说明                                                                                                                                                                                                                                                                      |
|----------------------|-----------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `reply_and_continue` | `content` | 发送一条文字过程消息，**回合不结束**：内容经 `cleanReplyContent` 净化后由 `MessageService` 发送并记入聊天记录，工具结果回传后循环继续。用于耗时操作（如搜索）前告知用户一句简短的话，或在一次回复中先发其他内容、形成多条连续消息；最终回复仍由 `reply` / `no_reply` 收尾 |

### 表情包

| 工具             | 参数                                | 说明                                                                                                                                                                                                                                             |
|------------------|-------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `send_face`      | `id`                                | 生成 QQ 原生表情 CQ 码（返回的 CQ 码需拼进 `reply` 的 content）                                                                                                                                                                                  |
| `send_image`     | `url`                               | 生成网络图片 CQ 码（同上）                                                                                                                                                                                                                       |
| `send_sticker`   | `name`                              | 把收藏表情作为**独立消息**直接发出，无需拼进 reply：商城表情走 `[CQ:mface]`，个人收藏走 `[CQ:image,sub_type=1]`；经 `MessageService` 发送，成功后自动记入聊天记录并推送 WebSocket，后续轮次模型能从记录中看到自己发过这张表情                    |
| `save_sticker`   | `message_id`, `image_index`, `name` | 把用户发的图片存为收藏表情并设置描述名：工具按消息 ID 和从 0 开始的图片索引在当前会话记录中解析图片来源，先 `get_image` 拿容器内路径，失败回退 `download_file`；`add_custom_face` 保存后 diff 保存前后的 `res_id` 集合定位新表情，再设置描述名。 |
| `rename_sticker` | `name`, `new_name`                  | 修改收藏表情的描述名                                                                                                                                                                                                                             |
| `delete_sticker` | `name`                              | 从 QQ 收藏表情中删除                                                                                                                                                                                                                             |

表情包工具依赖 OneBot 收藏表情接口，操作成功后都会 `invalidateFavoriteEmojiCache()` 使缓存失效。需要表情名称的工具都要求先调
`list_stickers` 查看可用名称。

### 用户与群互动

| 工具             | 参数              | 说明                                                                                                                                                                                      |
|------------------|-------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `at_user`        | `qq`              | @某人的 CQ 码（拼进 reply 用），`"all"` 为 @全体成员；私聊中禁用                                                                                                                          |
| `ban_user`       | `qq`, `duration?` | 禁言群成员，默认 600 秒、0 为解禁；描述要求模型自行判断违规程度选时长（轻度 60-300 秒 / 中度 600-1800 秒 / 重度 3600 秒+），不盲从指令                                                    |
| `add_to_blacklist` | `qq`              | 将用户加入全局 QQ 黑名单，后续消息在工作流入口被过滤；工具提示要求先用 `reply` 明确提醒并结束本轮，仅在后续仍继续同类恶意行为时调用，成功后用 `reply` 告知用户；私聊也可用 |
| `send_poke`      | `qq`              | 拍一拍群成员，打招呼、引起注意等轻松互动；私聊中禁用。拍一拍不是文字消息（无 message_id），成功后以 `segments` 中的 `poke` 条目记入聊天记录并推送 WebSocket，后续轮次模型能看到自己拍过谁 |
| `recall_message` | `message_id`      | 撤回消息：撤引用的消息用 `reply_to` 字段值，撤某条消息本身用 `message_id` 字段值                                                                                                          |

拍一拍的接收：OneBot notice 事件（`notice_type=notify, sub_type=poke`）先由 `OneBotEventNormalizer` 归一化，再进入
`OneBotEventWorkflow` 的会话消息预处理队列。禁用会话会提前跳过；已启用会话中，拍一拍保存为独立 `poke` 段，所有拍一拍通知均与普通消息一样由
Router
决策是否回应。群成员入群、退群以 `member_event` 段走同一工作流。

`add_to_blacklist` 只接受当前会话快照中出现过的发送者 QQ 号，拒绝机器人自身和管理员，已在黑名单中的用户不会重复添加。
“先提醒一次”由工具描述引导模型判断；系统没有单独保存警告状态，也不会在工具执行时强制验证先前是否已提醒。
管理员仍可通过命令或后台管理黑名单。

### 定时任务

| 工具                    | 参数                        | 说明                                                                                                                                                                                                                                                                                                                 |
|-------------------------|-----------------------------|:---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `create_scheduled_task` | `time`, `content`, `daily?` | 创建定时提醒：`time` 必须是根据最新消息时间换算的绝对时间 `YYYY-MM-DD HH:MM:SS`（须晚于当前 10 秒、不超过一年）；`content` 是到点时留给模型自己的备忘说明（非最终回复文本，运行时注入 `botName`，上限 500 字符），到点后以【系统定时任务】消息回到原会话再组织回复；`daily=true` 为每日重复（如"每天 8 点叫我起床"） |
| `cancel_scheduled_task` | `task_id`                   | 取消未触发的定时任务（含每日任务）；不知道编号时先 `list_scheduled_tasks`                                                                                                                                                                                                                                            |

### 异步耗时任务

| 工具              | 参数 | 说明 |
|-------------------|------|------|
| `demo_async_task` | 无   | 仅供明确测试异步任务时使用；约 3 秒后向原会话发送固定完成回执，不提供生图能力 |
| `generate_image` | `prompt`, `size?`, `use_self_reference?` | 按描述和可选尺寸生成一张图片；`size` 格式为宽x高（如 `1024x1024`），默认 `2048x2048`。绘制机器人自身形象时将 `use_self_reference` 设为 `true`，使用后台上传的角色参考图；默认 `false`。立即返回任务编号，生成后自动发送到原会话 |
| `edit_image` | `message_id`, `image_index`, `prompt`, `size?` | 按同会话消息中的一张静态图片和文字要求生成修改版；尺寸规则同 `generate_image`，任务完成后自动发送 |

异步任务按会话互斥：同一会话已有任务时返回现有任务编号，不再启动新任务；不同会话可并行。工具调用立即把受理或忙碌状态交回 Executor，是否在当前回复中告知用户仍由模型决定。启动状态只写入消息列表，不触发回复；任务完成或失败时则像定时任务一样，将系统消息加入入站工作流，触发一轮 Router 和 Executor 回复。后台任务完成后通过 `MessageService` 直接向原会话发送结果；发送成功的结果及其图片描述先进入消息列表，然后再注入成功系统消息。任务执行或发送失败时只注入失败系统消息，附带最多 500 字符的失败原因供模型判断，不直接向 QQ 发送失败提示。退出中断状态仅写入消息列表。

`demo_async_task` 已对模型开放，但工具描述限定为明确测试时调用。后台任务只保存在进程内，正常退出会记录中断；异常退出或重启不会恢复、重试，也不能用它替代可恢复的记忆维护任务。

`generate_image` 使用管理后台“LLM 配置”中的“图片生成”独立配置。默认地址为
`https://api.openai.com/v1`、路径为 `/images/generations`、模型为 `gpt-image-2.5-flare`，API Key 默认为空，需自行配置。
可用尺寸取决于实际配置的图片生成接口和模型；尺寸格式通过工具校验后原样传给接口。
`edit_image` 按消息 ID 和图片索引从当前会话记录中定位来源，执行时下载原图并在内存中转换为
`data:image/...;base64,...`，通过 `input_references` 传给图片接口，不保存原图或 base64。
`generate_image` 的角色参考图在后台“提示词 → 角色形象”上传，保存在持久化的 `data/character-image` 中；
仅在 `use_self_reference=true` 时读取并通过相同的 `input_references` 字段传给生图接口，日常聊天模型不接收图像字节。
使用角色参考图时，工具提示词只需描述场景、动作、表情等变化；生图请求会明确要求以参考图中的角色外观为准，
除非用户指定修改，否则不重新设计发型、服饰等形象设定。
未上传角色图时工具直接返回错误，不启动生图任务；普通生图和 `edit_image` 不受影响。
目前支持 PNG、JPEG、WebP 静态图片；原始 URL 已失效或无 URL 时需重新发送图片。
该请求格式按 OpenRouter 图片接口实现，其他服务商即使支持图生图，也可能要求不同字段或 multipart 上传。
接口响应支持普通图片 URL、`data:image/...;base64,...`、纯 base64 和 `data[0].b64_json`；
base64 数据只用于 OneBot 发送和视觉识别，不保存到聊天记录或日志。生成后复用“图片识别”模型及其哈希缓存，
识别得到的画面描述会在图片发送成功后写入助手图片消息段，供后续 Router、Executor 和记忆召回使用。
如果识别不可用或失败，图片仍会发送，消息段改为明确标注“依据生成提示词”的备用描述。
每次任务只请求一次，不自动重试以避免重复计费；接口请求最长等待 180 秒。生成服务和 OneBot 的实际联通需在部署环境验证。

## Lua 自定义工具

后台中每条 Lua 工具记录包含一个脚本，定义 `run(args, ctx)` 并返回字符串。`args` 对应工具参数 JSON；
`ctx.session_id` 是统一会话 ID 的字符串，`ctx.is_private` 表示私聊。每次调用使用独立 Lua 状态，不保留全局变量。

```lua
function run(args, ctx)
    local result = bot.send_message(args.text)
    if not result.ok then
        return "发送失败：" .. result.error
    end
    return "已发送，消息 ID：" .. result.message_id
end

function background(payload, ctx)
    return "后台处理完成：" .. payload.text
end
```

`bot.send_message(text)` 会等待 OneBot 发送结果，返回 `{ok=true, message_id="..."}` 或
`{ok=false, error="..."}`。`bot.start_task(description, payload)` 返回
`{status="started"|"busy"|"stopping", task_id="..."}`；必须定义 `background(payload, ctx)`，后台返回的字符串由
`AsyncTaskManager` 发送到原会话。同一会话只运行一个后台任务。测试按钮模拟发送与启动，不产生实际 QQ 消息。

脚本编辑后立即重载；正在执行的调用继续使用启动时的脚本版本。Lua 不开放文件、进程与任意 C++ 对象接口，
并限制脚本大小、内存、指令数和单次执行时间。这是减少误操作的限制，不是允许不可信用户上传脚本的安全沙箱。

## PageWeave 网页工具

`agentTools/fetch_webpage.json` 和 `agentTools/search_web.json` 是可导入的 Python 自定义工具，不属于 C++ 内置工具。
Lua 当前没有 HTTP 或 WebSocket 宿主接口，因此这两个工具使用 Python 标准库请求 PageWeave，无需 Jina Reader、DuckDuckGo 或第三方 Python 包。

| 工具 | 参数 | 行为 |
|------|------|------|
| `fetch_webpage` | `url`、`content_scope?`、`max_chars?`、`remove_images?`、`remove_links?` | 读取动态加载后的网页，默认整页 Markdown、12000 字符、保留链接、去掉图片引用 |
| `search_web` | `query`、`max_chars?` | URL 编码关键词后打开 `https://www.bing.com/search?q=...`，等待 `#b_results` 有内容，读取整页 Markdown |

网页网址缺少协议时补全 `https://`，支持 `//example.com`、域名加端口以及页面片段；只接受 HTTP/HTTPS，
拒绝内嵌用户名密码、空白、控制字符和反斜杠，不自动回退 HTTP。目标地址能否访问仍由 PageWeave 网络策略校验。
原有 `remove_images`/`remove_links` 参数继续保留，但 `remove_links` 默认改为 `false`，便于模型引用来源。

输出包含标题、最终来源网址及 Markdown 内容，并提示截断、正文兜底或动态观察达到上限。
HTTP 错误向模型返回状态码、PageWeave 错误码、原因和请求 ID；请求不自动重试。
搜索只读取 Bing 页面，不自动打开每个来源；验证码、访问限制或页面结构变化可能导致提取失败，不等于没有搜索结果。

### 配置和部署

1. 确认机器人运行环境存在 `python3`，在后台“自定义工具 → Python 配置”中设置解释器路径。
2. 确认该环境能访问 `http://172.31.100.240:7779/health/ready`，响应应为 `{"status":"ok"}`。
3. 导入两个 JSON 文件并启用；有同名旧工具时编辑其脚本和参数，或先删除再导入。
4. 测试读取 `{"url":"example.com"}` 和搜索 `{"query":"InSoulForge GitHub"}`。

脚本默认服务地址为 `http://172.31.100.240:7779/extract`。可修改各脚本的默认值，
或在启动机器人时设置 `PAGEWEAVE_URL` 为完整 HTTP/HTTPS 接口地址。脚本请求超时为 30 秒，PageWeave 的服务预算应短于此值。
Docker 里 `127.0.0.1` 指向机器人容器自身，不能因为两个服务在同一宿主机就使用回环地址互访。

当前发布镜像未安装 Python。部署时可用以下派生镜像添加解释器，然后使用该镜像启动机器人；
直接在正在运行的容器中安装只适合临时测试，重建容器后会丢失。

```dockerfile
FROM dreamdonghao/insoulforge:latest
USER root
RUN apt-get update && apt-get install -y --no-install-recommends python3 \
    && rm -rf /var/lib/apt/lists/*
```

网页工具的参数和错误契约测试无需 C++ 构建：

```bash
python3 -m unittest discover -s tests -p 'test_pageweave_tools.py'
```

## 共同模式

- **跨回复工具历史**：Executor 按实际处理顺序保存 `tool_history`，每项含 `name`、`arguments` 和 `status`。
  正常回复发送成功后，历史附在助手消息上；`no_reply` 写入内部助手执行记录，不发送 QQ 消息、没有 QQ 消息 ID，也不触发新回复。
  已调用工具后模型请求失败、耗尽轮数或发送失败，也保留内部记录并标明 `failed`/`send_failed`，不把它们当主动不回复。
  记录可随 MessageList 保存和恢复，工具历史只投影给 Executor；Router 和记忆维护不接收调用参数。
  参数中的常见凭据和图片数据会省略，单次参数超过 2048 字节时明确标注省略，不保存完整工具结果。
  `decision` 表示回复工具形成决策，`invalid_arguments` 表示回复参数无效，`rejected` 表示最后一轮拒绝调用；
  `returned` 只表示工具处理器返回，不能仅凭它判断业务成功。此前查询不含完整结果，需要时仍可重新核实。
- **调用日志**：Executor 记录工具名称，并在 debug 级别记录工具结果；不打印脚本源码或 Python 启动命令。
  Python 执行失败时保留退出状态和输出日志，具体输出摘要继续作为工具结果回传给模型。
- **错误反馈**：普通工具的结果会作为 `tool` 消息回传给 Executor。处理器抛异常时，`ToolRegistry` 将异常摘要（最多 500 字符）转为工具结果，不让异常中断整轮调用；回复工具的参数错误则由 Executor 自行回传。Python 工具失败时附带退出状态与输出摘要，HTTP 自定义工具附带传输错误或 HTTP 状态及服务端错误消息。仅返回布尔成功状态的 OneBot 接口无法提供更细的失败原因，工具会返回相应的通用提示。异步工具启动后的执行错误通过新的系统消息反馈，不作为原工具调用的即时结果。
- **会话上下文**：所有工具签名统一为 `(json args, ToolCallContext)`，会话 ID 从 `ctx.sessionId` 取得；`ctx.messageSnapshot`
  是本轮冻结的完整消息快照，媒体工具可用它按 `message_id` 与 `image_index` 解析图片来源。群操作类工具统一做私聊拦截与
  `sessionId == 0` 校验
- **参数来源**：QQ 号、消息 ID、图片索引等参数取自聊天记录 JSON 的 `sender.qq` / `message_id` /
  `segments[].image_index` / `reply_to` 等字段；图片 `file/url` 仅由服务端从 `assets.images` 读取，不会提供给模型
- **宽容取值**：参数读取统一走 `argString` / `getInt` / `getUInt` / `getBool`（见 `include/infrastructure/JsonUtil.hpp`
  ），缺参数返回引导性错误提示而非异常
- **注册方式**：见 `include/agent/tools/plugins/*ToolsPlugin.hpp`、`include/agent/tools/ToolArgument.hpp`
  与 [DEVELOPMENT.md](./DEVELOPMENT.md) 开发指南
