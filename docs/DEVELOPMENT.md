# insoulforge 开发文档

面向开发者的构建、架构与贡献指南。使用说明见 [README](../README.md)，编码规范见 [CODING_STYLE.md](./CODING_STYLE.md)。

## 环境要求

以 [Dockerfile](../Dockerfile) 的 `builder` 阶段为准：它是发布镜像实际使用的、可复现的构建环境。CMake
配置阶段会查找下列依赖；缺失任一必需项会直接失败。

| 依赖                          | 要求                                           | CMake / 构建用途                                       |
|-------------------------------|------------------------------------------------|--------------------------------------------------------|
| CMake                         | 3.20 或更高                                    | 项目构建系统                                           |
| C++ 编译器                    | 支持 C++23，推荐 GCC 13+ 或同等 Clang + 标准库 | 后端代码使用 C++23 与 `<format>`                       |
| npm                           | Node.js 18+                                    | CMake 配置阶段查找 npm，`insoulforge` 目标自动构建前端 |
| Drogon                        | 1.9.10                                         | `find_package(Drogon CONFIG REQUIRED)`                 |
| SQLite3、spdlog、fmt、OpenSSL | 开发包                                         | 存储、日志、格式化、加密                               |
| nlohmann-json                 | 头文件或 CMake 包                              | JSON；CMake 找不到包时会直接查找头文件                 |
| libpng、giflib                | 开发包                                         | 图片与 GIF 解码                                        |
| Lua                           | 5.4 或更新的兼容版本                           | 进程内自定义工具执行器                                 |

`ninja` 不是必需依赖，但与 Dockerfile 一致，推荐作为 CMake 生成器使用。

### Ubuntu 24.04

先安装与 Dockerfile `builder` 阶段相同的系统依赖：

```bash
sudo apt update && sudo apt install -y \
    build-essential cmake ninja-build git ca-certificates \
    libsqlite3-dev libspdlog-dev libfmt-dev libjsoncpp-dev nlohmann-json3-dev \
    zlib1g-dev libssl-dev uuid-dev libgif-dev libpng-dev liblua5.4-dev \
    nodejs npm
```

Ubuntu 官方仓库不保证提供可供本项目使用的 Drogon CMake 包，因此按 Dockerfile 固定构建 Drogon 1.9.10：

```bash
git clone --depth 1 --branch v1.9.10 --recurse-submodules --shallow-submodules \
    https://github.com/drogonframework/drogon.git /tmp/drogon
cmake -S /tmp/drogon -B /tmp/drogon/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DOPENSSL_USE_STATIC_LIBS=ON \
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_CTL=OFF \
    -DBUILD_ORM=OFF \
    -DMYSQL_SUPPORT=OFF \
    -DLIBPQ_SUPPORT=OFF
cmake --build /tmp/drogon/build
sudo cmake --install /tmp/drogon/build
```

Ubuntu 22.04 默认编译器不满足本项目的 C++23 / `<format>` 要求。请升级到 GCC 13+ 后，在首次 CMake 配置时指定
`-DCMAKE_CXX_COMPILER=g++-13`。

### macOS（Homebrew）

```bash
brew install cmake drogon spdlog fmt nlohmann-json sqlite3 openssl brotli libpng giflib lua node
```

Homebrew 的 `drogon` 会提供 CMake 包。若 CMake 找不到 OpenSSL、SQLite3 等 Homebrew 前缀下的依赖，请在配置时传入对应的
`CMAKE_PREFIX_PATH`。

用 CLion 打开项目后，选择或创建 `cmake-build-debug/` 构建目录即可；CMake 配置和 `insoulforge` 构建会自动安装前端依赖并生成管理后台静态文件。

## 构建

### 一次性构建（发布）

```bash
cmake -S . -B cmake-build-release -DCMAKE_BUILD_TYPE=Release
cmake --build cmake-build-release -j    # Linux 可用 -j$(nproc)
```

构建产物：

- 可执行文件 → `build/insoulforge/exe/insoulforge`
- 前端静态文件 → `build/insoulforge/public/`（CMake 会在前端源码变化时自动执行 `npm run build`）

后端源码由 CMake 的 `${PROJECT_NAME}_backend` 对象库统一编译，主程序和契约测试复用同一组对象文件；新增后端 `.cpp`
仍由 `src/` 的自动发现规则纳入构建，无需分别维护两个目标的源文件清单。

### 日常开发

推荐使用 CLion（`cmake-build-debug/` 目录）或 IDE 内建 CMake 支持。VSCode 可安装 CMake Tools 与 clangd 插件。

**前端开发**建议单独启动 Vite dev server（带热更新），API 请求会代理到后端：

```bash
cd frontend && npm run dev
```

代理规则见 `frontend/vite.config.ts`：`/admin/api` → `http://localhost:7778`，`/admin/ws` → `ws://localhost:7778`。

### 仅检查前端类型

```bash
cd frontend && npm run type-check
```

## 运行

```bash
./build/insoulforge/exe/insoulforge
```

- HTTP 服务监听 **7778** 端口，管理后台：`http://localhost:7778/index.html`
- 数据目录 `data/` 与日志 `logs/bot.log` 在工作目录下生成；首次启动会自动创建全局配置文件 `data/config.json`
- 控制台输入 `exit` 优雅退出

注意：CMake 缓存位于 `cmake-build-*`，部署产物固定输出到仓库根目录的 `build/insoulforge/`。配置文件、数据库、日志和上传目录相对进程工作目录创建：
从仓库根目录运行时使用根目录的 `data/`、`logs/`、`uploads/`；在 `build/insoulforge/` 中运行时则使用该目录下的同名目录。

契约测试默认不参与常规构建。执行测试时先显式构建测试目标，再运行 CTest：

```bash
cmake --build cmake-build-debug --target insoulforge_message_contract_tests insoulforge_mcp_tests insoulforge_memory_tests -j8
ctest --test-dir cmake-build-debug --output-on-failure
```

