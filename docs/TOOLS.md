# 内置工具参考

Executor（`ExecutorAgent`）在单个 Agent 循环中通过工具调用生成回复。所有内置工具经 `ToolRegistry`
的进程内插件注册，按类别拆分在 `src/workflow/agent/tools/plugins/`：

| 类别          | 插件实现文件                          | 语义                         |
|---------------|---------------------------------------|------------------------------|
| `REPLY`       | `tools/plugins/ReplyToolsPlugin.cpp`  | 结束控制，直接结束或发送成功后结束     |
| `INFORMATION` | `tools/plugins/InfoToolsPlugin.cpp`   | 查询数据、获取答案，无副作用 |
| `ACTION`      | `tools/plugins/ActionToolsPlugin.cpp` | 执行操作、产生副作用         |

管理后台“执行配置”中的最大迭代轮数控制整个 Executor 工具循环，配置字段为 `execution.maxToolRounds`，
默认 8、允许 1～100。一次模型请求及其工具处理算一轮，同轮多个工具不增加轮数，生成最终回复也占一轮。
保存后对新回复流程生效；达到上限仍未生成回复时，记录错误并结束，不额外生成兜底回复。

每轮请求在上下文末尾附带当前轮次及后续剩余轮数，执行状态只用于当前请求，不写入聊天记录或后续轮次历史。
最后一轮只开放 REPLY 类的 `send_message_and_finish`、`finish_turn`，不会执行查询或动作工具。
若最大轮数设为 1，第一轮就是仅回复模式。工具结果日志仍保留。

内置三组分别归属 `builtin.send_message_and_finish`、`builtin.info`、`builtin.action` 插件，由 `ToolPluginCatalog`
显式加载；自定义工具（Lua）归属 `custom` 插件并保守注册为 `ACTION`（脚本可发送消息和写入外部服务）。同名工具不能跨插件覆盖；刷新自定义工具只会替换
`custom` 的工具。

注入顺序固定为“类别 → `promptOrder` → 工具名”，避免重启或刷新后顺序漂移。私聊注入时会排除仅群聊的 `at_user`、`ban_user`、
`send_poke`；工具本身仍保留会话校验作为防线。

> `REPLY` 类工具的 handler 只是占位：它们在 `ExecutorAgent::processToolCalls` 内被拦截执行（需要改写回复决策而非返回工具结果），不经
> `ToolRegistry::executeTool`。

## 消息发送与结束（3 个）

| 工具 | 参数 | 行为 |
| --- | --- | --- |
| `send_message` | `content`, `quote_message_id?`, `group_id?`, `user_id?` | 发送后继续处理，支持指定已启用目标 |
| `send_message_and_finish` | `content`, `quote_message_id?` | 向当前会话发送，成功后结束 |
| `finish_turn` | 无 | 不发送消息，直接结束 |

引用参数取当前会话可见消息的 message_id。发送失败或未知时回传模型，由模型决定下一步；仍受最大工具轮数限制，最后一轮失败时记录未完成状态，不额外请求模型。

## INFORMATION（查询）

| 工具                   | 参数       | 说明                                                                                                                                                                                                                                                                            |
|------------------------|------------|---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `get_group_messages`   | `group_id` | 读取已启用目标群当前内存中保留的全部可共享消息；群号为数字字符串，返回 `session` 与按旧到新排列的 `messages`，每条最多 200 个 Unicode 字符                                                                                                                                      |
| `recall_memory`        | `query`    | 长期记忆检索：`LongTermMemory::searchMemory` 按 Brute-Force 余弦相似度（阈值 0.3）取 top-3，返回"回忆起：…"或"想不起来"                                                                                                                                                         |
| `deep_think`           | `question` | 深度思考：调用 `executorThinking` 配置的模型求解。只接收 question 中显式提供的完整问题、必要材料和已取得结果，不自动携带聊天上下文 |
| `list_scheduled_tasks` | 无         | 列出当前会话待触发的定时任务（编号 / 触发时间 / 备忘内容），取消前查询用                                                                                                                                                                                                        |

`get_group_messages` 不受当前或目标会话的共享开关、自动共享条数、三天范围和模型上下文窗口限制，私聊也可调用。
它不查数据库或平台历史，不能恢复已总结移除的消息；已启用群没有内存记录时返回空数组，不创建会话。
消息字段与自动共享相同，排除系统、内部执行及发送失败记录，不含消息 ID、引用目标、图片来源或索引、工具历史及召回记忆。
此工具的读取范围比自动共享更广；如不希望模型主动查询其他已启用群，可在后台关闭该工具。

## ACTION（动作）

### 过程消息

