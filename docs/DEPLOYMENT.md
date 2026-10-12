# InSoulForge + NapCat QQ 机器人部署与更新示例

本教程使用 Docker 部署，不需要安装 C++ 编译器、Node.js，也不需要下载项目源码。InSoulForge 负责对话和管理后台，NapCat 负责登录 QQ 并收发消息，两者通过正向 WebSocket 连接。

>  本教程以全新安装的 Ubuntu 系统为例，请根据自己机器的实际环境调整步骤。

## 验证环境与准备工作

本教程在 Ubuntu 26.04.1 LTS、ARM64 虚拟机上逐步验证。测试机器的局域网 IP 为 `192.168.42.129`。其他系统和架构不能视为已经验证。

开始前准备：

- 一台安装好 Ubuntu、可以联网的机器，以及具有 `sudo` 权限的账户。
- 一个用于机器人的 QQ 账号，登录时需要手机 QQ 扫码并确认。
- 一个支持 OpenAI 兼容聊天接口和工具调用的模型服务，以及它的 API Key、接口地址和模型名称。
- 一台能访问服务器的电脑，用浏览器完成配置。

以下命令均在 Ubuntu 机器的终端执行，浏览器操作则在你的电脑上进行。每段命令成功后再执行下一段；不要在报错后继续往下操作。

## 1. 检查系统 

```bash
cat /etc/os-release
uname -m
df -h /
```

测试机器显示 Ubuntu 26.04.1 LTS，架构为 `aarch64`，根目录剩余约 10 GB。`aarch64` 对应 Docker 的 `arm64`。镜像下载和解压会占用磁盘，不要在空间已经不足时继续安装。

查看服务器的局域网地址：

```bash
ip -4 address
```

找到联网网卡上的 `inet` 地址，不要使用 `127.0.0.1` 或 Docker 网桥的地址。本文后面的 `192.168.42.129` 都需要换成你的服务器地址。

## 2. 安装 Docker 

> 已有docker环境可跳到第3节

本教程使用 Ubuntu 软件仓库中的 Docker，不运行第三方一键安装脚本，也不需要 Docker Compose。

```bash
sudo apt update
```

```bash
sudo apt install -y docker.io curl ca-certificates openssl
```

```bash
sudo systemctl enable --now docker
```

```bash
sudo docker version
```

最后一条命令应同时显示 `Client` 和 `Server`。测试机器安装的是 Docker 29.1.3。只有客户端信息或提示无法连接 Docker daemon 时，先解决服务启动问题。

本教程的 Docker 命令始终带 `sudo`，不需要把账户加入 `docker` 用户组。

### 软件源连接超时的处理

如果 `apt` 可以正常下载，跳过这一节。测试机器原来的 `cn.archive.ubuntu.com` 下载连接超时，改用清华 HTTPS 镜像后安装成功。

先查看实际配置，不要直接覆盖已有文件：

```bash
cat /etc/apt/sources.list.d/ubuntu.sources
```

下面的替换命令仅适用于本文测试机器的两个原始地址。它保留发行版名称、组件和签名验证配置；如果你的文件使用其他地址或格式，不要照搬。

```bash
sudo cp -a /etc/apt/sources.list.d/ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources.before-insoulforge
```

```bash
sudo sed -i 's|http://cn.archive.ubuntu.com/ubuntu/|https://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g; s|http://security.ubuntu.com/ubuntu/|https://mirrors.tuna.tsinghua.edu.cn/ubuntu/|g' /etc/apt/sources.list.d/ubuntu.sources
```

然后重新执行本节的 `sudo apt update` 和安装命令。备份文件不会作为软件源加载；`apt` 提示忽略它是正常的。

## 3. 下载镜像

**先尝试官方镜像。只有官方镜像拉取失败时，才使用下面的国内镜像站。** 每个镜像下载成功后即可继续，不需要把所有来源都下载一遍。

### 官方发布仓库

依次执行：

```bash
sudo docker pull dreamdonghao/insoulforge:latest
```