记忆维护测试使用本机临时 HTTP 接口模拟模型，不访问外部服务；测试结束后自动关闭。

## 项目结构

```
insoulforge/
├── CMakeLists.txt            # C++23 构建脚本（含前端自动构建）
├── include/                  # 头文件（按模块分目录）
│   ├── workflow/             # 消息处理主干，组织预处理、路由和 Agent 执行
│   │   ├── access/           # 机器人管理员权限与全局用户黑名单
│   │   ├── agent/            # 工作流中的决策与执行环节
│   │   │   ├── ability/      # Agent 工具启动的后台耗时任务
│   │   │   ├── llm/          # 模型客户端、提示词和用量统计
│   │   │   ├── memory/       # 记忆召回、维护、好感度及存储
│   │   │   ├── runtime/      # AgentSystem / ExecutorAgent / AgentTypes
│   │   │   └── tools/        # 工具运行时、插件、自定义工具与 MCP 适配
│   │   │       └── image/    # 图片生成与角色参考图存储
│   │   ├── command/          # 命令识别与执行
│   │   ├── history/          # 聊天记录及其存储
│   │   ├── message/          # 统一消息模型、会话 ID、图片与记忆补全
│   │   │   └── image/        # 图片/GIF 识别与描述缓存，供预处理和工具复用
│   │   ├── scheduling/       # 定时任务的存储、恢复与工作流事件投递
│   │   └── session/          # 会话配置、存储、名称刷新与昵称目录
│   ├── admin/                # 管理后台接口与实时推送
│   │   ├── access/           # 后台访问令牌与会话 Cookie
│   │   ├── events/           # 管理后台状态推送
│   │   ├── http/             # 管理 API 控制器与响应工具
│   │   └── logging/          # 实时日志推送
│   ├── infrastructure/       # NumericTypes.hpp、配置、数据库、日志、HTTP 与共享工具
│   └── onebot/               # OneBot 接入
│       ├── incoming/        # OneBot 上报到内部消息记录的转换
│       ├── messaging/       # OneBot 发送文本的 CQ 码转换
│       └── transport/       # HTTP 事件入口与 WebSocket/API 客户端
├── src/                      # 源文件（按相同模块镜像组织，入口为 main.cpp）
├── frontend/                 # Vue 3 + Vite + TypeScript 管理后台
│   └── src/
│       ├── components/      # 跨页面复用的 UI 组件
│       └── features/        # 按访问、执行、会话、LLM、OneBot、工具、诊断与概览归类的页面
├── agentTools/               # 自定义工具 JSON（时间、天气、随机数及 PageWeave 网页读取/搜索）
└── docs/                     # 文档
```

`src/main.cpp` 组织一级模块的启动；`workflow/` 组织消息处理及其子模块。
`include/` 与 `src/` 保持相同层级，分别存放声明与实现。目录表示职责归属，
不要求包含关系严格成树；跨模块仍直接包含实际使用的接口。

`workflow/message/MessageService` 组织发送流程并将发送成功或失败记录写入上下文，
通过 `onebot/messaging/MessageFormatter` 转换文本，再调用 `OneBotClient` 发送。
会话名称查询与保存由 `workflow/session/SessionConfigManager` 负责。

## 架构

### 消息处理工作流

`OneBotEventWorkflow::instance()` 只取得对象，不恢复会话或启动维护任务。
启动入口显式调用 `initialize(database, config, agent)`，检查返回的 `std::expected<void, std::string>`，
成功后才启动 OneBot 接收与定时任务。数据库、配置和 Agent 必须已初始化，并覆盖工作流的生命周期。
恢复查询及消息列表使用传入的数据库与配置；同一组依赖重复初始化不会重复恢复消息或调度任务。
初始化仅限启动阶段调用，不与消息处理并发执行。

```
HTTP 模式：OneBot HTTP POST /
WebSocket 模式：OneBotWebSocketClient 接收事件，并按 echo 匹配动作响应
    │
    ▼
两种入口均投递已解析的 OneBot JSON（HTTP 入口会立即确认请求）
    │
    ▼
OneBotEventWorkflow::enqueueOneBotEvent
    │ 归一化 OneBot 消息、通知和机器人回显；Agent 不可用时立即结束
    ▼
消息预处理队列（每会话 FIFO，不同会话可并行）
    │ 1. 确保会话配置存在
    │ 2. 过滤全局黑名单用户；命中后直接结束
    │ 3. 识别并执行命令；命令写入消息列表后结束
    │ 4. 跳过未启用会话
    │ 5. 图片/GIF 识别与长期记忆召回
    │ 6. 写入 MessageList，推送管理后台并生成不可变快照
    │ 7. 空闲时投递回复队列；回复进行中仅 @机器人和系统任务排队
    ▼
回复队列（每会话最多一个执行任务）
    │ @机器人和系统任务在已有回复时排队；普通消息记为 AgentBusy，不再发起第二次请求
    ▼
MessageRouter
    │ 从冻结快照投影路由上下文；先执行硬规则
    │ 群聊可选走 Jev；低置信度、不明确或失败时调用 Router LLM
    │ 私聊直接使用原有 Router LLM 路径
    │ 输出 RouterDecision：SKIP / REPLY + 策略（语气、长度）
    ▼
ExecutorAgent
    │ 带工具调用循环生成回复
    │ deep_think 工具按需把复杂问题交给深度思考模型求解，答案作为工具结果回传后再组织回复
    ▼
MessageService → OneBot API
    │ 成功的助手消息重新写入 MessageList
    ▼
会话统计与异步记忆维护
```

关键数据结构见 `include/workflow/agent/runtime/AgentTypes.hpp`（`RouterDecision`、`ReplyDecision`）。

消息处理的几个约束：

- 黑名单检查先于命令与启用检查，命中用户的消息不会进入任何后续处理。
- 命令先于启用检查处理，因此管理员可以在禁用会话中继续使用 `/enable`、`/status` 等管理命令。
- `OneBotEventNormalizer` 生成统一 JSON。`segments` 是内容和顺序的唯一来源；图片来源仅保存在
  `assets.images[].source`，经 `MessageRecord::projectForAgent` 投影后不会发送给 LLM。
