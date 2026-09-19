# insoulforge 开发文档

面向开发者的构建、架构与贡献指南。使用说明见 [README](../README.md)，编码规范见 [CODING_STYLE.md](./CODING_STYLE.md)。

## 环境要求

| 依赖                                   | 版本要求            | 说明                                                 |
|----------------------------------------|---------------------|------------------------------------------------------|
| CMake                                  | ≥ 3.20              | 构建系统                                             |
| GCC / Clang                            | GCC 13+ / Clang 14+ | 代码使用 `<format>`，GCC 11/12 不支持                |
| Node.js + npm                          | 任意较新版本        | 必需：CMake 配置阶段查找 npm，后端构建会连带构建前端 |
| Drogon                                 | 1.8+ 推荐           | 异步 Web 框架                                        |
| spdlog / fmt / nlohmann-json / SQLite3 | 任意较新版本        | 日志、格式化、JSON 与存储                            |
| libpng / giflib                        | 任意较新版本        | 图片与 GIF 解码                                      |

**Ubuntu 24.04（推荐，apt 开箱即用）**：

```bash
sudo apt update && sudo apt install -y \
    cmake g++ \
    libdrogon-dev \
    libspdlog-dev \
    libfmt-dev \
    libsqlite3-dev \
    nlohmann-json3-dev \
    libpng-dev \
    libgif-dev \
    libssl-dev \
    libbrotli-dev \
    zlib1g-dev \
    libuuid1
```

**Ubuntu 22.04** 默认 g++ 11 不支持 `<format>`，需额外安装 GCC 13：

```bash
sudo add-apt-repository ppa:ubuntu-toolchain-r/test
sudo apt update && sudo apt install -y g++-13
# 构建时指定编译器
cmake -DCMAKE_CXX_COMPILER=g++-13 ..
```

**macOS（Homebrew）**：

```bash
brew install cmake drogon spdlog fmt nlohmann-json sqlite3 openssl brotli libpng giflib node
```

用 CLion 打开项目直接构建（`cmake-build-debug/`），无需额外配置。

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
- 数据目录 `data/` 与日志 `logs/bot.log` 在工作目录下生成
- 控制台输入 `exit` 优雅退出

注意：CMake 缓存位于 `cmake-build-*`，部署产物固定输出到仓库根目录的 `build/insoulforge/`。数据库、日志和上传目录相对进程工作目录创建：
从仓库根目录运行时使用根目录的 `data/`、`logs/`、`uploads/`；在 `build/insoulforge/` 中运行时则使用该目录下的同名目录。

契约测试默认不参与常规构建。执行测试时先显式构建测试目标，再运行 CTest：

```bash
cmake --build cmake-build-debug --target insoulforge_message_contract_tests -j8
ctest --test-dir cmake-build-debug --output-on-failure
```

## 项目结构

```
insoulforge/
├── CMakeLists.txt            # C++23 构建脚本（含前端自动构建）
├── include/                  # 头文件（按模块分目录）
│   ├── agent/
│   │   ├── ability/          # 定时任务及其存储
│   │   ├── memory/           # Agent 记忆的查询、召回与存储
│   │   ├── runtime/          # AgentSystem / ExecutorAgent / AgentTypes
│   │   └── tools/            # 工具运行时、插件与工具存储
│   ├── admin/                # 后台 HTTP、实时 WebSocket 与后台数据
│   ├── conversation/
│   │   ├── history/          # 聊天记录及其存储
│   │   ├── maintenance/      # 派生状态维护任务协调、记忆维护与好感度维护
│   │   ├── message/          # 统一消息模型与会话 ID
│   │   ├── session/          # 会话配置、存储与昵称目录
│   │   └── workflow/         # OneBot 消息处理工作流
│   ├── infrastructure/       # 配置、数据库、日志、HTTP 与共享工具
│   ├── llm/                  # LLM 客户端、提示词与用量存储
│   ├── media/                # 图片/GIF 识别与描述缓存
│   └── onebot/               # OneBot 客户端、消息发送与 HTTP 入口
├── src/                      # 源文件（按相同模块镜像组织，入口为 main.cpp）
├── frontend/                 # Vue 3 + Vite + TypeScript 管理后台
│   └── src/components/       # 13 个功能组件（Dashboard、LLM配置、提示词、自定义工具、表情库、管理员、群管理、运行日志、请求调试、记忆与上下文、OneBot配置、用量统计、关于）
├── agentTools/               # 自定义工具的 JSON 配置（random / get_time / get_weather / search_web）
└── docs/                     # 文档
```

