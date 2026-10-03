/// @file LuaToolExecutor.cpp
/// @brief 每次调用独立的 Lua 状态，宿主请求交由 Drogon 协程处理

#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

#include <lua.hpp>

#include <agent/ability/AsyncTaskManager.hpp>
#include <agent/tools/custom/LuaToolExecutor.hpp>
#include <conversation/message/SessionId.hpp>
#include <onebot/messaging/MessageService.hpp>

namespace insoulforge::LuaToolExecutor {
    namespace {
        constexpr size_t kMaxSourceBytes = 256 * 1024;
        constexpr size_t kMaxMemoryBytes = 16 * 1024 * 1024;
        constexpr i32 kMaxJsonDepth = 32;
        constexpr i32 kMaxInstructions = 1'000'000;
        constexpr i32 kMaxHostOperations = 32;
        constexpr auto kMaxCpuTime = std::chrono::milliseconds(100);

        constexpr std::string_view kHostApi = R"lua(
            local yield_to_host = coroutine.yield
            bot = {}
            function bot.send_message(text)
                return yield_to_host({op = "send_message", text = text})
            end
            function bot.start_task(description, payload)
                return yield_to_host({op = "start_task", description = description, payload = payload})
            end
            coroutine = nil
        )lua";

        struct MemoryBudget {
            size_t used = 0;
        };

        auto allocate(void *userData, void *pointer, size_t oldSize, size_t newSize) -> void * {
            auto &budget = *static_cast<MemoryBudget *>(userData);
            const size_t previous = pointer ? oldSize : 0;
            if (newSize == 0) {
                std::free(pointer);
                budget.used -= previous;
                return nullptr;
            }
            if (newSize > previous && newSize - previous > kMaxMemoryBytes - budget.used) {
                return nullptr;
            }
            void *result = std::realloc(pointer, newSize);
            if (result) {
                budget.used = budget.used - previous + newSize;
            }
            return result;
        }

        struct ExecutionBudget {
            i32 instructions = 0;
            std::chrono::steady_clock::time_point started = std::chrono::steady_clock::now();
        };

        void checkBudget(lua_State *state, lua_Debug *) {
            auto *budget = *static_cast<ExecutionBudget **>(lua_getextraspace(state));
            budget->instructions += 1000;
            if (budget->instructions > kMaxInstructions ||
                std::chrono::steady_clock::now() - budget->started > kMaxCpuTime) {
                luaL_error(state, "Lua 执行资源超限");
            }
        }

        [[nodiscard]] auto luaError(lua_State *state) -> std::runtime_error {
            const char *message = lua_tostring(state, -1);
            return std::runtime_error(message ? message : "未知 Lua 错误");
        }

        void pushJson(lua_State *state, const json &value, const i32 depth = 0) {
            if (depth > kMaxJsonDepth) {
                throw std::runtime_error("JSON 嵌套过深");
            }
            if (value.is_object()) {
                lua_createtable(state, 0, static_cast<i32>(value.size()));
                for (auto it = value.begin(); it != value.end(); ++it) {
                    pushJson(state, it.value(), depth + 1);
                    lua_setfield(state, -2, it.key().c_str());
                }
            } else if (value.is_array()) {
                lua_createtable(state, static_cast<i32>(value.size()), 0);
                for (size_t index = 0; index < value.size(); ++index) {
                    pushJson(state, value[index], depth + 1);
                    lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
                }
            } else if (value.is_string()) {
                const auto &text = value.get_ref<const std::string &>();
                lua_pushlstring(state, text.data(), text.size());
            } else if (value.is_boolean()) {
                lua_pushboolean(state, value.get<bool>());
            } else if (value.is_number_integer()) {
                lua_pushinteger(state, value.get<lua_Integer>());
            } else if (value.is_number()) {
                lua_pushnumber(state, value.get<lua_Number>());
            } else {
                lua_pushnil(state);
            }
        }