- 图片识别服务先下载原始媒体字节，按 SHA-256、视觉模型与提示词版本查询缓存，再以 Base64 Data URL 请求视觉模型。静态图片直接提交；GIF
  解码后最多提交 16 帧，超出时按播放时长均匀抽样并保留首尾帧。成功描述长期缓存，失败结果只缓存 10
  分钟；缓存和记录仅保存哈希、媒体类型、抽帧数与描述，不保存 Base64 内容。
- 同一会话的消息预处理阶段严格 FIFO：图片识别与向量召回完成后才写入消息列表和创建快照，因此后到消息不能改变已启动任务的上下文。
- 回复阶段与消息预处理阶段解耦。已有回复任务时，@机器人和系统任务进入回复队列，普通消息仅完成预处理、记录 `AgentBusy`
  ，不会打断正在生成的回复。
- `MessageList` 是 Router、Executor 和工具的唯一运行时消息源。它保存完整消息，但快照始终最多暴露
  `contextWindowLimit` 条近期消息。预处理结束后，若消息数达到或超过 `memorySummaryTriggerCount` 且没有执行中的维护任务，
  则选取最旧的 `memorySummaryBatchSize` 条启动内存维护任务；全部结果提交成功后才按条数删除队首消息。启动时从数据库恢复消息，正常退出时调用
  `flushToStorage()` 覆盖持久化恢复副本。
- `SharedConversationContext` 为 Executor 构建 `other_conversations`。会话共享开关保存在 `group_config.cross_session_sharing`，
  默认关闭，群聊、私聊双向互通；仅已启用且参与共享的会话可提供和接收。每来源默认读取 10 条（`memory.sharedConversationMessageCount`），
  限三天内且尚未被总结移除的消息，所有内容段共用 200 字符额度，超出标记 `truncated`。只读有限条数快照，不复制整份会话历史，
  不包含消息 ID、引用目标、媒体索引、工具执行历史或召回记忆；共享背景不能作为当前会话指令。回复期间保持快照不变，不额外限制全局预算。
- 拍一拍、入群、退群均归一化为类型化消息段：`poke`、`member_event`。拍一拍不再按参与者区分，所有拍一拍通知均与普通消息一样交由
  Router 决策。
- Router 的空快照、找不到触发消息、系统任务、@机器人和群聊刷屏/短文本规则先于 Jev；只有剩余群聊消息才进入 Jev。
  Jev 与原有 Router LLM 共用窗口起点和机器人静默状态计算，不修改冻结快照。Jev 使用压平的消息段，并保留
  `message_id` / `reply_to` 供引用关系判断。
- Jev 一次请求给出 `action`、`tone` 和 `maxLength`。仅当 `answers.action.choice` 为 `reply` / `skip` 且
  `answers.action.confidence` 有效并达到 `jevMinConfidence` 时采纳；`unclear`、低置信度、响应异常或请求失败均交给原有
  Router LLM。`probabilities.reply` 不是当前阈值的比较对象。Jev 请求超时为 10 秒；Router LLM 兜底仍保留原有
  fail-open 行为。
- 预处理、路由、执行或发送的异常均限制在当前消息或当前回复任务内，不能使同会话队列停滞。工作流不使用轮询或忙等待。

新增主处理步骤时，先确定其属于消息预处理阶段还是回复阶段，再在 `OneBotEventWorkflow` 的对应队列处理函数中调用一个职责单一的组件。
不得跨阶段持有锁或修改已交给回复队列的消息快照；需要扩展的后处理应在工作流中定义明确的调用边界。

### 工具系统

- `ToolRegistry` 以进程内插件管理工具，LLM 调用时按类别分组注入 prompt：
    - `REPLY`：回复工具（`reply` / `no_reply` / `reply_with_quote`），调用后结束本轮处理
    - `INFORMATION`：信息工具（`recall_memory` / `list_stickers` / `deep_think` / `list_scheduled_tasks`），获取数据
    - `ACTION`：动作工具（`send_face` / `send_image` / `send_sticker` / `save_sticker` / `rename_sticker` /
      `reply_and_continue` / `delete_sticker` / `at_user` / `ban_user` / `add_to_blacklist` / `send_poke` /
      `recall_message` / `create_scheduled_task` / `cancel_scheduled_task` / `demo_async_task`），执行操作
- 内置工具分为 `builtin.reply`、`builtin.info`、`builtin.action` 三个 `ToolPlugin` 实现，注册代码按类别拆分在
  `src/workflow/agent/tools/plugins/ReplyToolsPlugin.cpp` / `InfoToolsPlugin.cpp` / `ActionToolsPlugin.cpp`。`ToolPluginCatalog`
  显式组合并加载
  插件，`ToolRuntime` 不感知具体插件；自定义工具归属 `custom` 插件。重载某个插件只影响该插件的工具，工具名在
  全局唯一，冲突时拒绝注册，避免自定义工具覆盖内置工具后出现定义与执行不一致。
- 工具定义按“类别 → `promptOrder` → 工具名”稳定排序，避免重启或自定义工具刷新后改变 provider 的 prompt cache
  和模型偏好。私聊请求会在注入前排除 `GROUP_ONLY` 工具（当前为 `at_user`、`ban_user`、`send_poke`）。
- 自定义工具仅支持 Lua，存储于数据库同级的 `tools/` 文件夹，启动及后台刷新时加载。每次调用创建独立 Lua 状态，宿主网络和消息操作由协程桥接，详见 [TOOLS.md](./TOOLS.md)
- MCP 远程工具由 `agent/tools/mcp/McpTools` 适配，协议和 HTTP 客户端位于 `infrastructure/mcp`。
  在 config.json 的 mcp.servers 或后台“MCP 服务”页面配置名称和用途描述，异步发现并缓存，按服务插件注册。
  Executor 仅注入目录及查询/启用工具，具体工具在本次回复选中后的下一轮才开放；选择不跨会话或回复共享，
  执行时校验本轮授权和缓存版本。调用使用独立会话，最后一轮禁止 MCP 查询、启用和执行。
  数量限制在“执行配置”页设置。配置方式和当前 SSE 限制见 [MCP.md](./MCP.md)。
