# InSoulForge

一个基于 Agent 架构的智能 QQ 群聊机器人后端。

QQ 交流群：1097487360

## ✨ 特性

- **Web 管理后台** - 可视化查看与配置
- **智能对话** - Router + Executor 两阶段决策，按会话生成稳定上下文快照并自主判断是否回复
- **好感度** - 可以根据对话自动调整对某人的好感度
- **图片识别** - 可单独配置视觉模型识别图片与 GIF 动图，媒体哈希缓存避免重复请求
- **自定义角色** - 可设置 Bot 的提示词来定制人设和性格
- **稳定上下文快照** - 每条消息在完成媒体识别与记忆召回后生成冻结快照，保证 Router 和 Agent 使用一致的上下文
- **智能记忆** - 记忆自动提炼，记住群友的喜好、习惯、重要事件
- **长期记忆召回** - 长期记忆本地向量化（SQLite 存储 + 余弦相似度检索），自动召回相关记忆注入回复上下文，也可主动查询，可选配
  Embedding 模型
- **适配 QQ 功能** - Bot 可自行收藏发送表情包、引用、@ 群友、撤回消息、拍一拍、禁言
- **多条回复与深度思考** - 一次对话可连续发送多条消息，耗时操作前可先发过程消息，复杂问题按需调用深度思考模型
- **自定义工具** - 支持 Python 脚本 / HTTP 接口，可视化配置，一键导入导出
- **定时任务** - 提供低耦合的定时任务功能，可定时发送消息以及与自定义工具组合使用

## 🚀 快速开始

### 前置要求

本机器人需要配合 **OneBot** 协议实现使用，推荐使用 **napcat**

选择一个 OneBot 实现，并启用 HTTP 或正向 WebSocket 服务之一。

消息接收端口：7778

### 方式一：Docker 部署（推荐）

镜像已发布至 Docker Hub 和 GitHub Container Registry，支持 **amd64 / arm64** 架构：

Docker Hub （推荐，可配置国内镜像加速）

```bash
docker pull dreamdonghao/insoulforge:latest
```

GitHub Container Registry （备用）

```bash
docker pull ghcr.io/dreamdonghao/insoulforge:latest
```

启动容器：

```bash
docker run -d --name insoulforge \
  -p 7778:7778 \
  -v ./data:/app/data \
  dreamdonghao/insoulforge:latest
```

> 全局配置文件 `data/config.json`、数据库、日志和表情包分别持久化在宿主机的 `./data`
> 目录（也可自定义目录），升级镜像时数据不会丢失。首次启动会自动创建包含默认值的 `data/config.json`。

> 如果 napcat 也运行在 Docker 中，注意容器内的 `127.0.0.1` 指向容器自身。启动后让两个容器加入同一个 docker
> 网络，然后用容器名互访（无需重建容器，connect 直接生效）：
>
> ```bash
> docker network create bot-net
> docker network connect bot-net napcat       # napcat 换成你的 napcat 容器名
> docker network connect bot-net insoulforge
> ```
>
> 使用 HTTP 时，在管理后台将 OneBot 的 HTTP 服务地址填为 `http://napcat:3000`（容器名 + napcat HTTP 端口），
> 并将 napcat 的上报地址填为 `http://insoulforge:7778/`。使用正向 WebSocket 时，将 WebSocket 地址填为
> `ws://napcat:3001`；此方式不需要配置 HTTP 上报地址。

服务启动日志会输出本机可用地址的自动登录链接，例如：

```text
管理后台自动登录链接（重启后失效）: http://127.0.0.1:7778/index.html#token=<token>
```

直接打开链接即可登录；令牌位于 URL 片段中，不会发送到服务端，验证后会自动从地址栏清除。令牌仅保存在运行中的进程内，不会写入配置文件或数据库；每次重启服务都会生成新的令牌并使已有登录会话失效。在
Docker 默认桥接网络中，容器无法枚举宿主机局域网地址；可将链接中的主机替换为宿主机 LAN IP 后访问。

### 首次配置

在管理后台完成以下配置：

