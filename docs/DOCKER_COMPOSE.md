# InSoulForge 与 NapCat 的 Docker Compose 部署

此配置使用正向 WebSocket：InSoulForge 主动连接 `ws://napcat:3001`。两个服务通过 Compose 默认网络互通，不需要手动创建网络，也不需要向宿主机映射 OneBot 的 3000、3001 端口。

NapCat 的镜像、初始化环境变量及数据目录参照 [NapCat-Docker 官方说明](https://github.com/NapNeko/NapCat-Docker)。

## 1. 准备文件

将 [compose.napcat.yaml](compose.napcat.yaml) 放到部署目录，例如 `/opt/insoulforge`，并命名为 `compose.yaml`，然后在该目录编辑。仓库根目录的 [compose.yaml](../compose.yaml) 仅部署 InSoulForge，不包含 NapCat：

```bash
nano compose.yaml
```

`napcat.environment.WEBUI_TOKEN` 已填入生成的后台登录令牌，可按需修改。镜像默认使用正式版 `latest`；如需预发布版本，将 `insoulforge.image` 改为对应标签，例如 `dreamdonghao/insoulforge:1.7.0-beta.8`。也可替换为 `ghcr.io/dreamdonghao/insoulforge` 的同名标签。所有部署参数直接写在 `compose.yaml` 中。

InSoulForge 镜像仅声明 `/app/data` 数据卷；不再为 `/app/logs`、`/app/uploads` 自动创建匿名卷。未额外挂载时，文件日志保存在容器内，重建容器后不保留。Compose 已配置 Docker 标准输出日志轮转。停止 InSoulForge 时最多等待 60 秒，供程序保存消息并关闭数据库。

数据目录使用 `/opt` 下的绝对路径，不受 `compose.yaml` 所在目录影响：

| 宿主机目录              | 内容                           |
|-------------------------|--------------------------------|
| `/opt/insoulforge/data` | InSoulForge 配置、数据库等数据 |
| `/opt/napcat/qq`        | QQ 登录状态                    |
| `/opt/napcat/config`    | NapCat 与 OneBot 配置          |
| `/opt/napcat/plugins`   | NapCat 插件                    |

已有部署应先核对原来的数据挂载路径，并修改 Compose 中对应的宿主机路径后再迁移。此配置固定使用 `napcat`、`insoulforge` 作为容器名，不能与同名旧容器同时创建。不要在迁移时改用空数据目录，也不要同时运行两份服务读取同一个数据库。

## 2. 启动与登录

```bash
docker compose config --quiet
docker compose up -d
docker compose logs --tail=100 napcat
docker compose logs --tail=100 insoulforge
```

- NapCat 管理后台：`http://服务器IP:6099/webui`，使用 `compose.yaml` 中的 `WEBUI_TOKEN` 登录，再扫描二维码登录 QQ。
- InSoulForge 管理后台：从启动日志复制自动登录链接，将链接中的主机地址替换为服务器 IP，保留端口、路径及 `#token=...` 部分。

默认映射 `6099` 和 `7778` 两个管理端口。如果通过 SSH 转发或本机反向代理访问，可将端口映射分别改为 `127.0.0.1:6099:6099` 和 `127.0.0.1:7778:7778`。

## 3. 连接 OneBot

在 NapCat 后台的网络配置中，确认启用一个 **WebSocket 服务端（正向 WebSocket）**：

| 设置     | 值                                      |
|----------|-----------------------------------------|
| 监听地址 | `0.0.0.0`                               |
| 端口     | `3001`                                  |
| Token    | 自行设置，并在 InSoulForge 中填写相同值 |

在 InSoulForge 的「OneBot 配置」中填写：

| 设置 | 值 |
| --- | --- |
| 传输方式 | 正向 WebSocket |
| WebSocket 地址 | `ws://napcat:3001` |
| Access Token | 与 NapCat WebSocket 服务端的 Token 相同 |
| 机器人 QQ 号 | 当前登录的 QQ 号 |

保存配置后检查连接状态，再配置模型与需要启用的会话。这里的 `napcat` 是服务名；不要填 `127.0.0.1`，也不需要配置 HTTP 上报或反向 WebSocket。

`WEBUI_TOKEN` 是管理后台令牌，与 OneBot Access Token 分开设置。`MODE=ws` 和 `WEBUI_TOKEN` 只初始化尚不存在的 NapCat 配置；已有配置通过 NapCat 后台修改。`depends_on` 只保证启动顺序，不表示 QQ 已登录或 OneBot 已就绪，InSoulForge 会自动重连。

## 4. 日常操作

```bash
# 查看状态与日志
docker compose ps
docker compose logs -f --tail=100

# 仅更新 InSoulForge
docker compose pull insoulforge
docker compose up -d insoulforge

# 更新全部服务
docker compose pull
docker compose up -d

# 停止服务，保留容器与数据
docker compose stop

# 再次启动
docker compose start
```

固定标签更新时先修改 `compose.yaml` 中的镜像标签，再执行拉取与启动。更新前备份数据目录；需要一致的数据库文件备份时，先停止 InSoulForge，再复制完整 `data` 目录，完成后启动。InSoulForge 重启后需从日志获取新的后台登录链接。