- 拍一拍、撤回、引用回复、表情包收发、定时任务均由上述工具实现，由 Executor 根据上下文自动决策调用；天气、搜索、随机数、时间等能力来自
  `agentTools/` 目录的可导入自定义工具

工具执行的几个约束：

- 工具循环上限由 `execution.maxToolRounds` 配置，默认 8、范围 1～100。一次回复在开始时读取并固定该值；
  同一轮可处理多个工具，最终模型回复也占一轮。达到上限仍无回复决策时返回空结果，不额外发起模型请求。
- 每轮复制当前请求上下文，在末尾追加一条 system 执行状态，说明当前轮次和后续剩余轮数；状态不进入持久上下文，
  不影响已有 assistant/tool 配对，也不会累积到下一轮或传给 `deep_think`。最后一轮仅提供 REPLY 类工具，
  `ToolCallContext.replyOnly` 同时在注册中心拒绝非回复处理器执行；上限为 1 时首轮即应用此规则。
- `reply` / `reply_with_quote` / `no_reply` 是回复工具，调用后结束本轮处理；它们在 `ExecutorAgent::processToolCalls`
  中被拦截，不走普通工具返回值路径。
- `ReplyDecision.toolHistory` 跨迭代累计工具名、脱敏参数和处理状态，成功发送时经 MessageService 附在助手记录上。
  发送失败同样保存尝试发送的正文和工具历史，额外添加字符串字段 `send_error`，保留 OneBot 错误码及说明；
  不生成平台消息 ID，成功记录不添加该字段。模型投影保留错误字段，避免把失败正文当作已送达内容。
  网络超时仅表示结果未知，不能据此推断被禁言；记录失败本身不触发新一轮回复或自动重发。
  MessageService 使用 `std::expected<u64, std::string>` 返回发送结果，工具直接把具体错误反馈给模型；
  工作流不再重复追加发送失败记录。助手消息构造函数接受可选消息 ID，缺失时直接省略该字段。
  不累计 `reply`、`reply_with_quote` 的调用参数，避免重复保存正文和引用目标；`no_reply` 与其他工具继续记录。
  `no_reply` 及有工具历史的流程失败通过 `appendAssistantExecutionRecord` 直接写入 MessageList，不进入入站队列。
  内部记录标记 `record_type=tool_execution` 和执行状态，没有 QQ 消息 ID；仅 Executor 使用包含历史的投影，
  Router、记忆和好感度仍使用默认投影。正常退出持久化和启动恢复沿用现有消息列表机制，不增加数据库表。
- `send_sticker` / `send_poke` / `reply_and_continue` 是中途动作，执行后本轮不结束，最终仍需用回复工具收尾；一次对话需要连续多条消息时，用
  `reply_and_continue` 发送前置消息，再用 `reply` 或 `no_reply` 收尾。
- `send_sticker` 与 `reply_and_continue` 通过 `MessageService` 直接发送，成功后自动写入聊天记录并推送
  WebSocket；主流程只负责发送最终文字回复。
- `deep_think` 是信息工具，不是全局思考模式。Executor 只在复杂问题需要额外推理时调用，工具结果再回到 Executor 组织成聊天回复。
- `add_to_blacklist` 复用全局黑名单存储；仅接受当前会话快照中的发送者 QQ 号，并拒绝管理员与机器人自身。
  工具描述要求先提醒、无效后拉黑并告知用户，但提醒状态未单独持久化，后端不强制校验提醒是否发生。
- 普通工具处理器抛出的异常由 `ToolRegistry` 转为带具体原因的工具结果，再交还 Executor；回复工具的参数错误由
  `ExecutorAgent` 处理。外部请求只回传限长的错误摘要，不将完整响应体或图片数据注入模型。
- `AsyncTaskManager` 按会话限制同时运行的后台耗时任务。工具立即返回任务状态；启动状态写入
  `MessageList`，不触发 Router。后台结果经 `MessageService` 发送；完成或失败会产生新的系统消息并触发回复，失败消息附带限长的错误原因。`demo_async_task` 仅用于显式测试，任务不持久化恢复。

添加生图等耗时能力时，在动作工具中先校验参数，再以统一会话 ID 调用 `AsyncTaskManager::start`，传入任务描述和返回
`drogon::Task<AsyncTaskManager::Result>` 的处理器。结果的 `content` 是最终可发送的文本或 CQ 码，可附带图片描述；抛异常、返回空内容或发送失败均视为失败。将
`StartResult` 的启动、忙碌或退出状态转换为工具结果交还 Executor，不在工具处理器中等待后台完成。处理器只按值捕获任务所需数据，
不得跨挂起点引用工具调用栈上的对象。后台结果由管理器发送，完成或失败的系统消息也由管理器加入入站工作流；工具自身不要重复注入事件。

`HttpUtil::send` 返回 `std::expected<HttpResponsePtr, std::string>`：网络、地址或超时失败在 `error()` 中，收到的 HTTP 4xx/5xx 仍是有效响应，由调用方检查状态码。调试记录保持原有脱敏规则；对模型只回传必要、限长的错误详情。

此管理器与 `TaskScheduler`（到点触发）及 `MemoryMaintenanceCoordinator`（聊天记忆与好感度维护）用途不同。每个会话仅有一个
运行中的耗时任务，不同会话可以并行；正常退出时 `main` 在 `MessageList` 落库前调用 `stop()` 记录中断，但不会等待处理器
完成。需要重启恢复、取消或进度查询的实际能力应另外设计持久化状态，不能假设该接口已有这些保证。