## 架构

### 消息处理工作流

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
    │ 2. 识别并执行命令；命令写入消息列表后结束
    │ 3. 跳过未启用会话
    │ 4. 图片/GIF 识别与长期记忆召回
    │ 5. 写入 MessageList，推送管理后台并生成不可变快照
    │ 6. 空闲时投递回复队列；回复进行中仅 @机器人和系统任务排队
    ▼
回复队列（每会话最多一个执行任务）
    │ @机器人和系统任务在已有回复时排队；普通消息记为 AgentBusy，不再发起第二次请求
    ▼
MessageRouter
    │ 从冻结快照投影路由上下文；硬规则优先，必要时调用 Router 模型
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

关键数据结构见 `include/agent/runtime/AgentTypes.hpp`（`RouterDecision`、`ReplyDecision`）。

消息处理的几个约束：

- 命令先于启用检查处理，因此管理员可以在禁用会话中继续使用 `/enable`、`/status` 等管理命令。
- `OneBotEventNormalizer` 生成统一 JSON。`segments` 是内容和顺序的唯一来源；图片来源仅保存在
  `assets.images[].source`，经 `MessageRecord::projectForAgent` 投影后不会发送给 LLM。
- 图片识别服务先下载原始媒体字节，按 SHA-256、视觉模型与提示词版本查询缓存，再以 Base64 Data URL 请求视觉模型。静态图片直接提交；GIF
  解码后最多提交 16 帧，超出时按播放时长均匀抽样并保留首尾帧。成功描述长期缓存，失败结果只缓存 10
  分钟；缓存和记录仅保存哈希、媒体类型、抽帧数与描述，不保存 Base64 内容。
- 同一会话的消息预处理阶段严格 FIFO：图片识别与向量召回完成后才写入消息列表和创建快照，因此后到消息不能改变已启动任务的上下文。
- 回复阶段与消息预处理阶段解耦。已有回复任务时，@机器人和系统任务进入回复队列，普通消息仅完成预处理、记录 `AgentBusy`，不会打断正在生成的回复。
- `MessageList` 是 Router、Executor 和工具的唯一运行时消息源。它保存完整消息，但快照始终最多暴露
  `contextWindowLimit` 条近期消息。达到 `memorySummaryTriggerCount` 时，它选取最旧的
  `memorySummaryBatchSize` 条创建总结任务；任务完成后才删除这批消息。启动时从数据库恢复，正常退出时调用
  `flushToStorage()` 覆盖持久化恢复副本。
- 拍一拍、入群、退群均归一化为类型化消息段：`poke`、`member_event`。拍一拍不再按参与者区分，所有拍一拍通知均与普通消息一样交由 Router 决策。
- 预处理、路由、执行或发送的异常均限制在当前消息或当前回复任务内，不能使同会话队列停滞。工作流不使用轮询或忙等待。

新增主处理步骤时，先确定其属于消息预处理阶段还是回复阶段，再在 `OneBotEventWorkflow` 的对应队列处理函数中调用一个职责单一的组件。
不得跨阶段持有锁或修改已交给回复队列的消息快照；需要扩展的后处理应优先消费持久化的维护任务，或在工作流中定义明确的调用边界。

### 工具系统

- `ToolRegistry` 以进程内插件管理工具，LLM 调用时按类别分组注入 prompt：
    - `REPLY`：回复工具（`reply` / `no_reply` / `reply_with_quote`），调用后结束本轮处理
    - `INFORMATION`：信息工具（`recall_memory` / `list_stickers` / `deep_think` / `list_scheduled_tasks`），获取数据
    - `ACTION`：动作工具（`send_face` / `send_image` / `send_sticker` / `save_sticker` / `rename_sticker` /
      `reply_and_continue` / `delete_sticker` / `at_user` / `ban_user` / `send_poke` / `recall_message` /
      `create_scheduled_task` / `cancel_scheduled_task`），执行操作
- 内置工具分为 `builtin.reply`、`builtin.info`、`builtin.action` 三个 `ToolPlugin` 实现，注册代码按类别拆分在
  `src/agent/tools/plugins/ReplyToolsPlugin.cpp` / `InfoToolsPlugin.cpp` / `ActionToolsPlugin.cpp`。`ToolPluginCatalog`
  显式组合并加载
  插件，`ToolRuntime` 不感知具体插件；自定义工具归属 `custom` 插件。重载某个插件只影响该插件的工具，工具名在
  全局唯一，冲突时拒绝注册，避免自定义工具覆盖内置工具后出现定义与执行不一致。