| 工具                 | 参数                               | 说明                                                                                                     |
|----------------------|------------------------------------|----------------------------------------------------------------------------------------------------------|
| `send_message` | `content`, `group_id?`, `user_id?` | 发送文字后继续处理，最终仍由 `send_message_and_finish` / `finish_turn` 收尾。可指定已启用的目标群或私聊，发送结果记入目标会话 |

`send_message` 和 `send_sticker` 的 `group_id`（群号）与 `user_id`（私聊对象 用户 ID）均为数字字符串，不能同时指定，均省略则使用当前会话。
显式指定的目标必须在后台启用；私聊黑名单用户不可发送，不会自动启用未知私聊。该能力不受跨会话共享开关限制，支持群到私聊、私聊到群和私聊之间发送。
`user_id` 使用普通 用户 ID，不是带高位标志的内部会话 ID。发送失败或超时不会自动重试，工具结果明确返回目标和状态，聊天记录仅进入目标会话，不触发目标会话自动回复。

### 表情包

后台「OneBot 配置 → 实现类型」选择 NapCat 或 SnowLuma；模型工具及后台收藏管理 API 不变。
适配层将两种收藏列表统一后供读取、发送、删除和改名使用：

| 操作 | NapCat | SnowLuma |
| --- | --- | --- |
| 查询 `fetch_custom_face_detail` | `eId` 为表情 ID，`resId` 为资源 ID | `emoji_id` 为表情 ID，`resId` 为资源 ID |
| 删除 `delete_custom_face` | 传 `res_id` | 传 `emoji_id`（该版本列表的 `resId` 与 `emoji_id` 相同） |
| 修改描述 | `set_custom_face_desc`，传 `emoji_id/res_id/md5/desc` | `modify_custom_face`，传 `emoji_id/desc` |
| 添加收藏 | `add_custom_face.file` 接收协议主机路径 | 同名接口；POSIX 绝对路径转换为 `file://` 来源 |

下载与上传沿用 `download_file.url/base64`，读取 `data.file` 后再收藏；共享的拍一拍与空间接口不按实现拆分。
原生商城发送由实际字段决定：`is_mark_face`、表情 ID、表情包 ID 和 key 齐全时发送 `mface`；否则有 URL 就发图片表情，
两者都不满足则返回错误。SnowLuma 的收藏详情没有完整商城字段，不代表它不支持原生商城发送。
本次不收集聊天中的商城字段。保存已成功但新增条目未确认或命名失败时，工具明确返回部分成功，不能认为名称已设置。