```bash
sudo docker pull mlikiowa/napcat-docker:latest
```

两条命令都成功时，直接进入第 4 节，跳过国内镜像站的命令。

### 官方不可用时：使用国内镜像站

如果上面的命令出现连接超时、无法连接仓库等网络错误，对下载失败的镜像执行对应的替代命令：

```bash
sudo docker pull docker.1ms.run/dreamdonghao/insoulforge:latest
```

```bash
sudo docker pull docker.1ms.run/mlikiowa/napcat-docker:latest
```

等待下载完成后再继续。镜像站是第三方缓存，不是项目自己的发布仓库，其可用性和同步速度可能变化。

### 启动与更新时使用同一来源

**后续启动和更新命令默认使用官方镜像。** 如果你通过镜像站下载，必须将对应命令中的镜像地址替换为右侧地址，其他参数不变：

| 镜像        | 默认官方地址                      | 官方不可用时替换为                               |
|-------------|-----------------------------------|--------------------------------------------------|
| InSoulForge | `dreamdonghao/insoulforge:latest` | `docker.1ms.run/dreamdonghao/insoulforge:latest` |
| NapCat      | `mlikiowa/napcat-docker:latest`   | `docker.1ms.run/mlikiowa/napcat-docker:latest`   |

不要只修改拉取命令而保留启动命令中的旧地址，否则 Docker 会查找另一个镜像名称，并可能重新从旧来源下载。切换镜像来源不需要更换数据目录或容器网络。

InSoulForge 另有官方 GHCR 地址 `ghcr.io/dreamdonghao/insoulforge:latest`。能访问 GHCR 的用户也可以使用它，拉取、启动和更新时保持地址一致即可。

`latest` 是可变标签，重新执行 `pull` 才会检查更新；已经运行的容器不会自动更新。

## 4. 创建数据目录与容器网络

```bash
sudo mkdir -p /opt/insoulforge/data /opt/napcat/qq /opt/napcat/config /opt/napcat/plugins
```

这些是宿主机上的固定绝对路径，与执行命令所在目录无关：

| 宿主机目录              | 容器内目录            | 用途                                           |
|-------------------------|-----------------------|------------------------------------------------|
| `/opt/insoulforge/data` | `/app/data`           | InSoulForge 配置、数据库及保存在该目录下的资源 |
| `/opt/napcat/qq`        | `/app/.config/QQ`     | QQ 登录数据                                    |
| `/opt/napcat/config`    | `/app/napcat/config`  | NapCat 配置                                    |
| `/opt/napcat/plugins`   | `/app/napcat/plugins` | NapCat 插件                                    |

不要把整个 `/app` 挂载到宿主机目录，否则会遮住镜像自带的程序和前端文件。InSoulForge 的 `/app/logs` 和 `/app/uploads` 不在上述持久化目录中；本教程不保留它们作为长期数据。

创建供两个容器通信的网络：

```bash
sudo docker network create bot-net
```

这个网络只需创建一次。如果以后再次执行提示网络已经存在，不需要删除重建。

## 5. 启动 InSoulForge

以下端口绑定到测试机器的局域网 IP，执行前替换为你的服务器 IP：

```bash
sudo docker run -d \
  --name insoulforge \
  --restart unless-stopped \
  --network bot-net \
  -p 192.168.42.129:7778:7778 \
  -v /opt/insoulforge/data:/app/data \
  --log-opt max-size=10m \
  --log-opt max-file=3 \
  dreamdonghao/insoulforge:latest
```

如果第 3 节使用了国内镜像站，将命令最后一行替换为 `docker.1ms.run/dreamdonghao/insoulforge:latest`。

`--restart unless-stopped` 让容器在 Docker 服务启动后自动运行，但手动停止后不会自动重新启动。日志参数限制 Docker 收集的标准输出日志大小，不控制程序内部日志文件的大小。

查看启动日志：

```bash
sudo docker logs --tail 40 insoulforge
```

首次启动会创建：

- `/opt/insoulforge/data/config.json`：全局配置。
- `/opt/insoulforge/data/insoulforge.db`：数据库。