1. **OneBot 配置** - 填写连接参数
    - Access Token
    - Bot QQ 号
    - HTTP 或 WebSocket 传输方式及其服务地址
    - Bot 名称

2. **LLM 配置** - 配置模型 API
    - 支持 Router、Executor、Executor 思考、Image 与 Embedding 分别配置
    - 兼容 OpenAI API 格式

3. **启用群聊** - 添加要启用的 QQ 群或用户

## 📖 使用指南

推荐使用web页面进行配置

### 群聊命令

在群中 @机器人 发送命令（私聊无需 @，直接发送即可）：

| 命令                | 说明                                   | 权限   |
|---------------------|----------------------------------------|--------|
| `/help`             | 显示帮助                               | 所有人 |
| `/status`           | 查看当前会话状态                       | 所有人 |
| `/admins`           | 查看管理员列表                         | 所有人 |
| `/about`            | 关于本项目                             | 所有人 |
| `/enable [会话ID]`  | 启用会话（群聊传群号，私聊可不带参数） | 管理员 |
| `/disable [会话ID]` | 禁用会话（私聊可不带参数）             | 管理员 |
| `/groups`           | 查看已启用的会话列表                   | 管理员 |
| `/addadmin <QQ号>`  | 添加管理员                             | 管理员 |
| `/deladmin <QQ号>`  | 移除管理员                             | 管理员 |
| `/blacklist`        | 查看全局 QQ 黑名单                     | 管理员 |
| `/addblacklist <QQ号>` | 将用户加入全局 QQ 黑名单            | 管理员 |
| `/delblacklist <QQ号>` | 将用户移出全局 QQ 黑名单            | 管理员 |
| `/listemoji`        | 查看QQ收藏表情列表                     | 管理员 |
| `/delemoji <名称>`  | 从QQ收藏表情中删除                     | 管理员 |
| `/clearimagecache`  | 清除图片和 GIF 描述缓存                | 管理员 |

命令支持中文别名，如 `/帮助`、`/状态`、`/启用`。

## ⚙️ 配置说明

### OneBot 配置

| 参数               | 说明                                   |
|--------------------|----------------------------------------|
| Access Token       | OneBot API 访问令牌                    |
| Bot QQ 号          | 机器人自身的 QQ 号                     |
| 传输方式           | HTTP 或正向 WebSocket，两者互斥        |
| HTTP 服务地址      | HTTP 模式下的 OneBot HTTP 服务地址     |
| WebSocket 服务地址 | WebSocket 模式下的 OneBot 正向连接地址 |
| Bot 名称           | 机器人在群聊中的名称                   |

### LLM 配置

各模型可分别配置：

| 模型         | 用途                                        | 建议配置              |
|--------------|---------------------------------------------|-----------------------|
| Router       | 快速路由决策                                | 轻量模型，低温度      |
| Executor     | 生成回复                                    | 主力模型，较高温度    |
| Executor思考 | `deep_think` 工具使用的深度思考模型（可选） | 推理模型，如 DeepSeek |
| Image        | 图片内容识别                                | 多模态模型            |
| Embedding    | 长期记忆向量化与检索（可选）                | 向量模型              |

全局运行配置保存在 `data/config.json`，包括 LLM、OneBot 与记忆参数。文件不存在时程序会写入默认内容；缺失或类型错误的字段会在启动时自动修复。配置文件已被
Git 忽略，Docker 部署时通过 `./data:/app/data` 挂载即可持久化。

> `config.json` 不保存管理后台访问令牌。请通过服务启动日志获取当前令牌，不要将其提交到仓库或发送给无关人员。

**深度思考**：`Executor思考` 不是全局思考模式开关，而是 `deep_think` 工具使用的模型配置。Executor
只有在遇到数学计算、多步推理、技术分析等复杂问题时才会按需调用，日常闲聊不会固定走推理模型。

### 记忆与上下文参数

