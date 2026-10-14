#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include <drogon/utils/coroutine.h>
#include <future>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/mcp/Client.hpp>

namespace {
    enum class Action { Connect, ListTools, CallTool, Close };

    auto readLine(const std::string &prompt) -> std::optional<std::string> {
        std::cout << prompt << std::flush;
        auto line = std::string{};
        if (!std::getline(std::cin, line))
            return std::nullopt;
        return line;
    }

    void printError(const drogonmcp::Error &error) {
        std::cerr << "请求失败：" << error.message << "（错误码：" << error.code << "）\n";
        if (!error.data.is_null())
            std::cerr << error.data.dump(2) << '\n';
    }

    // 完成通知只把结果传回交互线程，不在事件循环中同步等待。
    auto perform(drogonmcp::Client &client, Action action, std::string name, drogonmcp::Json arguments,
      std::shared_ptr<std::promise<bool>> completion) -> drogon::Task<> {
        auto succeeded = false;
        try {
            switch (action) {
                case Action::Connect: {
                    auto result = co_await client.initialize();
                    if (!result)
                        printError(result.error());
                    else {
                        std::cout << "连接成功，服务器信息：\n" << (*result)["serverInfo"].dump(2) << '\n';
                        succeeded = true;
                    }
                    break;
                }
                case Action::ListTools: {
                    auto result = co_await client.listTools();
                    if (!result)
                        printError(result.error());
                    else {
                        std::cout << "共 " << result->size() << " 个工具：\n";
                        for (const auto &tool: *result)
                            std::cout << tool.dump(2) << '\n';
                        succeeded = true;
                    }
                    break;
                }
                case Action::CallTool: {
                    auto result = co_await client.callTool(std::move(name), std::move(arguments));
                    if (!result)
                        printError(result.error());
                    else {
                        const auto toolFailed = result->value("isError", false);
                        std::cout << (toolFailed ? "工具执行失败：\n" : "工具执行结果：\n") << result->dump(2) << '\n';
                        succeeded = !toolFailed;
                    }
                    break;
                }
                case Action::Close: {
                    auto result = co_await client.close();
                    if (!result)
                        printError(result.error());
                    else
                        succeeded = true;
                    break;
                }
            }
        } catch (const std::exception &error) {
            std::cerr << "意外错误：" << error.what() << '\n';
        }
        completion->set_value(succeeded);
    }

    auto execute(trantor::EventLoop &loop, drogonmcp::Client &client, Action action, std::string name = {},
      drogonmcp::Json arguments = drogonmcp::Json::object()) -> bool {
        auto completion = std::make_shared<std::promise<bool>>();
        auto result = completion->get_future();
        loop.queueInLoop([&client, action, name = std::move(name), arguments = std::move(arguments), completion] {
            drogon::async_run([&client, action, name, arguments, completion]() -> drogon::Task<> {
                co_await perform(client, action, name, arguments, completion);
            });
        });
        // 仅终端交互主线程等待；网络事件循环始终独立运行。
        return result.get();
    }
} // namespace

auto main() -> int {
    std::cout << "MCP 客户端交互示例\n";
    auto endpoint = readLine("服务地址（例如 http://127.0.0.1:8080/mcp）：");
    if (!endpoint || endpoint->empty())
        return 0;

    const auto environment = std::getenv("MCP_TOKEN");
    auto token = environment ? std::optional<std::string>{environment}
                             : readLine("Bearer Token（无需认证可留空，输入会显示；也可预设 MCP_TOKEN）：");
    if (!token)
        return 0;

    auto thread = trantor::EventLoopThread{};
    thread.run();
    auto &loop = *thread.getLoop();
    auto client = drogonmcp::Client{loop, {
                                            .endpoint = std::move(*endpoint),
                                            .name = "interactive-example",
                                            .bearerToken = std::move(*token),
                                          }};
    auto exitCode = 0;
    if (!execute(loop, client, Action::Connect))
        exitCode = 1;
    else {
        while (true) {
            auto selection = readLine("\n1. 查看工具\n2. 调用工具\n3. 重新初始化过期会话\n0. 退出\n请选择：");
            if (!selection || *selection == "0")
                break;
            if (*selection == "1")
                execute(loop, client, Action::ListTools);
            else if (*selection == "3")
                execute(loop, client, Action::Connect);
            else if (*selection == "2") {
                auto name = readLine("工具名称：");
                if (!name)
                    break;
                if (name->empty()) {
                    std::cout << "工具名称不能为空\n";
                    continue;
                }
                auto text = readLine("参数 JSON（单行对象，例如 {\"text\":\"hello\"}，留空为 {}）：");
                if (!text)
                    break;
                auto arguments =
                  text->empty() ? drogonmcp::Json::object() : drogonmcp::Json::parse(*text, nullptr, false);
                if (!arguments.is_object()) {
                    std::cout << "参数必须是有效的 JSON 对象，请重新选择调用\n";
                    continue;
                }
                execute(loop, client, Action::CallTool, std::move(*name), std::move(arguments));
            } else
                std::cout << "请输入菜单中的数字\n";
        }
    }
    if (!execute(loop, client, Action::Close))
        exitCode = 1;
    loop.quit();
    thread.wait();
    return exitCode;
}