日志应包含 `HTTP 服务启动 | port=7778` 和管理后台访问令牌。此时 NapCat 尚未配置，出现 OneBot WebSocket 连接失败和重连日志是预期现象。

`7778` 是 InSoulForge 的 HTTP 服务端口，提供后台页面和管理接口；HTTP 模式的 OneBot 上报也使用它。本教程使用正向 WebSocket，所以不是通过 `7778` 接收 QQ 消息。

## 6. 登录 InSoulForge 管理后台

在电脑浏览器中打开：

```text
http://192.168.42.129:7778/index.html
```

把启动日志中的管理后台访问令牌填入登录页面，点击「进入后台」。本文的部署验证使用手动输入令牌的方式。

程序也会输出 `index.html#token=...` 自动登录链接，但测试时 Safari 打开这类链接出现了 404，普通 `index.html` 页面和手动登录正常。遇到相同情况时使用上面的地址并手动输入令牌，不要把自动链接作为唯一入口。

令牌每次程序启动都会重新生成。重启容器后旧令牌失效，需要查看新的启动日志；它不是 OneBot 的 Access Token，也不是模型 API Key。

本教程仅在可信局域网内通过 HTTP 管理。不要直接把这些管理端口暴露到公网；公网管理应使用 SSH 转发或另行配置 HTTPS 和访问限制。

## 7. 启动 NapCat 并登录 QQ

生成 NapCat 管理页面的令牌：

```bash
openssl rand -hex 24
```

保存输出的字符串，替换下面的 `YOUR_NAPCAT_WEBUI_TOKEN`。不要直接使用这个占位文本，也不要把自己的真实令牌发给其他人。

```bash
sudo docker run -d \
  --name napcat \
  --restart unless-stopped \
  --network bot-net \
  -p 192.168.42.129:6099:6099 \
  -e MODE=ws \
  -e WEBUI_TOKEN='YOUR_NAPCAT_WEBUI_TOKEN' \
  -v /opt/napcat/qq:/app/.config/QQ \
  -v /opt/napcat/config:/app/napcat/config \
  -v /opt/napcat/plugins:/app/napcat/plugins \
  --log-opt max-size=10m \
  --log-opt max-file=3 \
  mlikiowa/napcat-docker:latest
```

如果第 3 节使用了国内镜像站，将命令最后一行替换为 `docker.1ms.run/mlikiowa/napcat-docker:latest`。

查看日志：

```bash
sudo docker logs --tail 60 napcat
```

测试镜像启动后显示 NapCat 4.18.37，随后输出 WebUI 地址、令牌和 QQ 登录二维码。日志中还出现了 DBus、EGL/GPU 初始化错误，但 WebUI 和扫码页面正常启动；不要仅凭这些字样判断整个容器启动失败。

在电脑浏览器中打开：

```text
http://192.168.42.129:6099/webui
```

1. 输入刚才生成的 WebUI 令牌，点击「登录」。
2. 在 QQ 登录页选择「扫码登录」。
3. 用机器人账号的手机 QQ 扫码，并在手机上确认登录。
4. 如果二维码已过期，点击「刷新二维码」后重新扫码。

`6099` 是 NapCat 管理页面端口，不是 InSoulForge 管理页面，也不是 OneBot 消息接口。两个管理页面使用各自的令牌。

启动命令没有向宿主机发布 `3001`。InSoulForge 与 NapCat 都在 `bot-net` 网络内，后续使用 `ws://napcat:3001` 通信，不需要额外开放宿主机端口。

`MODE=ws` 和 `WEBUI_TOKEN` 用于初始化缺失的配置。已有配置不会因重新设置这些环境变量而自动覆盖；后续更改应在 NapCat 页面完成。

## 8. 配置 OneBot 连接

### NapCat 端

在 NapCat 左侧选择「网络配置」。`MODE=ws` 已创建名为 `ws` 的配置，点击其「编辑」，核对：

