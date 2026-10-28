# InSoulForge + NapCat Docker Compose 部署

本文使用 [compose.napcat.yaml](compose.napcat.yaml) 联合部署 InSoulForge 和 NapCat，不需要安装编译器、Node.js，也不使用 `.env` 文件。仓库根目录的 [compose.yaml](../compose.yaml) 仅部署 InSoulForge，适用于已经单独部署 NapCat 的情况，OneBot 地址需按实际网络配置。

InSoulForge 负责对话与管理后台，NapCat 负责登录 QQ、收发消息。两个服务通过 Compose 默认网络通信，InSoulForge 连接 `ws://napcat:3001`。

## 1. 准备环境

准备：

- 一台能够运行 Docker 的 Linux 服务器。
- Docker Engine 和 Docker Compose 插件。
- 用于登录 NapCat 的 QQ 账号。
- 模型服务的 API Key、接口地址和模型名称。

尚未安装 Docker 时，按照 [Docker 官方 Ubuntu 安装说明](https://docs.docker.com/engine/install/ubuntu/)安装 Docker Engine 和 Compose 插件。其他发行版使用对应的官方安装说明。

检查环境：

```bash
sudo docker version
sudo docker compose version
```

第一条应同时显示 Docker Client 和 Server，第二条应输出 Compose 版本。

下文命令在服务器执行。示例使用 `sudo`，已经使用 root 登录时可以省略。

## 2. 准备 Compose 和数据目录

创建目录：

```bash
sudo mkdir -p \
  /opt/insoulforge/data \
  /opt/napcat/qq \
  /opt/napcat/config \
  /opt/napcat/plugins
```

将仓库中的 `docs/compose.napcat.yaml` 上传或复制到以下位置，并命名为 `compose.yaml`：

```text
/opt/insoulforge/compose.yaml
```

进入部署目录：

```bash
cd /opt/insoulforge
```

后续 `docker compose` 命令都在这个目录执行。

### 数据目录

| 宿主机目录 | 容器内目录 | 用途 |
| --- | --- | --- |
| `/opt/insoulforge/data` | `/app/data` | InSoulForge 配置、数据库及相关数据 |
| `/opt/napcat/qq` | `/app/.config/QQ` | QQ 登录数据 |
| `/opt/napcat/config` | `/app/napcat/config` | NapCat 配置 |
| `/opt/napcat/plugins` | `/app/napcat/plugins` | NapCat 插件 |

这些是绝对路径，不受执行命令的位置影响。不要把整个 `/app` 挂载到宿主机，否则会遮住镜像自带的程序和前端文件。

已有部署迁移时，先核对原数据目录，再调整挂载路径。换成空目录不会迁移旧数据，而是创建一份新配置。

### 修改部署参数

编辑 Compose：

```bash
sudo nano compose.yaml
```

当前配置：

- InSoulForge 镜像：`dreamdonghao/insoulforge:latest`。
- NapCat 镜像：`mlikiowa/napcat-docker:latest`。
- 容器名：`insoulforge`、`napcat`。
- 时区：`Asia/Shanghai`。
- 两个服务均使用 `restart: unless-stopped`。
- InSoulForge 停止时最多等待 60 秒，供程序保存消息并关闭数据库。
- Docker 标准输出日志按每个文件 10 MiB、最多 3 个文件轮转。

`napcat.environment.WEBUI_TOKEN` 已填入生成的后台登录令牌，可以按需修改。它是 NapCat 管理后台的令牌，不是 OneBot Access Token。

使用预发布版时，将 `insoulforge.image` 改为对应的版本标签。也可以使用官方 GHCR 镜像：

```text
ghcr.io/dreamdonghao/insoulforge:latest
```

启动和更新都以 Compose 中的 `image` 为准。

### 端口

| 宿主机映射 | 用途 |
| --- | --- |
| `0.0.0.0:7778:7778` | InSoulForge 管理后台 |
| `0.0.0.0:6099:6099` | NapCat 管理后台 |

`0.0.0.0` 表示绑定宿主机所有 IPv4 接口。浏览器访问时使用服务器实际 IP，不使用 `0.0.0.0`。

如果只通过服务器本机反向代理或 SSH 转发访问，可以将绑定地址改为 `127.0.0.1`。

OneBot 的 `3001` 端口只供 Compose 网络内的服务通信，不需要映射到宿主机，也不需要手动创建网络。

## 3. 启动服务

先检查配置：

```bash
sudo docker compose config --quiet
```

无错误后拉取镜像并启动：

```bash
sudo docker compose pull
sudo docker compose up -d
```

查看状态：

```bash
sudo docker compose ps
```

查看启动日志：

```bash
sudo docker compose logs --tail=100 napcat
sudo docker compose logs --tail=100 insoulforge
```

Compose 中固定了容器名。如果提示名称已被占用，先确认是否已有旧部署，核对并备份数据后再迁移，不要重复启动两份服务读取同一个数据库。

`depends_on` 只保证启动顺序，不保证 QQ 已登录或 OneBot 已就绪。完成后续配置前，InSoulForge 出现连接失败和自动重连日志是正常现象。

## 4. 登录 NapCat

在浏览器中打开：

```text
http://服务器IP:6099/webui
```

使用 Compose 中的 `WEBUI_TOKEN` 登录，再用机器人账号的手机 QQ 扫码并确认。

如果二维码过期，在页面刷新二维码后重新扫码。

`MODE=ws` 和 `WEBUI_TOKEN` 只初始化尚不存在的配置。已有配置不会因为修改环境变量而自动覆盖，应通过 NapCat 后台修改。

## 5. 登录 InSoulForge

首次启动会在数据目录创建：

```text
/opt/insoulforge/data/config.json
/opt/insoulforge/data/insoulforge.db
```

从日志获取管理后台令牌：

```bash
sudo docker compose logs --tail=100 insoulforge
```

在浏览器中打开：

```text
http://服务器IP:7778/index.html
```

输入日志中的后台访问令牌。

也可以使用日志输出的自动登录链接，将其中的主机地址替换成服务器实际 IP，并保留端口、路径和 `#token=...` 部分。

InSoulForge 每次启动会重新生成后台令牌。重启后需要从新的启动日志获取令牌。

## 6. 配置 OneBot 连接

### NapCat 端

进入「网络配置」，确认启用一个正向 WebSocket 服务端：

| 设置 | 值 |
| --- | --- |
| 类型 | WebSocket 服务端 |
| 监听地址 | `0.0.0.0` |
| 端口 | `3001` |
| Token | 自行设置一个非空令牌 |

这里的 Token 与 NapCat WebUI 令牌分开设置。

### InSoulForge 端

进入「OneBot 配置」：

| 设置 | 值 |
| --- | --- |
| 机器人 QQ 号 | NapCat 实际登录的 QQ 号 |
| Bot 名称 | 机器人使用的名称 |
| 连接方式 | WebSocket |
| WebSocket 服务地址 | `ws://napcat:3001` |
| Access Token | 与 NapCat WebSocket 服务端的 Token 相同 |

保存后，在首页检查 OneBot 连接状态。

`napcat` 是 Compose 服务名。不要填写 `127.0.0.1`，因为它在 InSoulForge 容器中指向 InSoulForge 自身。

这里使用正向 WebSocket，不需要配置 HTTP 上报或反向 WebSocket。

## 7. 配置模型与会话

### 模型

在「模型配置」中为需要使用的角色填写 API Key、Base URL、Path 和 Model，分别保存。

| 角色 | 用途 |
| --- | --- |
| 回复决策 | 群聊回复判断 |
| Jev | 可选的快速回复判断 |
| 回复生成 | 生成回复并调用工具 |
| 备用模型 | 可选的回复备用模型 |
| 深度思考 | 深度思考工具 |
| 图片识别 | 图片描述与多模态提问 |
| 图片生成 | 生图工具 |
| 记忆维护 | 记忆提取与整理 |
| 向量化 | 长期记忆向量存储与召回 |

以 OpenAI 兼容聊天接口为例：

```text
完整接口：https://api.example.com/v1/chat/completions

Base URL：https://api.example.com/v1
Path：/chat/completions
Model：服务商提供的真实模型名称
```

示例域名不是真实服务。聊天、嵌入和生图接口应分别配置。

模型请求超时时间和重试次数可以在对应模型配置中调整。

### 会话

连接 OneBot 不等于启用了所有会话。

在「会话管理」中检查已登记的会话，并按需启用。群聊的自动回复、命令解析、群名片发送及跨会话共享等设置按各会话需要配置。

如果使用群聊管理命令，先在「访问管理」中添加管理员 QQ，并确认对应群聊开启命令解析。

私聊不解析管理命令，通过后台管理启用状态。「OneBot 配置」中的「新私聊默认启用」决定首次收到消息的新私聊是否自动启用，不会批量改变已有会话。

先选择一个会话验证模型回复、工具和记忆功能，再扩大使用范围。

全局配置位于 `/opt/insoulforge/data/config.json`。优先通过后台修改；如需直接编辑文件，先停止服务，编辑完成后再启动。

## 8. 日常操作

以下命令均在 `/opt/insoulforge` 执行。

查看状态：

```bash
sudo docker compose ps
```

持续查看日志，按 Ctrl+C 退出查看：

```bash
sudo docker compose logs -f --tail=100
```

只重启 InSoulForge：

```bash
sudo docker compose restart insoulforge
```

停止全部服务，保留容器和数据：

```bash
sudo docker compose stop
```

重新启动已有容器：

```bash
sudo docker compose start
```

修改 Compose 中的端口、环境变量等部署配置后，使用以下命令应用：

```bash
sudo docker compose config --quiet
sudo docker compose up -d
```

`restart` 不会应用 Compose 文件中的配置变更。

## 9. 备份

先停止相应服务，再备份完整数据目录，避免只复制仍在写入的数据库文件。

创建备份目录：

```bash
sudo mkdir -p /opt/insoulforge-backups
sudo chmod 700 /opt/insoulforge-backups
```

### InSoulForge

```bash
sudo docker compose stop insoulforge

backup_time=$(date +%Y%m%d-%H%M%S)
sudo tar -czf "/opt/insoulforge-backups/insoulforge-${backup_time}.tar.gz" \
  -C /opt/insoulforge data compose.yaml

sudo docker compose start insoulforge
```

### NapCat

```bash
sudo docker compose stop napcat

backup_time=$(date +%Y%m%d-%H%M%S)
sudo tar -czf "/opt/insoulforge-backups/napcat-${backup_time}.tar.gz" \
  -C /opt napcat

sudo docker compose start napcat
```

确认备份命令成功后再继续操作。备份包含配置、令牌、聊天数据或 QQ 登录数据，应妥善保存。

## 10. 更新

### 只更新 InSoulForge

先按上一节备份数据。固定版本标签需要先修改 Compose 中的 `insoulforge.image`，使用 `latest` 则不需要改标签。

```bash
sudo docker compose pull insoulforge
```

拉取成功后执行：

```bash
sudo docker compose up -d --no-deps insoulforge
sudo docker compose logs --tail=100 insoulforge
```

`--no-deps` 表示这次只更新 InSoulForge，不启动或重建 NapCat。

### 更新全部服务

先备份，再执行：

```bash
sudo docker compose pull
```

拉取成功后执行：

```bash
sudo docker compose up -d
sudo docker compose ps
sudo docker compose logs --tail=100
```

更新时不需要手动删除旧容器或重新创建网络。Compose 会根据镜像和配置重建需要更新的服务，固定挂载目录中的数据保留。

镜像拉取失败时，先解决下载问题，不要删除正在运行的容器。

InSoulForge 重启后需要获取新的后台令牌；NapCat 如果提示登录态失效，在 WebUI 重新扫码。

### 数据与日志

Compose 仅为 InSoulForge 绑定 `/app/data`，文件日志不作为长期数据保留。Docker 收集的标准输出日志已配置轮转。

仓库 Dockerfile 仅声明 `/app/data` 数据卷；这项变更需要发布新镜像后生效。旧镜像可能仍自动创建 `/app/logs`、`/app/uploads` 匿名卷，主配置和数据库仍位于绑定的 `data` 目录。

更新前备份数据。若新版修改了数据库结构，回退镜像时还需要考虑恢复与旧版本匹配的数据备份。

## 11. 常见问题

| 现象 | 检查方式 |
| --- | --- |
| `docker compose` 不存在 | 确认已安装 Docker Compose 插件 |
| 镜像拉取失败 | 检查服务器到镜像仓库的网络；InSoulForge 可切换到官方 GHCR 地址 |
| 容器名被占用 | 检查是否存在同名旧容器，核对原数据后再迁移 |
| 后台无法访问 | 检查服务器 IP、端口映射、容器状态和网络访问规则 |
| InSoulForge 登录失效 | 从当前启动日志获取新令牌 |
| OneBot 一直重连 | 检查 QQ 登录状态、WebSocket 是否启用、监听地址、端口和两端 Token |
| 修改 WebUI 环境变量没有生效 | 已有 NapCat 配置不会被初始化变量覆盖，在 NapCat 后台修改 |
| 更新后像全新安装 | 检查 `/opt/insoulforge/data:/app/data` 挂载是否变化 |
| 修改 Compose 后没有生效 | 使用 `docker compose up -d`，不能只执行 `restart` |
| 私聊 `/enable` 没有启用会话 | 私聊不解析管理命令，通过后台管理 |
| OneBot 已连接但不回复 | 检查会话启用状态、群聊自动回复设置、模型配置及运行日志 |

## 参考

- [InSoulForge 单服务配置](../compose.yaml)
- [InSoulForge + NapCat 联合部署配置](compose.napcat.yaml)
- [Compose 配置说明](DOCKER_COMPOSE.md)
- [Docker Engine 安装说明](https://docs.docker.com/engine/install/ubuntu/)
- [Docker Compose 更新部署说明](https://docs.docker.com/compose/how-tos/production/)
- [NapCat Docker 项目](https://github.com/NapNeko/NapCat-Docker)