- 工具定义按“类别 → `promptOrder` → 工具名”稳定排序，避免重启或自定义工具刷新后改变 provider 的 prompt cache
  和模型偏好。私聊请求会在注入前排除 `GROUP_ONLY` 工具（当前为 `at_user`、`ban_user`、`send_poke`）。
- 自定义工具（Python 脚本 / HTTP 接口）存储于数据库，启动及后台刷新时加载；Python 工具通过 `sys.argv[1]` 传入参数 JSON 文件路径
- 拍一拍、撤回、引用回复、表情包收发、定时任务均由上述工具实现，由 Executor 根据上下文自动决策调用；天气、搜索、随机数、时间等能力来自
  `agentTools/` 目录的可导入自定义工具

工具执行的几个约束：

- `reply` / `reply_with_quote` / `no_reply` 是回复工具，调用后结束本轮处理；它们在 `ExecutorAgent::processToolCalls`
  中被拦截，不走普通工具返回值路径。
- `send_sticker` / `send_poke` / `reply_and_continue` 是中途动作，执行后本轮不结束，最终仍需用回复工具收尾；一次对话需要连续多条消息时，用
  `reply_and_continue` 发送前置消息，再用 `reply` 或 `no_reply` 收尾。
- `send_sticker` 与 `reply_and_continue` 通过 `MessageService` 直接发送，成功后自动写入聊天记录并推送
  WebSocket；主流程只负责发送最终文字回复。
- `deep_think` 是信息工具，不是全局思考模式。Executor 只在复杂问题需要额外推理时调用，工具结果再回到 Executor 组织成聊天回复。

### 会话派生状态维护

- **短期记忆**：`MemoryManager` 按统一会话 ID 从 SQLite 读取当前短期记忆，供 Executor 构建提示词。
- **长期记忆**：`LongTermMemory` 封装 `long_term_memory` 表的读取与向量检索；embedding 以 float 数组存入 BLOB。
- **维护批次**：`MessageList` 到达 `memorySummaryTriggerCount` 后，选取最旧的 `memorySummaryBatchSize` 条消息作为待总结内容，并复制随后最多 `memorySummaryContextCount` 条只读上下文。待总结消息在任务成功前继续保留在列表中。
- **任务持久化与恢复**：`ConversationMaintenanceService` 通过 `ConversationMaintenanceStore` 在同一 SQLite 事务中创建
  `memory_maintenance_jobs` 与 `affinity_maintenance_jobs`，随后分别异步调度消费者。两类任务各自按会话串行、退避重试，并在启动时由 `resumePending()` 恢复；它们互不依赖。
- **记忆维护**：`MemoryMaintenanceService` 提取并归类短期/长期记忆；它会按 `longTermRecallThreshold` 召回相似长期记忆，用于合并、去重和替换。embedding 完成后，在同一事务中更新短期记忆、写入长期记忆、删除被取代条目并确认任务完成。只有该任务成功并确认后，`MessageList` 才删除对应的最旧前缀。
- **好感度维护**：`AffinityMaintenanceService` 消费同一待总结批次，但独立评估、重试和恢复；任务完成时以事务应用分数并删除任务，避免重试重复叠加。
- 记忆提取与合并复用 executor 模型（`LlmClient::requestLLM`）；向量化使用独立 embedding 配置（`LlmClient::requestEmbedding`）。`recall_memory` 工具按余弦相似度（阈值 0.3）检索长期记忆，Router 另有 `routerWindowTriggerCount` / `routerWindowKeepCount` 子窗口参数。
- **被动召回**：消息预处理阶段从文本段和成功图片描述构建查询，命中结果直接写入当前完整消息的 `memories` 字段；该字段随快照进入 Executor
  上下文，不依赖独立缓存或聊天记录读取时的二次注入。

### 配置系统

`Config` 单例从 SQLite 加载 LLM API 配置（router / executor / executorThinking / image / embedding，每组独立配置 model /
endpoint 等参数，可选 `reasoningEffort`）、QQ Bot 配置、记忆参数。管理后台另有 `memory`
LLM 配置项存储于数据库，但当前记忆提取复用 executor 模型。提示词由 `PromptService` 管理（`executor_system` /
`router_system`），支持 `{botName}` 占位符，修改后写回数据库。

**用量统计**：每次 LLM 调用通过 `LlmClient::logUsage` 记录模型与 token 用量，后台"用量统计"页读取 `/admin/api/usage` 展示。

