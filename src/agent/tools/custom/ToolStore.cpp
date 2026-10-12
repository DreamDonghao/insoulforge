/// @file ToolStore.cpp
/// @brief 自定义工具文件存储；数据库只保留 Python 解释器设置

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <stdexcept>

#include <agent/tools/custom/ToolStore.hpp>
#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::ToolStore {
    namespace {
        std::mutex storageMutex;
        constexpr size_t kMaxFileBytes = 1024 * 1024;

        void rejectSymlink(const std::filesystem::path &path) {
            if (std::filesystem::is_symlink(std::filesystem::symlink_status(path)))
                throw std::runtime_error("工具存储不允许符号链接: " + path.string());
        }

        auto toolsDirectory(const Database &db) -> std::filesystem::path {
            const auto filename = sqlite3_db_filename(db.handle(), "main");
            if (!filename || !*filename)
                throw std::runtime_error("工具文件存储需要文件数据库");
            const auto root = std::filesystem::path(filename).parent_path() / "tools";
            rejectSymlink(root);
            std::filesystem::create_directories(root);
            return root;
        }

        void validateName(const std::string &name) {
            if (name.empty() || name.size() > 64 || !std::ranges::all_of(name, [](const unsigned char c) {
                    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
                           c == '-';
                }))
                throw std::runtime_error("工具名只能包含英文字母、数字、下划线和连字符，长度为 1–64");
        }

        auto readFile(const std::filesystem::path &path) -> std::string {
            rejectSymlink(path);
            if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > kMaxFileBytes)
                throw std::runtime_error("工具文件不存在或超过 1 MiB: " + path.string());
            std::ifstream stream(path, std::ios::binary);
            stream.exceptions(std::ios::badbit | std::ios::failbit);
            const auto size = std::filesystem::file_size(path);
            auto text = std::string(size, '\0');
            if (size)
                stream.read(text.data(), static_cast<std::streamsize>(size));
            return text;
        }

        void writeFile(const std::filesystem::path &path, const std::string &text) {
            if (text.size() > kMaxFileBytes)
                throw std::runtime_error("工具文件超过 1 MiB");
            rejectSymlink(path);
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.exceptions(std::ios::badbit | std::ios::failbit);
            stream.write(text.data(), static_cast<std::streamsize>(text.size()));
            stream.close();
        }

        /// @brief 恢复保存过程中被中断的目录替换，避免重启后遗失旧工具。
        void recoverSaves(const std::filesystem::path &root) {
            for (const auto &entry: std::filesystem::directory_iterator(root)) {
                const auto name = entry.path().filename().string();
                if (!name.starts_with(".backup-") && !name.starts_with(".pending-"))
                    continue;
                try {
                    rejectSymlink(entry.path());
                    if (!entry.is_directory())
                        continue;
                    if (name.starts_with(".pending-")) {
                        std::filesystem::remove_all(entry.path());
                        continue;
                    }
                    const auto metadata = json::parse(readFile(entry.path() / "tool.json"));
                    const auto toolName = metadata.at("name").get<std::string>();
                    validateName(toolName);
                    const auto id = metadata.at("id").get<i32>();
                    bool published = false;
                    for (const auto &candidate: std::filesystem::directory_iterator(root)) {
                        if (candidate.path().filename().string().starts_with('.'))
                            continue;
                        try {
                            rejectSymlink(candidate.path());
                            if (candidate.is_directory() &&
                                json::parse(readFile(candidate.path() / "tool.json")).at("id").get<i32>() == id)
                                published = true;
                        } catch (const std::exception &) {
                            // 损坏的工具不能作为新版本已成功保存的证据。
                        }
                    }
                    if (published)
                        std::filesystem::remove_all(entry.path());
                    else if (!std::filesystem::exists(root / toolName))
                        std::filesystem::rename(entry.path(), root / toolName);
                    else
                        throw std::runtime_error("恢复旧工具时目录名冲突");
                } catch (const std::exception &error) {
                    Logger::warn(0, "Tool", fmt::format("工具保存恢复失败 '{}': {}", name, error.what()));
                }
            }
        }

        auto loadTools(const std::filesystem::path &root) -> std::vector<CustomTool> {
            recoverSaves(root);
            auto tools = std::vector<CustomTool>();
            for (const auto &entry: std::filesystem::directory_iterator(root)) {
                if (entry.path().filename().string().starts_with('.'))
                    continue;
                try {
                    rejectSymlink(entry.path());
                    if (!entry.is_directory())
                        continue;
                    const auto metadata = json::parse(readFile(entry.path() / "tool.json"));
                    auto tool = CustomTool{};
                    tool.id = metadata.at("id").get<i32>();
                    tool.name = metadata.at("name").get<std::string>();
                    validateName(tool.name);
                    if (tool.id <= 0 || tool.name != entry.path().filename().string())
                        throw std::runtime_error("工具 ID 或目录名不匹配");
                    tool.description = metadata.at("description").get<std::string>();
                    tool.parameters = metadata.value("parameters", json::object()).dump();
                    tool.executorType = metadata.at("executorType").get<std::string>();
                    tool.executorConfig = metadata.value("executorConfig", json::object()).dump();
                    tool.readme = metadata.value("readme", std::string());
                    tool.enabled = metadata.value("enabled", true);
                    if (tool.executorType == "lua" || tool.executorType == "python")
                        tool.scriptContent =
                          readFile(entry.path() / (tool.executorType == "lua" ? "main.lua" : "main.py"));
                    else if (tool.executorType != "http")
                        throw std::runtime_error("未知执行器类型");
                    if (std::ranges::any_of(tools, [&tool](const auto &other) { return other.id == tool.id; }))
                        throw std::runtime_error("重复工具 ID");
                    tools.push_back(std::move(tool));
                } catch (const std::exception &error) {
                    Logger::warn(
                      0, "Tool", fmt::format("跳过无效工具目录 '{}': {}", entry.path().string(), error.what()));
                }
            }
            std::ranges::sort(tools, {}, &CustomTool::id);
            return tools;
        }

        /// @brief 先写入完整的新目录，再替换旧目录；失败时恢复旧版本。
        void saveTool(const std::filesystem::path &root, const CustomTool &tool, const std::string &oldName = {}) {
            validateName(tool.name);
            if (tool.executorType != "lua" && tool.executorType != "python" && tool.executorType != "http")
                throw std::runtime_error("未知执行器类型");
            const auto destination = root / tool.name;
            rejectSymlink(destination);
            if (tool.name != oldName && std::filesystem::exists(destination))
                throw std::runtime_error("工具名已存在");
            const auto staging = root / (".pending-" + std::to_string(tool.id));
            const auto backup = root / (".backup-" + std::to_string(tool.id));
            rejectSymlink(staging);
            rejectSymlink(backup);
            if (std::filesystem::exists(staging) || std::filesystem::exists(backup))
                throw std::runtime_error("发现未完成的工具保存，请检查 .pending/.backup 目录");
            std::filesystem::create_directory(staging);
            bool movedOld = false;
            try {
                const auto metadata = json{{"id", tool.id}, {"name", tool.name}, {"description", tool.description},
                  {"parameters", tool.parameters.empty() ? json::object() : json::parse(tool.parameters)},
                  {"executorType", tool.executorType},
                  {"executorConfig", tool.executorConfig.empty() ? json::object() : json::parse(tool.executorConfig)},
                  {"readme", tool.readme}, {"enabled", tool.enabled}};
                writeFile(staging / "tool.json", metadata.dump(2));
                if (tool.executorType != "http")
                    writeFile(staging / (tool.executorType == "lua" ? "main.lua" : "main.py"), tool.scriptContent);
                if (!oldName.empty()) {
                    std::filesystem::rename(root / oldName, backup);
                    movedOld = true;
                }
                std::filesystem::rename(staging, destination);
            } catch (...) {
                if (movedOld)
                    std::filesystem::rename(backup, root / oldName);
                std::filesystem::remove_all(staging);
                throw;
            }
            if (movedOld)
                std::filesystem::remove_all(backup);
        }

        auto findTool(const std::vector<CustomTool> &tools, const i32 id) -> CustomTool {
            const auto found = std::ranges::find(tools, id, &CustomTool::id);
            if (found == tools.end())
                throw std::runtime_error("工具不存在");
            return *found;
        }
    } // namespace

    auto getCustomTools() -> std::vector<CustomTool> {
        const auto lock = std::lock_guard(storageMutex);
        return loadTools(toolsDirectory(Database::instance()));
    }

    auto getEnabledCustomTools(const Database &database) -> std::vector<CustomTool> {
        const auto lock = std::lock_guard(storageMutex);
        // 内存数据库用于契约测试，没有可持久化的工具目录。
        const auto filename = sqlite3_db_filename(database.handle(), "main");
        if (filename && !*filename)
            return {};
        auto tools = loadTools(toolsDirectory(database));
        std::erase_if(tools, [](const auto &tool) { return !tool.enabled; });
        return tools;
    }

    auto addCustomTool(const CustomTool &tool) -> i32 {
        const auto lock = std::lock_guard(storageMutex);
        const auto root = toolsDirectory(Database::instance());
        const auto tools = loadTools(root);
        auto next = tools.empty() ? i64{1} : i64{tools.back().id} + 1;
        const auto counter = root / ".next-id";
        rejectSymlink(counter);
        if (std::filesystem::exists(counter))
            next = std::max(next, json::parse(readFile(counter)).get<i64>());
        if (next <= 0 || next >= std::numeric_limits<i32>::max())
            throw std::runtime_error("工具 ID 无效或已耗尽");
        // 保留 ID，允许空号，但不能让删除后的旧 ID 指向其他工具。
        const auto temporaryCounter = root / ".next-id.tmp";
        writeFile(temporaryCounter, std::to_string(next + 1));
        std::filesystem::rename(temporaryCounter, counter);
        auto saved = tool;
        saved.id = static_cast<i32>(next);
        saveTool(root, saved);
        return saved.id;
    }

    void updateCustomTool(const CustomTool &tool) {
        const auto lock = std::lock_guard(storageMutex);
        const auto root = toolsDirectory(Database::instance());
        const auto old = findTool(loadTools(root), tool.id);
        saveTool(root, tool, old.name);
    }

    void deleteCustomTool(const i32 id) {
        const auto lock = std::lock_guard(storageMutex);
        const auto root = toolsDirectory(Database::instance());
        const auto tool = findTool(loadTools(root), id);
        std::filesystem::remove_all(root / tool.name);
    }

    void toggleCustomTool(const i32 id) {
        const auto lock = std::lock_guard(storageMutex);
        const auto root = toolsDirectory(Database::instance());
        auto tool = findTool(loadTools(root), id);
        tool.enabled = !tool.enabled;
        saveTool(root, tool, tool.name);
    }

    auto hasCustomTool(const std::string &name) -> bool {
        validateName(name);
        const auto lock = std::lock_guard(storageMutex);
        return std::filesystem::exists(toolsDirectory(Database::instance()) / name);
    }

    auto getCustomToolPython() -> std::string {
        const auto &db = Database::instance();
        const auto lock = std::shared_lock(db.mutex());
        const auto stmt = Statement(db.handle(), "SELECT value FROM settings WHERE key='custom_tool_python'");
        return stmt.step() ? stmt.getText(0) : "python3";
    }

    void setCustomToolPython(const std::string &pythonPath) {
        const auto &db = Database::instance();
        const auto lock = std::unique_lock(db.mutex());
        const auto stmt =
          Statement(db.handle(), "INSERT OR REPLACE INTO settings (key, value) VALUES ('custom_tool_python', ?)");
        stmt.bind(1, pythonPath);
        stmt.exec();
    }
} // namespace insoulforge::ToolStore