接口核对基准（2026-11-06）：[NapCat main 收藏接口](https://github.com/NapNeko/NapCatQQ/blob/main/packages/napcat-onebot/action/extends/CustomFace.ts)、
[SnowLuma v2.4.0 扩展实现](https://github.com/SnowLuma/SnowLuma/blob/v2.4.0/packages/onebot/src/actions/extended.ts)，
以及 [SnowLuma main 空间实现](https://github.com/SnowLuma/SnowLuma/blob/main/packages/onebot/src/actions/qzone.ts)。
本次验证使用本地模拟 OneBot 服务；未进行真实 QQ／SnowLuma 联调，不将源码契约兼容视为线上验证通过。


| 工具             | 参数                                | 说明                                                                                                                                                                                                                                             |
|------------------|-------------------------------------|--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `send_face`      | `id`                                | 生成 QQ 原生表情 CQ 码（返回的 CQ 码需拼进 `send_message_and_finish` 的 content）                                                                                                                                                                                  |
| `send_image`     | `image_id`                               | 读取图片缓存后以 Base64 直接发送到当前会话，记录图片 ID 和已有有效描述，缺失时使用识图及描述缓存，失败记录“图片描述不可用”仍发送；ID 只能来自当前可见快照                                                                                                                                                                                                                       |
| `send_sticker`   | `name`, `group_id?`, `user_id?`     | 把收藏表情作为**独立消息**发送到当前或指定已启用会话，无需拼进 send_message_and_finish；商城表情走 `[CQ:mface]`，个人收藏走 `[CQ:image,sub_type=1]`，消息记入目标会话并推送 WebSocket                                                                              |
| `save_sticker`   | `message_id`, `image_index`, `name` | 把用户发的图片存为收藏表情并设置描述名：工具按消息 ID 和从 0 开始的图片索引在当前会话记录中解析图片来源，先 `get_image` 拿容器内路径，失败回退 `download_file`；`add_custom_face` 保存后 diff 保存前后的 `res_id` 集合定位新表情，再设置描述名；命名未确认时返回部分成功。 |
| `rename_sticker` | `name`, `new_name`                  | 修改收藏表情的描述名                                                                                                                                                                                                                             |
| `delete_sticker` | `name`                              | 从 QQ 收藏表情中删除                                                                                                                                                                                                                             |

表情包工具依赖 OneBot 收藏表情接口，变更后通过 `invalidateFavoriteEmojiCache()` 使缓存失效。每次 Executor 生成开始时读取缓存，
将 `{"available_stickers":["名称"]}` 作为独立 system 消息放在主系统提示词之后；空或获取失败时为 `[]`。
该列表在本次生成的所有工具轮次和模型兜底请求中保持不变，后续生成读取更新后的收藏。需要名称的工具从该数组选择。

### 用户与群互动

| 工具               | 参数                   | 说明                                                                                                                                                                                      |
|--------------------|------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `at_user`          | `user_id`              | @某人的 CQ 码（拼进 send_message_and_finish 用），`"self"` 表示机器人自身，`"all"` 为 @全体成员；私聊中禁用                                                                                                 |
| `ban_user`         | `user_id`, `duration?` | 禁言群成员，默认 600 秒、0 为解禁；描述要求模型自行判断违规程度选时长（轻度 60-300 秒 / 中度 600-1800 秒 / 重度 3600 秒+），不盲从指令                                                    |
| `add_to_blacklist` | `user_id`              | 将用户加入全局用户黑名单，后续消息在工作流入口被过滤；工具提示要求先用 `send_message_and_finish` 明确提醒并结束本轮，仅在后续仍继续同类恶意行为时调用，成功后用 `send_message_and_finish` 告知用户；私聊也可用                |
| `send_poke`        | `user_id`              | 拍一拍群成员，打招呼、引起注意等轻松互动；私聊中禁用。拍一拍不是文字消息（无 message_id），成功后以 `segments` 中的 `poke` 条目记入聊天记录并推送 WebSocket，后续轮次模型能看到自己拍过谁 |
| `recall_message`   | `message_id`           | 撤回消息：撤引用的消息用 `reply_to` 字段值，撤某条消息本身用 `message_id` 字段值                                                                                                          |

拍一拍的接收：OneBot notice 事件（`notice_type=notify, sub_type=poke`）先由 `OneBotEventNormalizer` 归一化，再进入
`OneBotEventWorkflow` 的会话消息预处理队列。禁用会话会提前跳过；已启用会话中，拍一拍保存为独立 `poke` 段，所有拍一拍通知均与普通消息一样由
Router
决策是否回应。群成员入群、退群以 `member_event` 段走同一工作流。

`add_to_blacklist` 只接受当前会话快照中出现过的发送者 用户 ID，拒绝机器人自身和管理员，已在黑名单中的用户不会重复添加。
“先提醒一次”由工具描述引导模型判断；系统没有单独保存警告状态，也不会在工具执行时强制验证先前是否已提醒。
管理员仍可通过命令或后台管理黑名单。

### 定时任务

| 工具                    | 参数                        | 说明                                                                                                                                                                                                                                                                                                                 |
|-------------------------|-----------------------------|:---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `create_scheduled_task` | `time`, `content`, `daily?` | 创建定时提醒：`time` 必须是根据 execution_state.current_time（服务端本地时区）换算的绝对时间 `YYYY-MM-DD HH:MM:SS`（须晚于当前 10 秒、不超过一年）；`content` 是到点时留给模型自己的备忘说明（非最终回复文本，运行时注入 `botName`，上限 500 字符），到点后以【系统定时任务】消息回到原会话再组织回复；`daily=true` 为每日重复（如"每天 8 点叫我起床"） |
| `cancel_scheduled_task` | `task_id`                   | 取消未触发的定时任务（含每日任务）；不知道编号时先 `list_scheduled_tasks`                                                                                                                                                                                                                                            |

### 内置工具配置

管理后台“工具配置 → 内置工具”可逐个开启或关闭工具、修改传给模型的 description，
也可恢复默认描述。配置保存在 `data/config.json` 的 `builtinTools` 中，保存后立即生效，重启保留。
关闭的工具不会出现在后续模型请求中，后续调用也会被拒绝；已经开始执行的任务不强制中断。
回复工具也允许关闭，请保留所需的回复能力。自定义 Lua 工具仍在同页的“自定义工具”标签管理，
MCP 远程工具仍由“MCP 服务”管理。

### 空间说说

`send_qzone_msg(content, images?, ugc_right?)` 在机器人账号自己的 QQ 空间发布说说，
仅在用户明确要求发布时调用。正文不能为空；`images` 为最多 9 张图片的 HTTP/HTTPS URL 数组，
不接受本地路径或 base64。`ugc_right` 支持 `1` 所有人可见（默认）、`4` 好友可见、`64` 仅自己可见。

依赖 [NapCat 的 send_qzone_msg 扩展](https://github.com/NapNeko/NapCatQQ/pull/1965)，
不是标准 OneBot 11 能力；SnowLuma 的同名接口也支持当前参数和 tid 返回。复用后台配置的 HTTP/WebSocket 连接及 Access Token，无需另外配置空间 Cookie。
成功返回说说 `tid`，失败保留接口错误详情；超时或响应缺少 `tid` 时不能确认是否已发布，
应先检查空间，工具不会自动重试。调用结果通过现有工具历史进入上下文，不伪装成聊天消息。

### 异步耗时任务

| 工具              | 参数                                                          | 说明                                                                                                                                                 |
|-------------------|---------------------------------------------------------------|------------------------------------------------------------------------------------------------------------------------------------------------------|
| `demo_async_task` | 无                                                            | 仅供明确测试异步任务时使用；约 3 秒后向原会话发送固定完成回执，不提供生图能力                                                                        |
| `generate_image`  | `prompt`, `size?`, `use_self_reference?`, `reference_images?` | 生成或编辑图片；参考图数组支持图片 ID 字符串、`{image_id}` 或旧 `{message_id, image_index}`，支持多张会话图片与角色形象图同时使用。默认尺寸 `2048x2048`，任务完成后自动发送到原会话 |

异步任务按会话互斥：同一会话已有任务时返回现有任务编号，不再启动新任务；不同会话可并行。工具调用立即把受理或忙碌状态交回 Executor，是否在当前回复中告知用户仍由模型决定。启动状态只写入消息列表，不触发回复；任务完成或失败时则像定时任务一样，将系统消息加入入站工作流，触发一轮 Router 和 Executor 回复。后台任务完成后通过 `MessageService` 直接向原会话发送结果；发送成功的结果及其图片描述先进入消息列表，然后再注入成功系统消息。任务执行或发送失败时只注入失败系统消息，附带最多 500 字符的失败原因供模型判断，不直接向 QQ 发送失败提示。退出中断状态仅写入消息列表。

`demo_async_task` 已对模型开放，但工具描述限定为明确测试时调用。后台任务只保存在进程内，正常退出会记录中断；异常退出或重启不会恢复、重试，也不能用它替代可恢复的记忆维护任务。

`generate_image` 使用管理后台“LLM 配置”中的“图片生成”独立配置。默认地址为
`https://api.openai.com/v1`、路径为 `/images/generations`、模型为 `gpt-image-2.5-flare`，API Key 默认为空，需自行配置。
可用尺寸取决于实际配置的图片生成接口和模型；尺寸格式通过工具校验后原样传给接口。
`generate_image` 按 `reference_images` 中的可见图片 ID 或旧消息 ID 和图片索引从当前会话定位原图，在内存中转换为
`data:image/...;base64,...`，不保存下载的原图或 base64。原 `edit_image` 已合并到此工具。
`generate_image` 的角色参考图在后台“提示词 → 角色形象”上传，保存在持久化的 `data/character-image` 中；
仅在 `use_self_reference=true` 时读取并放在所有参考图的第一张，日常聊天模型不接收图像字节。
使用角色参考图时，工具提示词只需描述场景、动作、表情等变化；生图请求会明确要求以参考图中的角色外观为准，
除非用户指定修改，否则不重新设计发型、服饰等形象设定。
未上传角色图时工具直接返回错误，不启动生图任务；未启用角色参考的调用不受影响。
目前支持 PNG、JPEG、WebP 静态图片；原始 URL 已失效或无 URL 时需重新发送图片。
“模型配置 → 图片生成”可切换 `apiType`：
- `openai`：无参考图时使用 `path`（默认 `/images/generations`）；有参考图时使用 `editPath`（默认 `/images/edits`），发送 `images: [{"image_url":"data:image/...;base64,..."}]` JSON，适用于支持该格式的 OpenAI / CLIProxyAPI 接口。
- `openrouter`：始终使用 `path`（通常 `/images`），参考图字段为 `input_references: [{"type":"image_url","image_url":{"url":"data:image/...;base64,..."}}]`。

新配置默认 `openai`；已有无 `apiType` 的配置迁移为 `openrouter`，保留原地址和路径，需在后台手动切换。Base URL 和两种路径独立配置，避免重复 `/v1` 前缀。只支持 multipart 编辑的旧接口需升级或另行适配。
接口响应支持普通图片 URL、`data:image/...;base64,...`、纯 base64 和 `data[0].b64_json`；
base64 数据只用于 OneBot 发送和视觉识别，不保存到聊天记录或日志。生成后复用“图片识别”模型及其哈希缓存，
识别得到的画面描述会在图片发送成功后写入助手图片消息段，供后续 Router、Executor 和记忆召回使用。
如果识别不可用或失败，图片仍会发送，消息段改为明确标注“依据生成提示词”的备用描述。
每次任务只请求一次，不自动重试以避免重复计费；接口请求最长等待 180 秒。生成服务和 OneBot 的实际联通需在部署环境验证。

## 自定义工具文件存储

工具保存在数据库同级的 `tools/`，默认部署路径为 `/app/data/tools/`，沿用现有 `/app/data` 持久化挂载。

```text
data/tools/
├── fetch_webpage/
│   ├── tool.json
│   └── main.lua
└── search_web/
    ├── tool.json
    └── main.lua
```

每个目录对应一个工具；`tool.json` 保存定义，`main.lua` 保存 Lua 脚本。自定义工具仅支持 Lua，网络请求使用 `http.request()`。
工具名与目录名必须一致，仅允许英文字母、数字、下划线和连字符，长度 1–64。示例：

```json
{
  "id": 1,
  "name": "fetch_webpage",
  "description": "读取网页内容",
  "parameters": {"type": "object", "properties": {"url": {"type": "string"}}, "required": ["url"]},
  "executorType": "lua",
  "readme": "工具使用说明",
  "enabled": true
}
```

`id` 是前端操作所用的唯一正整数；手动新增时不得与其他工具重复。前端新增会自动分配 ID，`.next-id` 保存分配水位，不能随意删除。
`parameters` 在文件里是 JSON 对象，不是双重编码的字符串。旧 Python/HTTP 工具目录会记录不支持的执行类型并跳过加载，不会自动删除文件；请改写为 Lua 工具。
前端添加、编辑、启停、删除和原有单文件导入导出仍可使用。手动编辑后通过后台刷新工具或重启加载；执行中的调用保留旧脚本快照。
单个工具文件最多 1 MiB；无效目录会记录原因并跳过，不影响其他工具加载。禁止工具目录及定义、脚本文件使用符号链接。

保存使用 `.pending-<id>` 暂存目录和 `.backup-<id>` 旧版本目录，失败时回滚；下次读取时清理未发布的暂存目录并恢复或清理旧版本。
这些名称为程序保留，不要手动创建。不要让多个程序实例同时写同一个工具目录，也不要在前端保存的同时手工编辑文件。

**升级说明：数据库 Schema 从 v12 升级到 v13。启动时自动删除旧 `custom_tools` 表，不导出或迁移旧工具内容。**
更新前如需保留旧工具，请先在旧版管理后台导出，更新后重新导入。其他业务数据不受此迁移影响。Schema v13 → v14 会清理旧 `custom_tool_python` 设置；旧 Python/HTTP 工具文件只跳过加载，不自动删除。
新版本的备份必须同时保留数据库和 `tools/`（备份整个 `data/` 即可）。

## Lua 自定义工具

后台中每条 Lua 工具记录包含一个脚本，定义 `run(args, ctx)` 并返回字符串。`args` 对应工具参数 JSON；
`ctx.session_id` 是带类型的规范字符串，例如 `group:123456` 或 `private:some-user`；目标中的特殊字符使用百分号编码，不能再用 `tonumber(ctx.session_id)` 取得目标。`ctx.is_private` 表示私聊。后台独立测试没有会话时，`ctx.session_id` 为 `json.null`，`ctx.is_private` 为 `false`。每次调用使用独立 Lua 状态，不保留全局变量。

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

### HTTP、JSON 与 URL

- `http.request(options)`：异步发送 HTTP 请求。`url` 必填，只接受 HTTP/HTTPS，不允许内嵌用户名密码。`method` 默认 `GET`，支持 GET、POST、PUT、PATCH、DELETE、HEAD、OPTIONS。
- `options.headers` 是字符串键值对象；`body` 是原始字符串，不自动进行 JSON 编码。`timeout_ms` 默认 30000，范围 1–120000。
- 请求成功返回 `{ok=true, status=200, headers={}, body="..."}`；HTTP 4xx/5xx 仍是 `ok=true`，由脚本检查 `status`。传输或参数错误返回 `{ok=false, error="具体错误"}`。不自动跟随重定向。
- 请求体最多 1 MiB；响应接收完成后检查大小，超过 1 MiB 不传回 Lua。此检查不限制网络接收阶段的内存使用。
- `json.encode(value)` 与 `json.decode(text)` 转换 JSON；错误会抛出 Lua 错误，可用 `pcall` 捕获。`json.null` 表示 JSON null；解码得到的数组（包括空数组）再次编码时保留数组类型。普通空表编码为 `{}`，需要空数组时使用 `json.decode("[]")`。
- `url.encode(text)` 编码查询参数值，空格编码为 `+`，其他特殊字符采用百分号编码；不要用它编码整个网址或路径片段。

```lua
function run(args, ctx)
    local result = http.request({
        url = "https://api.example.com/search?q=" .. url.encode(args.query),
        headers = { Accept = "application/json" }
    })
    if not result.ok then return "请求失败：" .. result.error end
    if result.status < 200 or result.status >= 300 then
        return "HTTP " .. result.status .. "：" .. result.body
    end
    return result.body
end
```

测试按钮不会实际发送 HTTP 请求，而是校验参数并返回状态码 200、空响应头和传入的 body（未传时为空字符串）。
HTTP 与消息发送、启动任务合计最多 32 次；JSON 和 URL 转换不占此配额，但仍受脚本指令与内存限制。
HTTP 接口可访问容器网络中的内部服务，不是 SSRF 安全隔离层；只有受信任的管理员应当维护脚本。

## PageWeave 网页工具

`agentTools/fetch_webpage.json` 和 `agentTools/search_web.json` 是可导入的 Lua 自定义工具，不属于 C++ 内置工具。
工具通过 Lua 的异步 `http.request()` 请求 PageWeave，不需要 Python 或额外第三方包。

| 工具            | 参数                                                                     | 行为                                                                                                  |
|-----------------|--------------------------------------------------------------------------|-------------------------------------------------------------------------------------------------------|
| `fetch_webpage` | `url`、`content_scope?`、`max_chars?`、`remove_images?`、`remove_links?` | 读取动态加载后的网页，默认整页 Markdown、12000 字符、保留链接、去掉图片引用                           |
| `search_web`    | `query`、`max_chars?`                                                    | URL 编码关键词后打开 `https://www.bing.com/search?q=...`，等待 `#b_results` 有内容，读取整页 Markdown |

网页网址缺少协议时补全 `https://`，支持 `//example.com`、域名加端口以及页面片段；只接受 HTTP/HTTPS，
拒绝内嵌用户名密码、空白、控制字符和反斜杠，不自动回退 HTTP。目标地址能否访问仍由 PageWeave 网络策略校验。
原有 `remove_images`/`remove_links` 参数继续保留，但 `remove_links` 默认改为 `false`，便于模型引用来源。

输出包含标题、最终来源网址及 Markdown 内容，并提示截断、正文兜底或动态观察达到上限。
HTTP 错误向模型返回状态码、PageWeave 错误码、原因和请求 ID；请求不自动重试。
搜索只读取 Bing 页面，不自动打开每个来源；验证码、访问限制或页面结构变化可能导致提取失败，不等于没有搜索结果。

### 配置和部署

1. 确认机器人容器能访问 `http://172.31.100.240:7779/health/ready`。
2. 修改脚本中的 `endpoint` 为实际 PageWeave 接口地址，默认 `http://172.31.100.240:7779/extract`。
3. 导入两个 JSON 文件并启用；同名旧工具需要删除后重新导入或直接修改文件。
4. 实际调用读取 `{"url":"example.com"}` 和搜索 `{"query":"InSoulForge GitHub"}`。

请求超时为 30 秒，不自动重试。Docker 中的 `127.0.0.1` 指向容器自身，不能用它访问其他容器或宿主机服务。
后台测试模式模拟 HTTP，不访问 PageWeave；真实网络请求需要通过实际工具调用验证。
示例脚本校验及参数契约测试纳入 C++ 的 `message_contracts`，运行环境不需要 Python。

## DeepSeek 网页工具

`agentTools/deepseek_search.json` 和 `agentTools/deepseek_fetch.json` 是独立的可导入 Lua 工具，
不替换 PageWeave 工具，也不依赖其服务。

| 工具              | 参数                                                                 | 行为                                                     |
|-------------------|----------------------------------------------------------------------|----------------------------------------------------------|
| `deepseek_search` | `query`、`count?`、`need_content?`、`ori_question?`、`localization?` | 搜索网页，默认返回 5 条结果及摘要，可选择附带正文        |
| `deepseek_fetch`  | `url`                                                                | 抓取网页，返回标题及 Markdown 正文；缺少协议时补全 HTTPS |

导入后在两个脚本顶部的 `api_key` 中分别填写 DeepSeek API Key，再启用工具。
密钥不作为模型参数，不要提交真实密钥。默认请求官方 `https://api.deepseek.com/v1/tools/search`
和 `/v1/tools/fetch`；普通聊天模型中转站不一定支持这些接口。
请求使用异步 HTTP，超时 60 秒，不自动重试；网络错误和 HTTP/API 错误详情返回模型。
搜索结果保留来源、摘要及评分等原始字段；抓取能力受目标站点访问策略影响，不能替代所有浏览器动态渲染场景。
修改示例不会覆盖已导入工具，需在后台同步更新。
接口及计费说明见 [DeepSeek 官方文档](https://api-docs.deepseek.com/zh-cn/guides/search/)。

两个工具的单次输出（含来源、标题及截断提示）最多 20000 个 Unicode 字符，
超出按 UTF-8 字符边界截断并明确标记。搜索返回的 JSON 文本截断后不保证结构完整。
此上限用于限制单次上下文开销，并非模型的上下文极限；多轮累计结果仍可能超出模型容量。
HTTP 响应体仍受 Lua 执行器的 1 MiB 限制，超限时返回错误，而不是下载后截断。

## 共同模式

- **跨回复工具历史**：Executor 按实际处理顺序保存 `tool_history`，每项含 `name`、`arguments` 和 `status`。
  `send_message`、`send_message_and_finish` 不重复保存调用正文到工具历史，正文与引用目标分别保留在助手消息的 `segments`、`reply_to` 中；
  两种回复工具的参数错误仍在当前迭代中回传模型，但不保留跨回复的调用记录。`finish_turn`、查询和动作工具继续记录。
  正常回复发送成功后，历史附在助手消息上；`finish_turn` 写入内部助手执行记录，不发送 QQ 消息、没有 QQ 消息 ID，也不触发新回复。
  已调用工具后模型请求失败、耗尽轮数或发送失败，也保留内部记录并标明 `failed`/`send_failed`，不把它们当主动不回复。
  记录可随 MessageList 保存和恢复，工具历史只投影给 Executor；Router 和记忆维护不接收调用参数。
  参数中的常见凭据和图片数据会省略，单次参数超过 2048 字节时明确标注省略，不保存完整工具结果。
  新记录中的 `decision` 表示 `finish_turn` 形成决策，`rejected` 表示最后一轮拒绝调用；
  `returned` 只表示工具处理器返回，不能仅凭它判断业务成功。此前查询不含完整结果，需要时仍可重新核实。
- **调用日志**：Executor 记录工具名称，并在 debug 级别记录工具结果；不打印脚本源码。
- **错误反馈**：普通工具的结果会作为 `tool` 消息回传给 Executor。处理器抛异常时，`ToolRegistry` 将异常摘要（最多 500 字符）转为工具结果，不让异常中断整轮调用；回复工具的参数错误则由 Executor 自行回传。Lua HTTP 接口返回传输错误或原始 HTTP 响应，由脚本将具体原因回传模型。仅返回布尔成功状态的 OneBot 接口无法提供更细的失败原因，工具会返回相应的通用提示。异步工具启动后的执行错误通过新的系统消息反馈，不作为原工具调用的即时结果。
- **会话上下文**：所有工具签名统一为 `(json args, ToolCallContext)`，需要真实会话时通过 `ctx.requireSession()` 取得 `SessionId`，可选日志归属使用 `ctx.sessionId`；`ctx.messageSnapshot`
  是本轮冻结的完整消息快照，媒体工具可用它按 `message_id` 与 `image_index` 解析图片来源。群操作类工具统一检查会话存在并拦截私聊。
- **参数来源**：用户 ID、消息 ID、图片索引等参数取自聊天记录 JSON 的 `sender.uid` / `message_id` /
  `segments[].image_index` / `reply_to` 等字段；图片 `file/url` 仅由服务端从 `assets.images` 读取，不会提供给模型
- **宽容取值**：参数读取统一走 `argString` / `getInt` / `getUInt` / `getBool`（见 `include/infrastructure/JsonUtil.hpp`
  ），缺参数返回引导性错误提示而非异常
- **注册方式**：见 `include/workflow/agent/tools/plugins/*ToolsPlugin.hpp`、`include/workflow/agent/tools/ToolArgument.hpp`
  与 [DEVELOPMENT.md](./DEVELOPMENT.md) 开发指南

图片 ID 使用 `img_` 加 8 位十六进制的短标识；旧完整 SHA-256 ID 仍可读取。

`ask_images(question, images)` 支持按顺序查询多张原图；`images` 接受图片 ID 字符串、`{image_id}` 或旧 `{message_id, image_index}`。
ID 必须在当前冻结会话快照中可见，缓存过期时要求重发；旧引用优先读缓存，缺失时回退原始 URL。GIF 仍按播放时间抽帧。

### 管理后台 QQ 收藏表情

“收藏表情”页面直接管理当前机器人 QQ 账号的收藏，不写入本地图片缓存。添加支持 **HTTP(S) 图片直链**（不含 URL 账号密码，最多 4096 字符），不需要聊天消息 ID。后端复用 `OneBotClient::downloadFile` 将图片下载至 OneBot/NapCat 主机，再调用 `addCustomFace` 收藏；不支持浏览器本地文件上传、Base64、后台服务器本地路径或网页分享链接。添加后可使用现有描述编辑功能命名。

[NapCat 收藏接口实现](https://github.com/NapNeko/NapCatQQ/blob/main/packages/napcat-onebot/action/extends/CustomFace.ts) 的 `add_custom_face.file` 要求 NapCat 主机上的本地文件路径，并非通用上传接口，因此未将浏览器文件或后台路径直接传给它。删除通过 `delete_custom_face.res_id` 定位 QQ 资源，不按可能重复的描述名删除；界面必须先确认，删除会影响该账号所有会话的收藏使用。

管理接口均受现有 `/admin/api/` 管理员会话认证保护：

| 方法与路径 | 请求 | 行为 |
| --- | --- | --- |
| `POST /admin/api/emoji/add` | `{"url":"https://example.com/image.gif"}` | 下载并提交 QQ 收藏；随后刷新列表，可单独编辑描述 |
| `POST /admin/api/emoji/add` | `multipart/form-data`，单个 `image` 文件 | 上传本地图片并提交 QQ 收藏 |
| `POST /admin/api/emoji/delete` | `{"res_id":"QQ资源ID"}` | 删除指定 QQ 收藏 |

成功返回 `{"success":true,"message":"…"}`；校验或 OneBot 失败返回 `{"success":false,"error":"…"}`。收藏变更使用 `ToolRuntime` 域函数并使 `FavoriteEmojiCache` 失效，界面成功后重新请求真实列表。删除工具 `delete_sticker` 共享删除域函数。下载失败不会调用收藏接口；收藏或删除请求失败也会清除缓存，以便核对超时后实际已生效的情况。

QQ 必须在线，OneBot 实现及 QQ 版本必须支持相关扩展；收藏数量、图片格式/大小、重复图片处理和同步延迟由 QQ 决定。接口成功仅表示 QQ 请求被接受，不能保证新增独立条目；请核对刷新后的列表。当前列表接口最多请求 200 项，且沿用原有行为：离线/不支持/查询失败可能返回空数组，不能据此断言 QQ 收藏为空。商城表情展示与删除仍取决于 QQ 返回的数据和能力，不承诺将普通图片导入为商城表情。

验证：`npm --prefix frontend test`、`npm --prefix frontend run build`；后端构建 `insoulforge_favorite_emoji_tests` 后运行 `ctest --test-dir <构建目录> -R '^favorite_emojis$' --output-on-failure`。该测试使用本地模拟 OneBot HTTP 服务，覆盖管理员认证、输入校验、下载路径传递、相同名称按资源 ID 删除、错误反馈及变更后的缓存失效，不触碰真实 QQ 收藏。

本地上传默认启用，直链仍可选择。上传限非空且不超过 8 MiB，按 `Image::mimeType` 检查原始字节，仅允许 GIF（含动图）、PNG、JPEG、WebP，不信任扩展名或客户端 MIME；不创建通用图片缓存文件。格式识别沿用现有签名检查，不保证图片可完整解码，最终接收仍由 QQ 决定。multipart 必须包含恰好一个名为 `image` 的文件；无效 multipart 返回 400，超限返回 413，其他失败沿用 JSON 错误。保留现有管理员认证和全局 25 MiB 请求上限。

传输依据 NapCat 官方 [DownloadFile.ts](https://github.com/NapNeko/NapCatQQ/blob/main/packages/napcat-onebot/action/go-cqhttp/DownloadFile.ts)（`base64` 字段接收标准 Base64）和 [CustomFace.ts](https://github.com/NapNeko/NapCatQQ/blob/main/packages/napcat-onebot/action/extends/CustomFace.ts)（`add_custom_face.file` 仅接受 NapCat 本地路径）：图片字节先经 `download_file.base64` 上传，再用返回的 `data.file` 收藏。不会把管理服务本机路径传给远程 NapCat。旧版本或其他 OneBot 实现可能不支持此扩展，上传失败时不调用收藏；协议端临时文件沿用 NapCat 生命周期管理。上传 Base64、收藏文件参数在日志及 HTTP 调试记录中脱敏。

弹窗保留失败后的文件或 URL 草稿，关闭或离开页面需确认放弃；请求进行中阻止重复提交和关闭。模拟集成测试额外覆盖实际 GIF 二进制、伪造 MIME/扩展名、空文件、截断内容、超限、错误字段、损坏 multipart、上传失败不收藏，以及缓存失效与 Base64 日志脱敏。未连接真实 QQ 验证。

回复决策形成后，同批后续工具不执行；只保留第一个有效回复决策。收藏表情按名称查找时，名称不唯一则拒绝执行，需先在后台区分名称。execution_state.current_time 为当前请求的服务端本地时间，定时参数也按该时区解析。

`send_face` 与 `at_user` 只生成 CQ 码，分类为 INFORMATION；插件归属仍为 builtin.action。
