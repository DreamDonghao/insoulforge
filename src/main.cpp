/// @file main.cpp
/// @brief 程序入口 - insoulforge 主程序
/// @details 依次初始化日志、访问令牌、配置文件、数据库和 Agent，随后启动 OneBot 连接、
///          定时任务调度器与管理后台。控制台输入 exit 可正常关闭服务并保存消息列表。

#include <cstdlib>

#include <cstdio>
#include <poll.h>
#include <unistd.h>

#include <admin/access/AdminAccessToken.hpp>
#include <admin/access/AdminStore.hpp>
#include <agent/ability/AsyncTaskManager.hpp>
#include <agent/ability/TaskScheduler.hpp>
#include <agent/runtime/AgentSystem.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <onebot/transport/OneBotWebSocketClient.hpp>

namespace {
    using namespace insoulforge;

    /// @brief 处理控制台的退出和日志等级命令；输入关闭或收到停止请求时结束。
    void processConsoleCommands(const std::stop_token &stopToken) {
        std::string command;
        while (!stopToken.stop_requested()) {
            pollfd input{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
            const i32 result = poll(&input, 1, 200);
            if (result == 0) {
                continue;
            }
            if (result < 0 || (input.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                return;
            }
            if (!(input.revents & POLLIN) || !(std::cin >> command)) {
                return;
            }
            if (command == "exit") {
                drogon::app().quit();
                return;
            }
            if (command == "log-level") {
                std::string level;
                if (!(std::cin >> level)) {
                    return;
                }
                if (Logger::setLevel(level)) {
                    Logger::info(0, "Logger", fmt::format("日志等级已切换为 {}", level));
                } else {
                    Logger::warn(0, "Logger", fmt::format("无效的日志等级: {}", level));
                }
                continue;
            }
            Logger::warn(0, "Main", fmt::format("未知命令: {}", command));
        }
    }
} // namespace

auto main() -> int {
    using namespace insoulforge;
    try {
        // 按依赖顺序初始化，任何一步失败都不启动后续服务。
        if (auto result = Logger::initialize(); !result) {
            // 日志系统尚不可用，启动错误直接写入标准错误输出。
            std::fprintf(stderr, "%s\n", result.error().c_str());
            return EXIT_FAILURE;
        }
        if (auto result = AdminAccessToken::initialize(); !result) {
            Logger::error(0, "Admin", result.error());
            return EXIT_FAILURE;
        }
        auto &config = Config::instance();
        if (auto result = config.initialize("data/config.json"); !result) {
            Logger::error(0, "Config", result.error().message);
            return EXIT_FAILURE;
        }
        auto &database = Database::instance();
        if (auto result = database.initialize("data/insoulforge.db"); !result) {
            Logger::error(0, "Storage", result.error());
            return EXIT_FAILURE;
        }
        auto &agentSystem = AgentSystem::instance();
        // 注册工具并加载提示词；工具说明需要已经加载的机器人名称。
        if (auto result = agentSystem.initialize(database, config.botName); !result) {
            Logger::error(0, "Agent", result.error());
            return EXIT_FAILURE;
        }
        auto &workflow = OneBotEventWorkflow::instance();
        // 恢复会话消息和遗留维护任务，再开始接收新的 OneBot 事件。
        if (auto result = workflow.initialize(database, config, agentSystem); !result) {
            Logger::error(0, "Workflow", result.error());
            return EXIT_FAILURE;
        }

        // 将自己的 QQ 号映射为带“(我)”标记的名称，便于区分机器人与同名群友。
        QQNameDirectory::setCustomName(config.selfQQNumber, config.botName + "(我)");

        // 删除超过十天未使用的图片描述缓存，并记录删除数量。
        const auto expiredImageCount = u64{ImageDescriptionStore::purgeExpired()};
        Logger::info(0, "Media", fmt::format("过期图片描述缓存清理完成 | deleted={}", expiredImageCount));

        // 配置为 WebSocket 时建立正向连接；使用 HTTP 时不建立连接。
        OneBotWebSocketClient::instance().start();

        // 启动定时任务调度器
        TaskScheduler::instance().start();

        // 使用短时 poll，以便收到退出请求后命令线程能够及时结束。
        std::jthread commandThread(processConsoleCommands);

        // 在请求进入路由前统一检查管理 API 和 WebSocket 的登录状态。
        drogon::app().registerPreRoutingAdvice(AdminAccessToken::checkRequestAccess);

        // 监听所有 IPv4 网卡，允许通过 Docker 映射端口或局域网访问后台。
        drogon::app().addListener("0.0.0.0", 7778);
        // 角色形象图允许 8 MiB；为 multipart 边界和表单头预留空间。
        drogon::app().setClientMaxBodySize(10U * 1024U * 1024U);
        drogon::app().setClientMaxMemoryBodySize(10U * 1024U * 1024U);
        // 前端构建产物位于运行目录的 public/，由同一 HTTP 服务提供。
        drogon::app().setDocumentRoot("public");
        Logger::info(0, "Main", "HTTP 服务启动 | port=7778");

        Logger::info(0, "Admin", std::format("管理后台访问令牌（重启后失效）: {}", AdminAccessToken::token()));
        Logger::info(0, "Admin", "也可尝试一下链接访问");
        // 枚举当前进程可见的 IPv4 地址；容器内不能据此获取宿主机的局域网地址。
        for (const auto &url: AdminAccessToken::loginUrls(7778)) {
            Logger::info(0, "Admin", " " + url + " ");
        }

        // 进入事件循环并等待退出；返回后才执行下面的收尾操作。
        drogon::app().run();

        // stdin 读取不能依赖 SIGINT 自动返回；先结束命令线程，避免 jthread 析构时阻塞退出。
        commandThread.request_stop();
        commandThread.join();

        // 先停止任务来源并记录未完成的后台任务，再持久化消息列表、关闭数据库。
        TaskScheduler::instance().stop();
        OneBotWebSocketClient::instance().stop();
        AsyncTaskManager::instance().stop();
        workflow.flushMessageListsToStorage();
        database.close();
        Logger::info(0, "Main", "系统正常退出");
    } catch (const std::exception &e) {
        Logger::critical(0, "Main", fmt::format("程序崩溃: {}", e.what()));
        Logger::shutdown();
        return EXIT_FAILURE;
    } catch (...) {
        Logger::critical(0, "Main", "程序崩溃: 未知错误");
        Logger::shutdown();
        return EXIT_FAILURE;
    }
    Logger::shutdown();
    return EXIT_SUCCESS;
}