### 记忆维护

记忆模块包括聊天记忆和好感度：前者保存对话中值得记住的信息，后者保存机器人对用户的互动印象。
统一入口在一次调用中提取人物记忆与全局好感度，再沿用现有会话记忆整理流程，全部成功后一起保存。

- **短期记忆**：`MemoryManager` 按统一会话 ID 从 SQLite 原文本列读取会话记忆；新内容为 `aliases` 和 `recent_events` JSON 对象，
  Executor 通过 `MemoryStore::projectShortTermMemory` 直接注入对象，旧纯文本在成功整理前仍按行注入。后台沿用原编辑入口，返回格式化 JSON。
  `MemoryStore::normalizeSessionMemory` 统一校验结构并合并重复 QQ、称呼和事件，不限制条数。
  称呼只属于本会话，保留已有值，有明确纠正时才更新，不采纳一次性调侃；事件更新进展，模型可根据完成状态和后续价值移除过时内容，无固定过期天数。
  新消息的个人属性不写入会话记忆，也不清理全局人物档案或好感度；旧文本在下一次成功整理时转换，无独立数据库迁移。
  `legacy_entries` 是旧内容过渡区，首次转换时由程序保留全部旧文本条目并继续注入上下文。后续模型只能从已有原文条目中选留或删除，
  依据新证据、重复程度及相关性逐步遗忘，不设固定期限；不得新增或改写，遗漏该字段时保留原值。旧两字段结构兼容为空过渡区，无法恢复此前已丢失的数据。
- **长期记忆**：`LongTermMemory` 封装 `long_term_memory` 表的读取与向量检索；embedding 以 `f32` 数组存入 BLOB。
- **维护批次**：工作流在每条消息预处理结束后检查消息列表；达到或超过 `memorySummaryTriggerCount` 且没有待处理总结任务时，
  选取最旧的 `memorySummaryBatchSize` 条消息作为待总结内容，并复制随后最多
  `memorySummaryContextCount` 条只读上下文。待总结消息在任务成功前继续保留在列表中。
  追加机器人回复或内部记录不会单独触发总结；任务成功收尾只清理已总结消息，下一批由后续消息预处理触发。
  启动时不恢复维护任务，也不为历史消息主动创建新批次。
- **内存任务**：`MemoryMaintenanceCoordinator` 调用 `MemoryMaintenanceService` 联合提取人物条目和好感度，再整理旧会话记忆；全部成功后，
  由 `MemoryMaintenanceStore` 在同一 SQLite 事务中保存结果。任务本身不写入数据库，不保存完成状态或重试次数。
  数据库升级到 v18 时删除旧维护任务表，不迁移未完成任务；已有消息、记忆和好感度结果保留。
- **聊天记忆总结**：`MemoryMaintenanceService` 的整理请求直接接收当前会话记忆、待总结消息和只读补充上下文，
  没有新增人物记忆或只有好感度变化时也执行会话总结；补充上下文不得作为新记忆来源。复用原整理请求，不增加第三次模型调用。
  长期记忆仍只依据原来的新人物记忆及召回材料处理，无新人物条目时不触发向量召回或长期写入。按 `longTermRecallThreshold`
  召回相似长期记忆，用于合并、去重和替换；计算期间不写入结果。
  未配置向量模型时只维护短期记忆；已配置但向量化失败时，本批次不提交，等待后续消息重新触发。
- **人物记忆**：`person_memories` 按 QQ 保存 `stable_memory` 和 `recent_interactions` 字符串数组；
  `person_memory_entries` 保存追加的待整理条目、来源会话和 UTC 写入时间。群聊与私聊共用，不受原始消息共享开关影响。
  `PersonMemoryConsolidator` 每分钟按本地日历检查凌晨 03:00 的每日任务，使用 Memory 模型逐人处理有待整理条目的人，不重叠执行。
  v22 新增 `person_memory_daily_state`，记录完成日期和进行中日期，启动合并补跑错过的日期；首次启用在 03:00 前不补昨天。
  固定记忆合并重复、修正明确冲突，近期互动按相关性压缩遗忘，无固定时间淘汰；没有新条目的人不会额外调用模型。
  请求期间不持数据库锁，校验成功后事务更新档案并按快照最大 ID 删除旧条目，不消费期间新增的内容；单人失败不影响其他人。
  一轮结束记录当天完成，失败条目下一日重试；中断不标完成，重启仅消费剩余条目。停止与读写互斥，迟到响应不能在数据库关闭后提交。
  每日整理向 `LlmClient::requestMaintenance` 传递共享取消状态；取消与模型用量入库使用同一互斥保护，停止后丢弃迟到响应及其用量，不等待网络超时。
  人物昵称由 `PersonNameResolver` 后台查询，最多四路并发、同 QQ 去重，超时 5 秒、失败冷却 60 秒；接口立即返回缓存及 `name_pending`。
  前端有待查询昵称时每 2 秒刷新概要，不重新获取记忆正文，离开页面取消轮询和请求；进程退出时昵称队列停止接单并丢弃迟到结果。
  `PersonMemoryStore` 只批量读取 `recent_conversation` 中去重后的真实发送者档案，不加载仅被提及、其他会话出现或没有档案的人。
  上下文顺序为 `session → short_term_memory → earlier_conversation → recent_conversation → person_memories → other_conversations → response_requirements`；
  新条目也直接提供给模型，一次回复期间固定快照。旧自由文本记忆不猜测归属、不自动迁入人物档案。
- **好感度评估**：联合提取返回独立的 `memories` 和 `affinity_changes` 数组，允许两者为空或涉及不同用户。
  QQ 必须属于待提取消息的真实发送者，补充上下文不可作为新增记忆或评分来源；重复人物条目去重，重复评分或无效归属使批次失败。
  每批变化限制为 [-5, 5]，`person_affinity` 按 QQ 原子累计并限制在 [-100, 100]。v21 将旧 `group_affinity` 分数按 QQ 平均并四舍五入后迁移，
  不把没有记录的会话当作零分；后台沿用原查询路径，但列表、弹窗和群聊/私聊上下文均使用全局分数。
