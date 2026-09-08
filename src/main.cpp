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

#include <agent/runtime/AgentSystem.hpp>
#include <infrastructure/config/Config.hpp>
#include <drogon/drogon.h>
#include <include/agent/ability/TaskScheduler.hpp>
#include <iostream>
#include <iterator>
#include <conversation/session/QQNameDirectory.hpp>
#include <spdlog/spdlog.h>
#include <admin/AdminStore.hpp>
#include <infrastructure/storage/Database.hpp>
#include <conversation/session/SessionStore.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>

int main() {
    using namespace insoulforge;
    try {
        // 系统初始化
        Logger::init();
        auto &database = Database::instance();
        database.initialize("data/insoulforge.db");

        auto &config = Config::instance();
        config.loadFromDatabase();

        // 初始化 QQ 昵称
        QQNameDirectory::setCustomName(config.selfQQNumber, config.botName + "(我)");

        // 初始化 Agent 系统
        AgentSystem::instance().initialize();
        static_cast<void>(OneBotEventWorkflow::instance());

        // 启动定时任务调度器
        TaskScheduler::instance().start();

        spdlog::info("系统初始化完成 - 启用群: {}, 管理员: {}", std::ssize(SessionStore::getEnabledGroups()),
          std::ssize(AdminStore::getAdmins()));

        // 启动服务
        // 启动控制台命令线程
        std::jthread commandThread([]() {
            std::string command;
            while (std::cin >> command) {
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
                        spdlog::info("日志等级已切换为 {}", level);
                    } else {
                        spdlog::warn("无效的日志等级: {}", level);
                    }
                    continue;
                }
                spdlog::warn("未知命令: {}", command);
            }
        });

        drogon::app().addListener("0.0.0.0", 7778);
        drogon::app().setDocumentRoot("public");
        spdlog::info("HTTP服务启动，端口: 7778");
        spdlog::info("管理后台: http://localhost:7778/index.html");

        drogon::app().run();

        // 先停调度线程再关库，避免触发中的任务写已关闭的数据库
        TaskScheduler::instance().stop();
        OneBotEventWorkflow::instance().flushMessageListsToStorage();
        database.close();
        spdlog::info("系统正常退出");
    } catch (const std::exception &e) {
        spdlog::critical("程序崩溃: {}", e.what());
        Logger::shutdown();
        return 1;
    } catch (...) {
        spdlog::critical("程序崩溃: 未知错误");
        Logger::shutdown();
        return 1;
    }
    Logger::shutdown();
    return 0;
}