### 数据库

SQLite 文件位于 `data/insoulforge.db`（`Database` 单例，以读写锁保护）。存储包括完整消息恢复副本、短期/长期记忆、记忆与好感度维护任务、LLM
配置、提示词、会话配置、管理员、表情与自定义工具。

调试时可用任意 SQLite 客户端查看：

```bash
sqlite3 data/insoulforge.db ".tables"
```

## 开发指南

### 添加 C++ 内置工具

小型工具可直接按类别编辑 `src/agent/tools/plugins/ReplyToolsPlugin.cpp` / `InfoToolsPlugin.cpp` /
`ActionToolsPlugin.cpp` 注册（共享的参数 Schema 辅助函数在 `include/agent/tools/ToolArgument.hpp`）。内置文件由对应的
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

新增独立能力域时，实现 `ToolPlugin` 并在 `src/agent/tools/ToolPluginCatalog.cpp` 的实例列表中增加该类；不需要修改
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

### 添加自定义工具（Python）

1. 管理后台 → 自定义工具 → 添加
2. 填写名称、描述、参数定义（JSON Schema）、Python 脚本
3. 脚本从 `sys.argv[1]` 指定的 JSON 文件读取参数，结果打印到 stdout

也可直接编写 JSON 配置文件放入 `agentTools/` 后从后台导入，格式：

```json
{
  "name": "tool_name",
  "description": "工具描述",
  "parameters": {
    "type": "object",
    "properties": {
      "param1": {
        "type": "string",
        "description": "参数说明"
      }
    },
    "required": [
      "param1"
    ]
  },
  "scriptContent": "import json\nimport sys\nwith open(sys.argv[1]) as f:\n    args = json.load(f)\nprint(args['param1'])",
  "readme": "# 工具说明\n作者、用法、联系方式等"
}
```

### 添加管理 API

1. 在 `include/admin/http/AdminController.hpp` 中声明路由与 handler
2. 在 `src/admin/http/AdminController.cpp` 中实现（协程写法 `Task<HttpResponsePtr>` + `co_return`）
3. 数据访问统一通过 `Database` 单例

现有路由以 `/admin/api/` 为前缀（聊天记录、LLM 配置、提示词、表情、群、管理员、自定义工具、记忆、QQ 配置、用量统计），新路由建议沿用该前缀。

### 添加前端页面

1. 在 `frontend/src/components/` 新建 Vue 组件
2. 在 `frontend/src/App.vue` 注册导航
3. API 请求路径以 `/admin/api` 开头（dev 模式自动代理到后端）

### 修改数据库表结构

数据库初始化由 `Database::initialize()` 调用 `SchemaMigrator` 完成。修改表结构时，除了更新建表语句，还必须在
`SchemaMigrator` 中加入带版本号的增量迁移；迁移应能安全处理已存在的表、列和数据，不能只依赖新数据库的建表结果。

## 调试

- **日志**：`spdlog` 输出到控制台与 `logs/bot.log`，模块间通过 `infrastructure/logging/Logger.hpp` 封装。排查消息流水线问题时先看日志中
  Router 决策与 Executor 输出。管理后台读取 `LogBuffer` 的内存缓冲，启动时按 `bot.log`、`bot.1.log`、`bot.2.log` 从新到旧补足最近
  5000 条记录，避免全量解析滚动日志拖慢启动
- **协程**：所有异步 I/O 使用 `drogon::Task<T>` / `co_await`，注意 `co_await` 后对象生命周期（捕获 `shared_ptr` 而非裸指针）
- **会话并发**：`OneBotEventWorkflow` 为每个会话分别维护消息预处理队列、回复队列和 `MessageList`。不要在外部直接并发修改 `MessageList`，也不要跨 `co_await` 持有其内部或工作流内部锁。
- **前端**：`npm run dev` + 浏览器 DevTools；后端日志会打印收到的 OneBot 原始 JSON

## 代码规范

完整规范见 [CODING_STYLE.md](./CODING_STYLE.md)，要点：

- 头文件 `.hpp`、源文件 `.cpp`、类 PascalCase、方法 camelCase、成员 `m_` 前缀、静态 `s_` 前缀
- 包含顺序：标准库 → 第三方 → 项目头文件
- 格式化使用 `.clang-format`（Google 风格，4 空格缩进，120 列），提交前建议运行 clang-format 与 clang-tidy
- 单例统一 `static ClassName& instance()` + 私有构造