| 字段         | 填写内容                              |
|--------------|---------------------------------------|
| 启用         | 开启                                  |
| 类型         | Websocket服务器，不是 Websocket客户端 |
| Host         | `0.0.0.0`                             |
| Port         | `3001`                                |
| 消息格式     | `Array`                               |
| 上报自身消息 | 关闭                                  |
| Token        | 自行生成的非空随机字符串              |

OneBot Token 可以再次使用 `openssl rand -hex 24` 生成。它与 NapCat WebUI 令牌是两个独立配置，应使用不同的值。保存后确认 `ws` 配置已启用。

不要把 Host 填成 `127.0.0.1`：那样只能从 NapCat 容器自身连接。`0.0.0.0` 表示监听容器内的所有 IPv4 接口；没有 `-p 3001:3001` 时，它不会因此发布到宿主机。

### InSoulForge 端

进入 InSoulForge「OneBot 配置」：

| 字段               | 填写内容                                      |
|--------------------|-----------------------------------------------|
| Bot QQ 号          | NapCat 实际登录的 QQ 号，不是你的管理员 QQ 号 |
| Bot 名称           | 你希望机器人使用的名称                        |
| 连接方式           | `WebSocket`                                   |
| WebSocket 服务地址 | `ws://napcat:3001`                            |
| Access Token       | 与 NapCat 的 OneBot Token 完全相同            |

点击「保存配置」。此地址中的 `napcat` 是容器名，由 `bot-net` 网络解析；不能改成 `127.0.0.1`，也不需要使用经常变化的容器 IP。

返回首页，检查 **OneBot WebSocket 已连接**。这是 InSoulForge 与 NapCat 的连接，不是浏览器与管理后台的连接。首次部署时出现重连日志不代表数据损坏，应先检查地址、Token 和 QQ 登录状态。

## 9. 填写模型配置与启用会话

本节说明首次使用所需配置。部署验证未调用模型 API，也未验证机器人回复、工具调用或记忆提取，不能据此判断某个模型服务是否兼容。

### 模型配置

在「LLM配置」中选择角色，填入服务商提供的 API Key、Base URL、Path 和 Model，每个角色分别保存。

「会话管理」中每个群都有独立的「发送群名片」开关，默认关闭。群内机器人管理员也可发送 `@机器人 /groupcard on` 或 `@机器人 /groupcard off`。
开启后，发送给模型的群消息在 `sender.card` 中包含群名片（Router/Jev 精简记录使用 `card`）；
QQ 昵称仍使用 `sender.name`。关闭不删除消息记录中的群名片，空群名片和私聊不添加该字段。

| 角色         | 用途                         | 首次配置建议                         |
|--------------|------------------------------|--------------------------------------|
| Router       | 判断普通群聊消息是否需要回复 | 配置兼容聊天接口的模型               |
| Executor     | 生成回复、调用工具           | 配置支持工具调用的聊天模型           |
| Executor 兜底 | 默认 Executor 请求失败时重试 | 可选，配置支持工具调用的聊天模型 |
| Memory       | 记忆提取、总结和好感度维护   | 单独填写，不会自动使用 Executor 配置 |
| Image        | 图片描述                     | 需要识图时配置多模态模型             |
| Embedding    | 记忆向量化与检索             | 需要向量检索时配置嵌入模型           |
| Executor思考 | 深度思考工具                 | 按需配置                             |
| 图片生成     | 生图工具                     | 按需配置对应的生图接口               |
| Jev          | 快速路由                     | 可选；不配置时使用 Router            |

对于 OpenAI 兼容聊天接口，通常将服务商提供的接口拆成 Base URL 和 Path。例如完整地址为 `https://api.example.com/v1/chat/completions` 时：

```text
Base URL: https://api.example.com/v1
Path: /chat/completions
Model: 服务商提供的真实模型名称
```

`api.example.com` 仅用于说明地址结构，不是可调用的服务。不要把聊天接口、嵌入接口和图片生成接口混用，也不要把 API Key 填到 OneBot 的 Access Token 中。

