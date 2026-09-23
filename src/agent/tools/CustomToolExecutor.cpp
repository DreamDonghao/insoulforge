/// @file CustomToolExecutor.cpp
/// @brief 自定义 Python 与 HTTP 工具执行实现

#include <infrastructure/NumericTypes.hpp>

#include <fstream>
#include <infrastructure/logging/Logger.hpp>
#include <random>


#include <agent/tools/ToolRuntime.hpp>
#include <agent/tools/ToolStore.hpp>
#include <infrastructure/http/HttpUtil.hpp>

namespace insoulforge {
    namespace {
        /// @brief 管理临时文件，并在离开作用域时删除
        class TemporaryFile {
        public:
            explicit TemporaryFile(std::string path) : m_path(std::move(path)) {}

            TemporaryFile(const TemporaryFile &) = delete;
            TemporaryFile &operator=(const TemporaryFile &) = delete;

            ~TemporaryFile() {
                std::error_code error;
                std::filesystem::remove(m_path, error);
            }

            [[nodiscard]] const std::string &path() const noexcept { return m_path; }

        private:
            std::string m_path;
        };

        /// @brief 在当前平台启动读取子进程输出的管道
        [[nodiscard]] FILE *openReadPipe(const char *command) {
#ifdef _WIN32
            return _popen(command, "r");
#else
            return popen(command, "r");
#endif
        }

        /// @brief 关闭由 openReadPipe 创建的管道
        [[nodiscard]] i32 closePipe(FILE *pipe) {
#ifdef _WIN32
            return _pclose(pipe);
#else
            return pclose(pipe);
#endif
        }

        /// @brief 移除脚本开头的空白行，保留内部缩进
        [[nodiscard]] std::string trimScriptPrefix(std::string script) {
            const size_t firstNonSpace = script.find_first_not_of(" \t\n\r");
            if (firstNonSpace != std::string::npos) {
                script.erase(0, firstNonSpace);
            }
            return script;
        }
    } // namespace

    drogon::Task<std::string> ToolRuntime::executePythonTool(std::string scriptContent, json args) {
        if (scriptContent.empty()) {
            co_return std::string("脚本内容为空");
        }

        std::random_device randomDevice;
        const auto temporaryDirectory = std::filesystem::temp_directory_path();
        TemporaryFile scriptFile((temporaryDirectory / ("tool_" + std::to_string(randomDevice()) + ".py")).string());
        TemporaryFile inputFile(
          (temporaryDirectory / ("tool_input_" + std::to_string(randomDevice()) + ".json")).string());
        const std::string script = trimScriptPrefix(std::move(scriptContent));

        std::ofstream(scriptFile.path()) << script;
        std::ofstream(inputFile.path()) << dumpJson(args, false);

        const std::string command =
          ToolStore::getCustomToolPython() + " " + scriptFile.path() + " " + inputFile.path() + " 2>&1";
        Logger::debug(0, "Tool", fmt::format("Python脚本内容:\n{}", script));
        Logger::debug(0, "Tool", fmt::format("执行Python工具: {}", command));

        struct PipeCloser {
            void operator()(FILE *pipe) const noexcept {
                if (pipe) {
                    std::ignore = closePipe(pipe);
                }
            }
        };
        std::unique_ptr<FILE, PipeCloser> pipe(openReadPipe(command.c_str()));
        if (!pipe) {
            co_return std::string("执行脚本失败");
        }

        std::array<char, 4096> buffer{};
        std::string result;
        while (fgets(buffer.data(), static_cast<i32>(buffer.size()), pipe.get())) {
            result += buffer.data();
        }
        if (const i32 exitCode = closePipe(pipe.release()); exitCode != 0) {
            Logger::warn(0, "Tool", fmt::format("Python工具执行返回非零: {}, 输出: {}", exitCode, result));
        }

        while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
            result.pop_back();
        }
        co_return result;
    }

    drogon::Task<std::string> ToolRuntime::executeHttpTool(std::string config, json args, const u64 sessionId) {
        json configJson;
        if (!tryParseJson(config, configJson)) {
            Logger::error(0, "Tool", fmt::format("HTTP工具配置解析失败"));
            co_return std::string("工具配置错误");
        }

        const std::string url = getStr(configJson, "url");
        const std::string method = getStr(configJson, "method", "POST");
        const size_t protocolEnd = url.find("://");
        if (url.empty()) {
            co_return std::string("未配置URL");
        }
        if (protocolEnd == std::string::npos) {
            co_return std::string("URL格式错误");
        }

        const std::string_view address(url.data() + protocolEnd + 3, url.size() - protocolEnd - 3);
        const size_t pathStart = address.find('/');
        const std::string baseUrl = url.substr(0, protocolEnd + 3) + std::string(address.substr(0, pathStart));
        const std::string path = pathStart == std::string_view::npos ? "/" : std::string(address.substr(pathStart));
        const bool isGet = method == "GET";
        const auto response = co_await HttpUtil::send("[HttpTool]", baseUrl, path, isGet ? drogon::Get : drogon::Post,
          isGet ? json() : std::move(args), "", 30.0, sessionId);
        if (!response) {
            co_return std::string("HTTP请求失败");
        }
        co_return std::string((*response)->getBody());
    }
} // namespace insoulforge