- **任务收尾**：同一会话最多执行一个维护任务。全部成功并提交后，按批次条数删除消息列表队首；失败保留消息并释放执行状态，
  等下一条消息重新触发，不自动循环重试。重启后不恢复任务，后续消息会根据恢复的消息列表重新触发。
- **清空消息**：不会取消执行中的维护任务；旧任务成功后仍按原批次数量删除队首，可能删除清空后收到的新消息。
  前端清空确认框会提示此行为，不影响群内实际发送的消息。
- 人物记忆与好感度联合提取、旧记忆整理使用独立 memory 模型（`LlmClient::requestMaintenance`，采用 memory 输出上限，不再单独发起 256 token 的评分请求）；向量化使用独立 embedding 配置（
  `LlmClient::requestEmbedding`）。`recall_memory` 工具按余弦相似度（阈值 0.3）检索长期记忆，Router 另有
  `routerWindowTriggerCount` / `routerWindowKeepCount` 子窗口参数。
- **被动召回**：消息预处理阶段从文本段和成功图片描述构建查询，命中结果直接写入当前完整消息的 `memories` 字段；该字段随快照进入
  Executor
  上下文，不依赖独立缓存或聊天记录读取时的二次注入。

### 配置系统

群消息的 `sender.card` 保存 OneBot 上报的群名片，与 `sender.name`（QQ 昵称）独立；空群名片和私聊不保存该字段。

群消息的 `sender.role` 保留 OneBot 上报的群身份：`owner` 为群主，`admin` 为群管理员，`member` 为普通成员。
该字段随消息发送到模型上下文，不受群名片开关影响；缺失、无效身份和私聊不添加该字段，旧消息不补造身份。
群身份不等同于机器人管理员权限，不能作为工具操作的授权依据。
每个群的 `group_config.send_group_card` 默认关闭，在后台“会话管理”或群内管理员命令 `/groupcard on|off` 中设置。
消息记录始终保留已有群名片；构建模型上下文时读取本群设置，并传入 `MessageRecord::projectForAgent`，
按开关保留或移除 `card`，不修改原始消息。旧记录没有该字段时不补造，私聊不发送群名片。
数据库 v14 → v15 自动补充字段，保留消息统计；禁用及重新启用群聊不清除该设置。

`group_config.auto_reply` 控制每个群的普通消息自动回复，默认开启。数据库 v15 → v16 自动新增字段，保持旧群行为。
Router 在系统消息和明确 @ 机器人规则之后、Jev/Router 模型请求之前检查开关；关闭时直接返回 SKIP，
不影响入站消息保存、上下文和记忆维护。私聊不使用此开关，已开始的回复也不取消。
后台通过 `POST /admin/api/group/{groupId}/auto-reply` 保存布尔值 `autoReply`，群内管理员通过 `/autoreply on|off` 修改。
开关单独更新，不改变群名片设置、消息统计和会话启用状态。

`group_config.command_enabled` 控制每个群是否解析管理命令，默认开启，数据库 v18 → v19 自动补充。
后台“会话管理”的“命令解析”开关通过 `POST /admin/api/group/{groupId}/command-enabled`
保存布尔值 `commandEnabled`。关闭后，包含 @ 机器人及 `/` 前缀的消息也按普通消息处理，
仍受会话启用状态和原有回复规则约束；不执行管理操作，不改变私聊命令行为。
该设置仅在后台修改；关闭后需要从后台重新开启，群内 `/enable` 等管理命令也不再生效。

`group_config.additional_prompt` 保存每群独立的附加回复提示词，默认空字符串，UTF-8 内容最多 16000 字节。
数据库 v16 → v17 自动新增字段并保留已有统计和开关。Executor 在构建本次回复的第一条 system 消息时，
将该内容附加到全局人设和执行约束之后；默认模型和兜底模型共用这份消息，下一次回复读取最新设置。
它不替换全局提示词，不传给 Router/Jev 或记忆维护，也不应用于私聊。
`GET/POST /admin/api/group/{groupId}/prompt` 读取或保存字符串 `additionalPrompt`，空字符串清空设置；
群列表仅返回 `hasAdditionalPrompt`，避免批量传输全文。此设置仅通过管理后台编辑，不提供群命令。

`ConfigStore` 将 LLM API 配置（router / jev / executor / executorFallback / executorThinking / image / imageGeneration / memory / embedding）、QQ Bot 配置、记忆和执行参数统一写入
`data/config.json`。聊天模型可设置 `maxTokens`、`temperature`、`topP` 和 `reasoningEffort`；Embedding、Jev 与图片生成不使用这些采样参数。
后台上传的角色参考图单独保存在 `data/character-image`，不写入配置或公开静态目录；部署时沿用现有 `data/` 持久化挂载。
`main` 只调用 `Config::initialize("data/config.json")`，统一完成文件读取、迁移、类型转换和运行时加载。
`ConfigStore` 是配置系统内部的持久化实现，业务代码和管理接口通过 `Config` 读写配置，不分别初始化两层。
初始化和保存接口返回 `std::expected<void, ConfigError>`，调用方必须检查结果；参数错误与文件操作错误分别标识。
读取接口要求初始化已成功，不会自行创建或选择配置文件；调试构建通过断言检查这个前置条件。
初始化或保存失败不替换已有内存状态，管理接口保存失败不应用新配置或重连 OneBot。
配置先转换到临时对象，转换和校验通过后才提交文件与运行时值。整数字段含小数或溢出、负 QQ 号、
非有限数值和越界采样参数返回配置错误；启动时原有缺失字段与旧字段修复规则保持不变。
后台保存通过 `Config` 串行完成校验、持久化和运行时更新，控制器不再逐项修改配置成员。
保存只更新对应配置域的运行时字段；执行参数保存仍仅写入原子值，不重写模型或 OneBot 配置。
启动期的日志、管理令牌、数据库和 Agent 初始化接口返回 `std::expected<void, std::string>`，
`main` 检查每一步的结果，失败时输出原因并退出，不启动后续服务。日志尚未就绪时使用标准错误输出；
文件日志不可用仍允许降级为控制台日志。数据库迁移错误在初始化边界转换为返回值并关闭新连接；
Agent 仅在工具和提示词加载完成后标记为就绪，工具重载失败也通过返回值报告。
启动初始化必须显式传递实际依赖：`AgentSystem::initialize(database, config.botName)` 将已初始化的数据库
传给自定义工具查询和提示词初始化，将机器人名称传给内置工具注册。定时任务工具说明使用传入名称，
不在注册阶段读取全局配置。运行时提示词接口仍沿用进程级数据库，初始化流程使用显式数据库重载。
Jev 默认指向 OpenRouter Decisions API，API Key 默认为空，因此默认不启用；其 `minConfidence` 默认 0.6，限制在 0～1，
可在管理后台修改并即时更新运行时配置。启动时若文件不存在则创建默认配置；若
JSON 损坏则备份为 `config.json.broken.<时间戳>` 后重建；缺失或类型不匹配的字段会补默认值并回写。管理后台保存时先写入临时文件，再原子替换原文件。