保存后的全局配置位于宿主机 `/opt/insoulforge/data/config.json`。优先使用后台修改；若直接编辑文件，应停止程序后编辑，再启动，避免文件内容与运行时配置不一致。

### 管理员与会话

1. 在「访问管理」中添加操作者的 QQ 号为管理员。
2. 使用管理员 QQ，在目标群里发送 `@机器人 /enable`，或发送 `@机器人 /enable 群号`。
3. 私聊机器人时可以直接发送 `/enable`，不需要 `@`。
4. 在「会话管理」中查看已登记的会话，并使用列表的启用/禁用按钮管理状态。

只登录 QQ 和连接 OneBot 并不等于启用了所有群聊。不要为了测试一次性启用所有群。群内管理命令需要真实的 QQ `@`，手动输入机器人的名字不等于提及。

上述操作不会让机器人自动加入 QQ 群；账号必须已在对应群中。模型配置是否可用，需要你在指定测试会话中另外验证。

## 10. 检查部署状态

查看容器是否仍在运行：

```bash
sudo docker ps --format 'table {{.Names}}\t{{.Status}}\t{{.Ports}}'
```

应看到 `napcat` 和 `insoulforge`，状态为 `Up`。只有容器 ID 输出，并不能证明程序一直正常运行。

检查两个容器是否在同一网络：

```bash
sudo docker network inspect bot-net --format '{{range .Containers}}{{.Name}}{{println}}{{end}}'
```

结果应包含两个容器名。

检查数据目录挂载是否正确：

```bash
sudo docker inspect insoulforge --format '{{range .Mounts}}{{.Type}} {{.Source}} -> {{.Destination}}{{println}}{{end}}'
```

其中必须包含：

```text
bind /opt/insoulforge/data -> /app/data
```

镜像还声明了 `/app/logs` 和 `/app/uploads`，因此会看到 Docker 自动创建的匿名卷。它们不是配置和数据库所在的数据目录，重建时不会自动复用；不要误把它们当成必须保留的主数据卷。

检查后台 HTTP 服务：

```bash
curl --max-time 10 -sS -o /dev/null -w '%{http_code}\n' http://192.168.42.129:7778/index.html
```

结果应为 `200`。它只表示页面入口可访问，仍需在浏览器中确认页面能显示并登录。

## 11. 备份数据

不要在程序仍然写数据库时直接复制单个数据库文件。先停止相应容器，再备份完整的挂载目录。

创建备份目录：

```bash
sudo mkdir -p /opt/insoulforge-backups
```

限制备份目录的访问权限：

```bash
sudo chmod 700 /opt/insoulforge-backups
```

备份 InSoulForge：

```bash
sudo docker stop -t 60 insoulforge
```

```bash
sudo tar -czf /opt/insoulforge-backups/insoulforge-20261011-1546.tar.gz -C /opt/insoulforge data
```

```bash
sudo tar -tzf /opt/insoulforge-backups/insoulforge-20261011-1546.tar.gz
```

最后一条应列出 `data/config.json`、`data/insoulforge.db` 和已有的其他数据。文件名中的日期和时间仅为本文实测示例，每次备份应换一个新文件名，不要覆盖以前的备份。

如果只是备份，不更新容器，完成后启动原容器：

```bash
sudo docker start insoulforge
```

NapCat 的 QQ 登录数据和配置也应独立备份：

```bash
sudo docker stop -t 60 napcat
```

```bash
sudo tar -czf /opt/insoulforge-backups/napcat-20261011-1554.tar.gz -C /opt napcat
```

然后启动 NapCat：

```bash
sudo docker start napcat
```

备份中包含 API Key、OneBot Token、QQ 登录数据和聊天数据，应限制访问，并在另一块磁盘或另一台可信机器上保留副本。同一磁盘上的备份不能防止整盘损坏。

## 12. 更新容器

### 更新 InSoulForge

先下载镜像。默认使用官方地址；官方拉取失败时，改用第 3 节的国内镜像站地址。下载失败时不要停止或删除正在运行的旧容器：

