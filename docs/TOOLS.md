# 内置工具参考

Executor（`ExecutorAgent`）在单个 Agent 循环中通过工具调用生成回复。所有内置工具经 `ToolRegistry`
的进程内插件注册，按类别拆分在 `src/agent/tools/plugins/`：

| 类别          | 插件实现文件                          | 语义                         |
|---------------|---------------------------------------|------------------------------|
| `REPLY`       | `tools/plugins/ReplyToolsPlugin.cpp`  | 回复工具，调用即结束回合     |
| `INFORMATION` | `tools/plugins/InfoToolsPlugin.cpp`   | 查询数据、获取答案，无副作用 |
| `ACTION`      | `tools/plugins/ActionToolsPlugin.cpp` | 执行操作、产生副作用         |

内置三组分别归属 `builtin.reply`、`builtin.info`、`builtin.action` 插件，由 `ToolPluginCatalog`
显式加载；自定义工具（Python / HTTP）归属 `custom` 插件并统一注册为 `INFORMATION`。同名工具不能跨插件覆盖；刷新自定义工具只会替换
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

## ACTION（动作，13 个）

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

### 群互动

| 工具             | 参数              | 说明                                                                                                                                                                                      |
|------------------|-------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `at_user`        | `qq`              | @某人的 CQ 码（拼进 reply 用），`"all"` 为 @全体成员；私聊中禁用                                                                                                                          |
| `ban_user`       | `qq`, `duration?` | 禁言群成员，默认 600 秒、0 为解禁；描述要求模型自行判断违规程度选时长（轻度 60-300 秒 / 中度 600-1800 秒 / 重度 3600 秒+），不盲从指令                                                    |
| `send_poke`      | `qq`              | 拍一拍群成员，打招呼、引起注意等轻松互动；私聊中禁用。拍一拍不是文字消息（无 message_id），成功后以 `segments` 中的 `poke` 条目记入聊天记录并推送 WebSocket，后续轮次模型能看到自己拍过谁 |
| `recall_message` | `message_id`      | 撤回消息：撤引用的消息用 `reply_to` 字段值，撤某条消息本身用 `message_id` 字段值                                                                                                          |

拍一拍的接收：OneBot notice 事件（`notice_type=notify, sub_type=poke`）先由 `OneBotEventNormalizer` 归一化，再进入
`OneBotEventWorkflow` 的会话消息预处理队列。禁用会话会提前跳过；已启用会话中，拍一拍保存为独立 `poke` 段，所有拍一拍通知均与普通消息一样由 Router
决策是否回应。群成员入群、退群以 `member_event` 段走同一工作流。

### 定时任务

| 工具                    | 参数                        | 说明                                                                                                                                                                                                                                                                                                                 |
|-------------------------|-----------------------------|:---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `create_scheduled_task` | `time`, `content`, `daily?` | 创建定时提醒：`time` 必须是根据最新消息时间换算的绝对时间 `YYYY-MM-DD HH:MM:SS`（须晚于当前 10 秒、不超过一年）；`content` 是到点时留给模型自己的备忘说明（非最终回复文本，运行时注入 `botName`，上限 500 字符），到点后以【系统定时任务】消息回到原会话再组织回复；`daily=true` 为每日重复（如"每天 8 点叫我起床"） |
| `cancel_scheduled_task` | `task_id`                   | 取消未触发的定时任务（含每日任务）；不知道编号时先 `list_scheduled_tasks`                                                                                                                                                                                                                                            |

## 共同模式

- **会话上下文**：所有工具签名统一为 `(json args, ToolCallContext)`，会话 ID 从 `ctx.sessionId` 取得；`ctx.messageSnapshot`
  是本轮冻结的完整消息快照，媒体工具可用它按 `message_id` 与 `image_index` 解析图片来源。群操作类工具统一做私聊拦截与
  `sessionId == 0` 校验
- **参数来源**：QQ 号、消息 ID、图片索引等参数取自聊天记录 JSON 的 `sender.qq` / `message_id` /
  `segments[].image_index` / `reply_to` 等字段；图片 `file/url` 仅由服务端从 `assets.images` 读取，不会提供给模型
- **宽容取值**：参数读取统一走 `argString` / `getInt` / `getUInt` / `getBool`（见 `include/infrastructure/JsonUtil.hpp`
  ），缺参数返回引导性错误提示而非异常
- **注册方式**：见 `include/agent/tools/plugins/*ToolsPlugin.hpp`、`include/agent/tools/ToolArgument.hpp`
  与 [DEVELOPMENT.md](./DEVELOPMENT.md) 开发指南