`llm.executorFallback` 是可选的回复兜底模型，配置结构与 `executor` 相同，默认留空。每轮先请求默认 Executor；网络错误、HTTP 错误或无效响应导致现有重试失败后，使用相同消息和工具列表请求兜底模型。兜底不额外消耗工具迭代轮数，不重复执行已完成的工具；下一轮及下一次回复仍先请求默认模型。两者均失败时结束本次回复流程，用量以 `executorFallback` 单独统计。

`Config` 单例提供业务使用的运行时配置。提示词由 `PromptService` 管理（`executor_system` /
`router_system`），支持 `{botName}` 占位符，修改后写回数据库。

执行参数位于独立的 `execution` 节点，管理接口为 `GET/POST /admin/api/execution-config`，前端页面为“执行配置”。
首项 `maxToolRounds` 只接受 1～100 的整数；启动时缺失或非法值修复为 8，接口保存非法值返回 HTTP 400。
文件写入成功后才更新原子运行时值，写入失败返回 HTTP 500 并保留原配置；保存时保留其他执行字段。
每次回复固定读取一次上限，后台修改不打断正在运行的循环。新增同类执行参数时，在这个节点、接口和页面中扩展，
不用添加到 LLM 采样参数或自定义工具脚本设置里。

**用量统计**：聊天模型与 Jev 调用通过 `LlmClient::logUsage` 记录模型与 token 用量；Jev 客户端先将
`input_tokens` / `output_tokens` 映射到现有记账字段，角色记为 `jev`。后台「用量统计」页读取 `/admin/api/usage` 展示。

### 数据库

SQLite 文件位于 `data/insoulforge.db`（`Database`
单例，以读写锁保护）。存储包括完整消息恢复副本、短期/长期记忆、好感度、提示词、会话配置、管理员、全局 QQ
黑名单和表情。全局运行配置与自定义工具不属于数据库，分别位于同目录的
`config.json` 和 `tools/`。

调试时可用任意 SQLite 客户端查看：

```bash
sqlite3 data/insoulforge.db ".tables"
```

## 开发指南

### 添加 C++ 内置工具