```bash
sudo docker pull dreamdonghao/insoulforge:latest
```

按上一节停止并备份 InSoulForge，确认备份可列出内容后，删除旧容器：

```bash
sudo docker rm insoulforge
```

重新执行第 5 节的完整启动命令。必须保留原来的宿主机数据路径、网络、端口绑定和其他自定义参数。换目录不会迁移数据，只会让程序读取另一份数据。

查看新的启动日志并重新登录。本文实际验证了停止、备份、删除和使用同一镜像重建：配置和数据库保留，新容器读取已有数据，后台登录令牌重新生成。这不是两个不同版本之间的兼容性测试。

### 更新 NapCat

先尝试官方镜像；拉取失败时，改用 `docker.1ms.run/mlikiowa/napcat-docker:latest`：

```bash
sudo docker pull mlikiowa/napcat-docker:latest
```

按上一节停止并备份 NapCat，然后删除旧容器：

```bash
sudo docker rm napcat
```

重新执行第 7 节的启动命令，保留三个数据目录及原 WebUI 令牌。已有 QQ 登录数据时，可以在启动命令中添加：

```text
-e ACCOUNT=你的机器人QQ号
```

本文重建时实际添加了 `ACCOUNT`，但 QQ 返回「登录态已失效，请重新登录」，因此仍需要在 WebUI 再扫码。保留目录能保留数据，不保证 QQ 永远接受旧登录态。

重新登录后确认 OneBot WebSocket 恢复连接。不要通过删除 QQ 数据目录来处理普通的登录态失效。

本文重建后已重新扫码，并确认带 Token 的 OneBot WebSocket 连接成功；InSoulForge 再次停止、备份和启动后，也从持久化配置恢复了连接。

## 13. 常见问题

| 现象                   | 排查方式                                                                                                      |
|------------------------|---------------------------------------------------------------------------------------------------------------|
| `apt` 下载连接超时     | 检查实际软件源和网络；本文的软件源替换步骤仅适用于注明的原始地址                                              |
| 拉取镜像超时           | 先确认网络和镜像站可用，不是重新安装 InSoulForge；不要在拉取失败后删除旧容器                                  |
| 容器名称已被占用       | 检查是否已有部署；更新时先停止、备份，再删除旧容器，不要随意换一个名字重复运行                                |
| 浏览器无法访问后台     | 检查服务器 IP、容器状态和 `-p` 的绑定地址；先用第 10 节的 `curl` 检查                                         |
| 后台登录失效           | 容器重启会更换令牌，从当前启动日志获取新令牌                                                                  |
| 自动登录链接返回 404   | 使用普通 `index.html` 地址并手动输入令牌；这是本次 Safari 测试实际遇到的情况                                  |
| OneBot 一直重连        | 检查 QQ 是否在线、`ws` 配置是否启用、Host 是否为 `0.0.0.0`、两端 Token 是否一致，以及两容器是否都在 `bot-net` |
| 更新后像全新安装       | 检查挂载源路径是否变化；不要立即填写一份新配置，先找回原数据目录                                              |
| NapCat 快速登录失败    | 到 WebUI 重新扫码，不要直接删除 `/opt/napcat/qq`                                                              |
| 能连接 OneBot 但不回复 | 另行检查模型 API、会话启用状态、管理员权限和日志；本教程的部署验证不覆盖这些业务行为                          |

## 验证范围

- 测试机器通过国内镜像站完整下载并部署了两个镜像。
- 通过代理访问两个 Docker Hub 官方镜像清单成功，摘要与镜像站一致；未通过 Docker 服务完成官方仓库的完整拉取。
- 已验证后台登录、QQ 扫码、带 Token 的 OneBot 连接、备份和同版本容器重建；未测试模型调用和跨版本升级。

## 官方与镜像站资料

- [Docker Engine 文档](https://docs.docker.com/engine/)
- [NapCat Docker 项目](https://github.com/NapNeko/NapCat-Docker)
- [毫秒镜像地址使用说明](https://1ms.run/guide)