        [[nodiscard]] auto readJson(lua_State *state, i32 index, i32 depth, std::unordered_set<const void *> &ancestors)
          -> json {
            if (depth > kMaxJsonDepth) {
                throw std::runtime_error("Lua 表嵌套过深");
            }
            index = lua_absindex(state, index);
            switch (lua_type(state, index)) {
                case LUA_TNIL:
                    return nullptr;
                case LUA_TBOOLEAN:
                    return lua_toboolean(state, index) != 0;
                case LUA_TNUMBER:
                    if (lua_isinteger(state, index)) {
                        return lua_tointeger(state, index);
                    }
                    return lua_tonumber(state, index);
                case LUA_TSTRING: {
                    size_t length = 0;
                    const char *value = lua_tolstring(state, index, &length);
                    return std::string(value, length);
                }
                case LUA_TTABLE:
                    break;
                default:
                    throw std::runtime_error("Lua 返回了不支持的数据类型");
            }

            const void *identity = lua_topointer(state, index);
            if (!ancestors.insert(identity).second) {
                throw std::runtime_error("Lua 表存在循环引用");
            }
            json object = json::object();
            json array = json::array();
            bool isArray = true;
            bool hasNumericKey = false;
            bool hasStringKey = false;
            size_t count = 0;
            const size_t length = lua_rawlen(state, index);
            lua_pushnil(state);
            while (lua_next(state, index) != 0) {
                ++count;
                const json item = readJson(state, -1, depth + 1, ancestors);
                if (lua_type(state, -2) == LUA_TNUMBER && lua_isinteger(state, -2)) {
                    hasNumericKey = true;
                    const lua_Integer key = lua_tointeger(state, -2);
                    if (key > 0 && static_cast<size_t>(key) <= length) {
                        array[static_cast<size_t>(key - 1)] = item;
                    } else {
                        isArray = false;
                    }
                } else if (lua_type(state, -2) == LUA_TSTRING) {
                    hasStringKey = true;
                    size_t keyLength = 0;
                    const char *key = lua_tolstring(state, -2, &keyLength);
                    object[std::string(key, keyLength)] = item;
                    isArray = false;
                } else {
                    lua_pop(state, 1);
                    ancestors.erase(identity);
                    throw std::runtime_error("Lua 表的键必须是字符串或正整数");
                }
                lua_pop(state, 1);
            }
            ancestors.erase(identity);
            if (hasNumericKey && hasStringKey) {
                throw std::runtime_error("Lua 表不能混用数组下标与字符串键");
            }
            return isArray && count == length && count != 0 ? std::move(array) : std::move(object);
        }

        [[nodiscard]] auto readJson(lua_State *state, const i32 index) -> json {
            std::unordered_set<const void *> ancestors;
            return readJson(state, index, 0, ancestors);
        }

        class LuaState {
        public:
            LuaState()
#if LUA_VERSION_NUM >= 505
                : m_state(lua_newstate(allocate, &m_memory, 0)) {
#else
                : m_state(lua_newstate(allocate, &m_memory)) {
#endif
                if (!m_state) {
                    throw std::runtime_error("无法创建 Lua 状态");
                }
                luaL_openlibs(m_state);
                for (const char *name:
                  {"io", "os", "package", "debug", "require", "dofile", "loadfile", "load", "collectgarbage"}) {
                    lua_pushnil(m_state);
                    lua_setglobal(m_state, name);
                }
                *static_cast<ExecutionBudget **>(lua_getextraspace(m_state)) = &m_execution;
                lua_sethook(m_state, checkBudget, LUA_MASKCOUNT, 1000);
                load(kHostApi);
            }

            LuaState(const LuaState &) = delete;
            auto operator=(const LuaState &) -> LuaState & = delete;

            ~LuaState() { lua_close(m_state); }

            void load(const std::string_view source) {
                if (source.size() > kMaxSourceBytes) {
                    throw std::runtime_error("Lua 脚本超过 256 KiB");
                }
                if (luaL_loadbufferx(m_state, source.data(), source.size(), "custom_tool", "t") != LUA_OK) {
                    throw luaError(m_state);
                }
                if (lua_pcall(m_state, 0, 0, 0) != LUA_OK) {
                    throw luaError(m_state);
                }
            }

            [[nodiscard]] auto hasFunction(const char *name) const -> bool {
                lua_getglobal(m_state, name);
                const bool found = lua_isfunction(m_state, -1);
                lua_pop(m_state, 1);
                return found;
            }

            [[nodiscard]] auto state() const -> lua_State * { return m_state; }

            void resetCpuClock() { m_execution.started = std::chrono::steady_clock::now(); }

            void attach(lua_State *thread) {
                *static_cast<ExecutionBudget **>(lua_getextraspace(thread)) = &m_execution;
                lua_sethook(thread, checkBudget, LUA_MASKCOUNT, 1000);
            }

        private:
            MemoryBudget m_memory;
            ExecutionBudget m_execution;
            lua_State *m_state;
        };

        [[nodiscard]] auto contextJson(const u64 sessionId) -> json {
            return {{"session_id", std::to_string(sessionId)}, {"is_private", SessionId::isPrivate(sessionId)}};
        }
    } // namespace

