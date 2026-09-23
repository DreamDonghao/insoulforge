/// @file main.cpp
/// @brief 程序入口 - insoulforge 主程序
/// @author donghao
/// @date 2026-04-02
/// @details 初始化并启动 QQ 群聊机器人服务：
///          - 日志系统初始化：控制台 + 滚动文件（Logger::init）
///          - 数据库初始化：SQLite 持久化存储
///          - 配置加载：从数据库读取 LLM、知识库、QQ Bot 配置
///          - Agent 系统初始化：注册内置工具和自定义工具
///          - HTTP 服务启动：监听 7778 端口，提供管理界面和 API
///          支持通过输入 "quit" 命令优雅退出

#include <infrastructure/NumericTypes.hpp>

#include <admin/AdminStore.hpp>
#include <admin/auth/AdminAccessToken.hpp>
#include <admin/http/AdminResponse.hpp>
#include <agent/ability/TaskScheduler.hpp>
#include <agent/runtime/AgentSystem.hpp>
#include <conversation/session/QQNameDirectory.hpp>
#include <conversation/session/SessionStore.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/config/ConfigStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <media/ImageDescriptionStore.hpp>
#include <onebot/OneBotWebSocketClient.hpp>
#include <poll.h>
#include <unistd.h>

int main() {
    using namespace insoulforge;
    try {
        // 系统初始化
        Logger::init();
        AdminAccessToken::initialize();
        ConfigStore::initialize();
        auto &database = Database::instance();
        database.initialize("data/insoulforge.db");
        static_cast<void>(ImageDescriptionStore::purgeExpired());

        auto &config = Config::instance();
        config.loadFromStorage();

        // 初始化 QQ 昵称
        QQNameDirectory::setCustomName(config.selfQQNumber, config.botName + "(我)");

        // 初始化 Agent 系统
        AgentSystem::instance().initialize();
        static_cast<void>(OneBotEventWorkflow::instance());
        OneBotWebSocketClient::instance().start();

        // 启动定时任务调度器
        TaskScheduler::instance().start();

        Logger::info(0, "Main",
          fmt::format("系统初始化完成 | enabled_sessions={} | admins={}", std::ssize(SessionStore::getEnabledGroups()),
            std::ssize(AdminStore::getAdmins())));

        // 启动服务
        // 启动控制台命令线程
        std::jthread commandThread([](const std::stop_token &stopToken) {
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
        });

        drogon::app().registerPreRoutingAdvice(
          [](const drogon::HttpRequestPtr &request, drogon::AdviceCallback &&callback,
            drogon::AdviceChainCallback &&next) {
              const std::string &path = request->path();
              const bool isAdminApi = path.starts_with("/admin/api/");
              const bool isAdminWebSocket = path == "/admin/ws" || path == "/admin/logs/ws";
              if (const bool isPublicAuthEndpoint = path == "/admin/api/auth/login" || path == "/admin/api/auth/status";
                (!isAdminApi && !isAdminWebSocket) || isPublicAuthEndpoint || AdminAccessToken::isAuthorized(request)) {
                  next();
                  return;
              }

              const auto response = jsonResponse(AdminResponse::failJson("未登录或登录已失效"));
              response->setStatusCode(drogon::k401Unauthorized);
              callback(response);
          });

        drogon::app().addListener("0.0.0.0", 7778);
        drogon::app().setDocumentRoot("public");
        Logger::info(0, "Main", "HTTP 服务启动 | port=7778");

        Logger::info(0, "Admin", std::format("管理后台访问令牌（重启后失效）: {}", AdminAccessToken::token()));
        Logger::info(0, "Admin", "也可尝试一下链接访问");
        for (const auto &url: AdminAccessToken::loginUrls(7778)) {
            Logger::info(0, "Admin", " " + url + " ");
        }

        drogon::app().run();

        // stdin 读取不能依赖 SIGINT 自动返回；先结束命令线程，避免 jthread 析构时阻塞退出。
        commandThread.request_stop();
        commandThread.join();

        // 先停调度线程再关库，避免触发中的任务写已关闭的数据库
        TaskScheduler::instance().stop();
        OneBotWebSocketClient::instance().stop();
        OneBotEventWorkflow::instance().flushMessageListsToStorage();
        database.close();
        Logger::info(0, "Main", "系统正常退出");
    } catch (const std::exception &e) {
        Logger::critical(0, "Main", fmt::format("程序崩溃: {}", e.what()));
        Logger::shutdown();
        return 1;
    } catch (...) {
        Logger::critical(0, "Main", "程序崩溃: 未知错误");
        Logger::shutdown();
        return 1;
    }
    Logger::shutdown();
    return 0;
}
