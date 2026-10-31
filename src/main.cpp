/// @file main.cpp
/// @brief 程序入口 - insoulforge 主程序
/// @details 依次初始化日志、访问令牌、配置文件、数据库和 Agent，随后启动 OneBot 连接、
///          定时任务调度器与管理后台。控制台输入 exit 可正常关闭服务并保存消息列表。

#include <cstdlib>
#include <exception>
#include <format>
#include <iostream>
#include <string>
#include <thread>

#include <cstdio>
#include <drogon/HttpAppFramework.h>
#include <fmt/color.h>
#include <fmt/format.h>
#include <poll.h>
#include <stop_token>
#include <unistd.h>

#include <admin/access/AdminAccessToken.hpp>
#include <admin/http/PersonNameResolver.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/monitoring/ProcessMetrics.hpp>
#include <infrastructure/storage/Database.hpp>
#include <onebot/transport/OneBotWebSocketClient.hpp>
#include <workflow/OneBotEventWorkflow.hpp>
#include <workflow/agent/ability/AsyncTaskManager.hpp>
#include <workflow/agent/memory/maintenance/PersonMemoryConsolidator.hpp>
#include <workflow/agent/runtime/AgentSystem.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/mcp/McpTools.hpp>
#include <workflow/scheduling/TaskScheduler.hpp>
#include <workflow/session/QQNameDirectory.hpp>

namespace {
    using namespace insoulforge;

    /// @brief 仅向服务终端输出当前登录凭据，不经过应用日志系统。
    void printLoginInstructions() {
        // 登录引导只直接输出到服务终端，不进入文件日志、日志缓存或 WebSocket 推送。
        std::string loginInstructions = std::format(
          "管理后台访问令牌（重启后失效）: {}\n也可尝试一下链接访问\n", AdminAccessToken::token());
        // 枚举当前进程可见的 IPv4 地址；容器内不能据此获取宿主机的局域网地址。
        for (const auto &url: AdminAccessToken::loginUrls(7778)) {
            loginInstructions.append(url+'\n');
    }
    // 与 info 日志使用相同的终端绿色；重定向输出时保持纯文本。
    if (isatty(STDOUT_FILENO)) {
        fmt::print(stdout, fmt::fg(fmt::terminal_color::green), "{}", loginInstructions);
    } else {
        fmt::print(stdout, "{}", loginInstructions);
    }
    std::fflush(stdout);
    }

    /// @brief 处理控制台的退出、登录凭据和日志等级命令；输入关闭或收到停止请求时结束。
    void processConsoleCommands(const std::stop_token &stopToken) {
        std::string command;
        while (!stopToken.stop_requested()) {
            pollfd input{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
            const i32 result = poll(&input, 1, 200);
            if (result == 0) {
                continue;
            }
            if (result < 0 || input.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                return;
            }
            if (!(input.revents & POLLIN) || !(std::cin >> command)) {
                return;
            }
            if (command == "exit") {
                drogon::app().quit();
                return;
            }
            if (command == "login-token") {
                printLoginInstructions();
                continue;
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
        // 这里只校验 MCP 配置；远程工具在首次回复前异步加载，不阻塞程序启动。
        if (auto result = McpTools::instance().initialize(config.getMcpConfig(), ToolRegistry::instance()); !result) {
            Logger::error(0, "MCP", result.error());
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

        printLoginInstructions();
        Logger::info(0, "Admin", "管理后台登录信息已输出到服务终端");

        // 资源历史在无人访问后台时也持续采样，退出事件循环后停止采样。
        ProcessMetrics::instance().start(*drogon::app().getLoop());
        PersonMemoryConsolidator::instance().start(*drogon::app().getLoop());
        drogon::app().run();
        PersonMemoryConsolidator::instance().stop();
        PersonNameResolver::instance().stop();
        ProcessMetrics::instance().stop();

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
        PersonMemoryConsolidator::instance().stop();
        PersonNameResolver::instance().stop();
        ProcessMetrics::instance().stop();
        Logger::critical(0, "Main", fmt::format("程序崩溃: {}", e.what()));
        Logger::shutdown();
        return EXIT_FAILURE;
    } catch (...) {
        PersonMemoryConsolidator::instance().stop();
        PersonNameResolver::instance().stop();
        ProcessMetrics::instance().stop();
        Logger::critical(0, "Main", "程序崩溃: 未知错误");
        Logger::shutdown();
        return EXIT_FAILURE;
    }
    Logger::shutdown();
    return EXIT_SUCCESS;
}