新增后端代码中的数值类型使用 `infrastructure/NumericTypes.hpp` 定义的 `i*`、`u*`、`f32`、`f64`；容器大小和索引使用 `size_t`
。完整规则见 [编码规范](./CODING_STYLE.md#数值类型)。

小型工具可直接按类别编辑 `src/workflow/agent/tools/plugins/ReplyToolsPlugin.cpp` / `InfoToolsPlugin.cpp` /
`ActionToolsPlugin.cpp` 注册（共享的参数 Schema 辅助函数在 `include/workflow/agent/tools/ToolArgument.hpp`）。内置文件由对应的
`builtin.*` 插件加载，因此工具会自动归属到该插件：

```cpp
registry.registerTool(
    {
        .name = "my_tool",
        .description = "工具描述，LLM 据此判断何时调用",
        .parameters = paramsJson,   // JSON Schema 格式
        .handler = [](const json args, ToolCallContext ctx) -> drogon::Task<std::string> {
            co_return "结果";
        },
    }, ToolCategory::ACTION);
```

新增独立能力域时，实现 `ToolPlugin` 并在 `src/workflow/agent/tools/ToolPluginCatalog.cpp` 的实例列表中增加该类；不需要修改
`ToolRuntime`、`AgentSystem` 或 `ExecutorAgent`。插件 ID 应使用稳定、全小写的命名空间形式。插件重载只会先卸载
自己此前的工具，跨插件同名工具会被拒绝：

```cpp
class WeatherToolsPlugin final : public ToolPlugin {
public:
    std::string_view id() const noexcept override { return "weather"; }

    void registerTools(ToolRegistry &registry) const override {
        registry.registerTool(myWeatherTool, ToolCategory::INFORMATION);
    }
};

// ToolPluginCatalog.cpp
const WeatherToolsPlugin weatherTools;
// 将 &weatherTools 加入 plugins 指针列表
```

不要把“工具排在前面”当作调用策略。顺序对部分模型有弱影响，因此系统保证顺序稳定；更有效的做法是按会话能力筛选、合并重叠工具，并在名称、描述和参数
Schema 中写清楚触发条件与边界。需要调整同类别展示位置时才设置 `Tool::promptOrder`，数值越小越靠前。

### 添加自定义工具（Lua）

管理后台填写名称、描述、JSON Schema 和 Lua 脚本，定义 `run(args, ctx)` 返回工具结果字符串。脚本可调用
`bot.send_message(text)`、`bot.start_task(description, payload)`、异步 `http.request(options)`，以及 JSON 编解码和 URL 编码接口；后台任务另定义 `background(payload, ctx)`。
详情和返回结构见 [TOOLS.md](./TOOLS.md)。测试接口模拟宿主操作，保存前校验脚本；新脚本无需重新编译或重启程序。

### 添加管理 API

1. 在 `include/admin/http/AdminController.hpp` 中声明路由与 handler
2. 在 `src/admin/http/AdminController.cpp` 中实现（协程写法 `Task<HttpResponsePtr>` + `co_return`）
3. 数据访问统一通过 `Database` 单例

现有路由以 `/admin/api/` 为前缀（聊天记录、LLM 配置、提示词、表情、群、管理员、黑名单、自定义工具、记忆、QQ 配置、用量统计），新路由建议沿用该前缀。

### 管理后台认证

程序启动时由 `AdminAccessToken` 使用安全随机数生成当前进程有效的访问令牌，并为当前进程网络命名空间中全部启用的 IPv4
地址输出自动登录链接。令牌放在 URL 片段 `#token=...`
中，浏览器不会把它发送到服务端；前端验证后会立即清除片段。令牌不写入配置文件、数据库或浏览器存储；重启后旧令牌和登录会话都会失效。Docker
默认桥接网络无法枚举宿主机 LAN IP，需将链接中的容器地址替换为宿主机局域网地址。

- `POST /admin/api/auth/login`：提交 `{"token":"..."}`，验证成功后写入 `HttpOnly`、`SameSite=Strict` 会话 Cookie。
- `POST /admin/api/auth/logout`：清除当前会话 Cookie。
- `GET /admin/api/auth/status`：返回当前请求是否已认证。
- 仅登录和认证状态接口公开；其他 `/admin/api/*` 请求（包括退出登录）、`/admin/ws` 和 `/admin/logs/ws` 均由服务端鉴权，未经认证返回 `401`。

启动入口注册 `AdminAccessToken::checkRequestAccess` 为 Drogon 路由前回调；路径判断、公开接口放行和未授权响应统一由访问控制模块处理。

新增后台接口时无需在 Controller 中重复校验 Cookie，但必须使用受保护的 `/admin/api/` 路径。若确实需要公开接口，应在
`AdminAccessToken::checkRequestAccess` 的认证白名单中显式声明，并审查其是否会泄露配置或运行数据。

### 添加前端页面

1. 在 `frontend/src/features/<功能模块>/` 新建页面；仅通用 UI 组件放在 `frontend/src/components/`
2. 在 `frontend/src/App.vue` 注册导航
3. API 请求路径以 `/admin/api` 开头（dev 模式自动代理到后端）

### 修改数据库表结构

数据库初始化由 `Database::initialize()` 调用 `SchemaMigrator` 完成。修改表结构时，除了更新建表语句，还必须在
`SchemaMigrator` 中加入带版本号的增量迁移；迁移应能安全处理已存在的表、列和数据，不能只依赖新数据库的建表结果。

## 调试

- **进程内存**：首页显示 RSS、启动以来峰值和线程数，以及最近 24 小时的 RSS 曲线。
  `ProcessMetrics` 使用 Drogon 定时器每 60 秒采样，启动时立即采样，不依赖是否打开后台；
  Linux 读取 `/proc/self/status`，macOS 使用 Mach 与 `getrusage`，不启动外部命令或新增采样线程。
  采样保存在最多 1440 点的固定容量环形缓冲区，重启清空，不写数据库。
  管理接口 `GET /admin/api/process-metrics` 返回 `sampleIntervalSeconds`、`latest` 和 `history`；
  每个点包含 Unix 毫秒 `timestamp`、字节数 `rssBytes`、`peakRssBytes` 以及 `threads`。
  顶部当前值通过 `GET /admin/api/process-metrics/current` 每 5 秒实时读取，只返回一个采样对象，
  不写入历史缓冲区；历史接口每分钟刷新。离开首页后停止前端轮询，后端分钟采样继续。
  无法采集的字段为 `null`，曲线不将缺失值画为零。RSS 包含共享映射，不代表独占内存或泄漏；
  分钟采样可能遗漏短暂峰值，峰值字段是进程启动以来的峰值，不是滚动 24 小时峰值。
  曲线横轴按已有历史自适应，最长显示 24 小时；只有一个采样时居中显示，不补造历史点。
- **日志**：业务代码只能调用 `Logger`，每条记录显式传入 `sessionId`、来源字符串和内容：
  `Logger::info(sessionId, "Executor", content)`。系统日志的 `sessionId` 固定为 `0`。日志系统统一生成
  `[level][session_id][source][content]`，并同时输出到控制台、`logs/bot.log`、`LogBuffer` 和后台日志 WebSocket。
  `LogBuffer` 启动时从滚动文件恢复最近 5000 条；旧格式日志以 `Legacy` 来源保留可查看性。排查消息流水线问题时优先按会话和
  `Router`、`Executor` 来源筛选。
- **协程**：所有异步 I/O 使用 `drogon::Task<T>` / `co_await`，注意 `co_await` 后对象生命周期（捕获 `shared_ptr` 而非裸指针）
- **会话并发**：`OneBotEventWorkflow` 为每个会话分别维护消息预处理队列、回复队列和 `MessageList`。不要在外部直接并发修改
  `MessageList`，也不要跨 `co_await` 持有其内部或工作流内部锁。
- **前端**：`npm run dev` + 浏览器 DevTools；后端日志会打印收到的 OneBot 原始 JSON

## 代码规范

完整规范见 [CODING_STYLE.md](./CODING_STYLE.md)，要点：

- 头文件 `.hpp`、源文件 `.cpp`、类 PascalCase、方法 camelCase、成员 `m_` 前缀、静态 `s_` 前缀
- 包含顺序：标准库 → 第三方 → 项目头文件
- 格式化使用 `.clang-format`（Google 风格，4 空格缩进，120 列），提交前建议运行 clang-format 与 clang-tidy
- 单例统一 `static ClassName& instance()` + 私有构造