- **近期上下文上限** - Router 与 Agent 实际可见的最近完整消息数，默认 100
- **总结触发条数** - 完整消息列表达到该数量时创建一批会话派生状态维护任务，默认 100
- **每批总结条数** - 本批真正参与记忆提取、并在记忆任务成功后删除的最旧消息数，默认 50，不能超过触发条数
- **总结补充上下文条数** - 紧随总结批次的只读消息数，仅帮助模型理解语境，不参与提取或删除，默认 10
- **记忆提取 maxTokens** - 记忆提取模型的输出 token 上限，默认 4000
- **Router 窗口触发/保留条数** - Router 使用的子窗口滑动参数，默认 20/10，用于平衡路由 prompt 长度与缓存命中
- **短期记忆上限** - 每次归类整理后保留的短期记忆条数上限，默认 15
- **长期记忆召回阈值** - 新记忆与已有长期记忆合并去重时的相似度阈值，默认 0.65
- **长期记忆注入阈值** - 消息预处理时被动召回并注入当前消息的相似度阈值，默认 0.45

记忆总结与好感度更新会作为独立的持久化任务异步执行。只有记忆任务成功后，对应的旧消息才会从完整消息列表移除；任务可在程序重启后恢复。

### 提示词定制

在管理后台可修改 Router 与 Executor 的系统提示词，支持 `{botName}` 占位符自动替换。

## 🔧 高级功能

### 自定义工具

支持通过 Python 脚本或 HTTP 接口扩展机器人能力：

1. 进入管理后台 → 自定义工具
2. 点击"添加工具"或"导入"
3. 填写工具名称、描述、参数定义和执行方式（Python 脚本 / HTTP 请求）
4. 保存并启用

工具支持：

- **参数定义** - JSON Schema 格式，LLM 自动理解
- **Python 脚本** - 通过 `sys.argv[1]` 接收参数 JSON 文件路径，可自定义 Python 解释器路径
- **HTTP 接口** - 配置请求地址、方法、参数模板
- **说明文档** - Markdown 格式，记录作者、用法、联系方式
- **导入导出** - JSON 文件格式，方便分享

#### 内置工具配置文件

项目提供了一些常用工具配置，位于 `agentTools/` 目录：

| 文件               | 功能       | 依赖                |
|--------------------|------------|---------------------|
| `random.json`      | 随机数生成 | 无                  |
| `get_time.json`    | 获取时间   | 无                  |
| `get_weather.json` | 天气查询   | 无                  |
| `search_web.json`  | 网络搜索   | `duckduckgo-search` |

导入方法：

1. 管理后台 → 自定义工具 → 导入
2. 上传 JSON 文件

### 配置 Embedding（可选）

1. 准备 OpenAI 兼容的 Embedding 服务（如硅基流动、OpenAI）
2. 管理后台 → LLM配置 → Embedding，填写 API 配置

配置后，短期记忆迁移到长期记忆时自动向量化入库；每条消息入库时后台召回相关长期记忆并注入回复上下文，也可通过
`recall_memory` 工具按余弦相似度主动查询。未配置时长期记忆自动停用，仅保留短期记忆。

### 图片与动图识别

图片识别会先下载媒体并按 SHA-256、视觉模型和提示词版本查询缓存。静态图片以 Base64 Data URL 提交视觉模型；GIF 会按播放时间抽取最多
16 帧并一次性提交，超过 16 帧时保留首尾帧并均匀取样。消息和 Agent 上下文只保留识别结果，不会包含图片 URL 或 Base64 内容；管理员可用
`/clearimagecache` 清除描述缓存。

---

## 🛠️ 开发者指南

本项目使用 **C++ **实现，Web页面使用**Vue3**

本地构建、架构和调试见[开发文档](./docs/DEVELOPMENT.md)；提交 Issue 或 Pull Request 前请阅读[参与贡献指南](./CONTRIBUTING.md)。

---

## 📄 许可证

本项目采用标准 [GNU Affero General Public License v3.0](https://www.gnu.org/licenses/agpl-3.0.html)（`AGPL-3.0-only`）开源。

详见 [LICENSE](LICENSE)。

---

Made with by [DreamDonghao](https://github.com/DreamDonghao)