    auto validate(const std::string_view source) -> std::optional<std::string> {
        try {
            LuaState vm;
            vm.load(source);
            if (!vm.hasFunction("run")) {
                return "Lua 脚本必须定义 run(args, ctx)";
            }
            return std::nullopt;
        } catch (const std::exception &error) {
            return error.what();
        }
    }

    auto execute(std::string source, json args, const u64 sessionId, const bool background, const bool testMode)
      -> drogon::Task<std::string> {
        LuaState vm;
        vm.load(source);
        const char *entry = background ? "background" : "run";
        if (!vm.hasFunction(entry)) {
            throw std::runtime_error(std::string("Lua 脚本缺少 ") + entry + " 入口");
        }

        lua_State *main = vm.state();
        lua_State *thread = lua_newthread(main);
        const i32 threadReference = luaL_ref(main, LUA_REGISTRYINDEX);
        vm.attach(thread);
        lua_getglobal(main, entry);
        lua_xmove(main, thread, 1);
        pushJson(thread, args);
        pushJson(thread, contextJson(sessionId));

        i32 argumentCount = 2;
        i32 hostOperations = 0;
        while (true) {
            vm.resetCpuClock();
            i32 resultCount = 0;
            const i32 status = lua_resume(thread, nullptr, argumentCount, &resultCount);
            if (status == LUA_OK) {
                if (resultCount != 1 || lua_type(thread, -1) != LUA_TSTRING) {
                    throw std::runtime_error("Lua 入口必须返回字符串");
                }
                size_t length = 0;
                const char *result = lua_tolstring(thread, -1, &length);
                std::string text(result, length);
                lua_pop(thread, resultCount);
                luaL_unref(main, LUA_REGISTRYINDEX, threadReference);
                co_return text;
            }
            if (status != LUA_YIELD) {
                throw luaError(thread);
            }
            if (resultCount != 1) {
                throw std::runtime_error("Lua 宿主请求格式错误");
            }
            if (++hostOperations > kMaxHostOperations) {
                throw std::runtime_error("Lua 宿主操作次数超限");
            }
            json request = readJson(thread, -1);
            lua_pop(thread, resultCount);
            if (!request.is_object()) {
                throw std::runtime_error("Lua 宿主请求必须是对象");
            }

            json response;
            const std::string operation = getStr(request, "op");
            if (operation == "send_message") {
                const std::string text = getStr(request, "text");
                if (text.empty()) {
                    response = {{"ok", false}, {"error", "消息内容为空"}};
                } else if (testMode) {
                    response = {{"ok", true}, {"message_id", "test-message"}};
                } else if (sessionId == 0) {
                    response = {{"ok", false}, {"error", "会话 ID 无效"}};
                } else {
                    const std::optional<u64> sent =
                      SessionId::isPrivate(sessionId)
                        ? co_await MessageService::sendPrivateMsg(SessionId::privateUserId(sessionId), text)
                        : co_await MessageService::sendGroupMsg(sessionId, text);
                    response = sent ? json{{"ok", true}, {"message_id", std::to_string(*sent)}}
                                    : json{{"ok", false}, {"error", "消息发送失败"}};
                }
            } else if (operation == "start_task") {
                const std::string description = getStr(request, "description");
                if (description.empty()) {
                    response = {{"status", "error"}, {"error", "任务描述为空"}};
                } else if (!vm.hasFunction("background")) {
                    response = {{"status", "error"}, {"error", "Lua 脚本缺少 background 入口"}};
                } else if (testMode) {
                    response = {{"status", "started"}, {"task_id", "test-task"}};
                } else {
                    const auto result = AsyncTaskManager::instance().start(sessionId, description,
                      [script = source, payload = request.value("payload", json::object()),
                        sessionId]() mutable -> drogon::Task<std::string> {
                          return execute(std::move(script), std::move(payload), sessionId, true);
                      });
                    std::string state;
                    switch (result.status) {
                        case AsyncTaskManager::StartResult::Status::Started:
                            state = "started";
                            break;
                        case AsyncTaskManager::StartResult::Status::Busy:
                            state = "busy";
                            break;
                        case AsyncTaskManager::StartResult::Status::Stopping:
                            state = "stopping";
                            break;
                    }
                    response = {{"status", state}, {"task_id", result.taskId}};
                }
            } else {
                throw std::runtime_error("未知 Lua 宿主操作: " + operation);
            }
            pushJson(thread, response);
            argumentCount = 1;
        }
    }
} // namespace insoulforge::LuaToolExecutor
