cmake_minimum_required(VERSION 3.20)
project(insoulforge VERSION 1.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)
set(DEPLOY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build/insoulforge)

# 固定上游版本；源码由 CMake 下载到构建目录，不进入项目仓库。
include(FetchContent)
# 显示首次下载/解压进度，避免配置阶段长时间没有输出。
set(FETCHCONTENT_QUIET OFF)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_APP OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_UI OFF CACHE BOOL "" FORCE)
set(LLAMA_CONNECT OFF CACHE BOOL "" FORCE)
set(GGML_CUDA OFF CACHE BOOL "" FORCE)
set(GGML_METAL OFF CACHE BOOL "" FORCE)
set(GGML_VULKAN OFF CACHE BOOL "" FORCE)
set(GGML_BLAS OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
FetchContent_Declare(llama_cpp
        # b11278：避开后续 kpool_layout 在 AppleClang 下的编译问题。
        URL https://codeload.github.com/ggml-org/llama.cpp/tar.gz/931351ea50dfdd3ee249606f655eef2e9a629daf
        URL_HASH SHA256=d095b9128252182ee78ff1e18865eba44f58b8d6b446ee073e8ccda9c7ee8f6b
        DOWNLOAD_DIR "${FETCHCONTENT_BASE_DIR}/downloads"
        DOWNLOAD_NAME llama.cpp-931351ea50dfdd3ee249606f655eef2e9a629daf.tar.gz
        TLS_VERIFY TRUE
        TIMEOUT 600
        INACTIVITY_TIMEOUT 60)
message(STATUS "Preparing llama.cpp (b11278): downloading/verifying source if needed")
FetchContent_MakeAvailable(llama_cpp)
message(STATUS "llama.cpp source ready: ${llama_cpp_SOURCE_DIR}")

# 禁用 nlohmann/json 的隐式转换运算符（官方开关）：std::string s = j; / return j; 等代码
# 直接编译报错，而不是编译通过、运行时抛 type_error.302；取值一律走 get<T>()/JsonUtil 宽容取值器
add_compile_definitions(JSON_USE_IMPLICIT_CONVERSIONS=0)

find_package(Drogon CONFIG REQUIRED)
# 若 Drogon 没有 SQLite3 应用独立的包
if (NOT TARGET SQLite3 AND NOT TARGET SQLite3::SQLite3 AND NOT TARGET SQLite3_lib)
    find_package(SQLite3 REQUIRED)
endif ()
find_package(spdlog REQUIRED)
find_package(fmt CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(Lua 5.4 REQUIRED)

find_path(GIFLIB_INCLUDE_DIR NAMES gif_lib.h REQUIRED)
find_library(GIFLIB_LIBRARY NAMES gif REQUIRED)

find_package(nlohmann_json CONFIG QUIET)

# 尽量静态链接：能找到系统静态库（.a）就直接链，找不到再回退到 Drogon 传入的动态依赖
find_library(SQLITE3_STATIC_LIB NAMES libsqlite3.a)

# 收集按业务模块组织的全部后端源文件
file(GLOB_RECURSE SRC_FILES CONFIGURE_DEPENDS src/*.cpp)
set(BACKEND_SRC_FILES ${SRC_FILES})
list(FILTER BACKEND_SRC_FILES EXCLUDE REGEX "/src/main\\.cpp$")

add_library(${PROJECT_NAME}_backend OBJECT ${BACKEND_SRC_FILES})
target_compile_features(${PROJECT_NAME}_backend PRIVATE cxx_std_23)
target_include_directories(${PROJECT_NAME}_backend PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        include
)

# 后端由大量翻译单元组成；仅预编译稳定的第三方头文件，项目自身头文件
# 仍须在各自的 .cpp/.hpp 中显式包含，避免 PCH 掩盖依赖关系。
option(INSOULFORGE_ENABLE_PCH "Enable precompiled headers for the backend target" ON)
if (INSOULFORGE_ENABLE_PCH)
    target_precompile_headers(${PROJECT_NAME}_backend PRIVATE
            <drogon/drogon.h>
            <nlohmann/json.hpp>
            <spdlog/spdlog.h>
            <fmt/format.h>
    )
endif ()

add_executable(${PROJECT_NAME}
        src/main.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>
)
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)

target_include_directories(${PROJECT_NAME} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        include
)

# 7. 链接库
target_link_libraries(${PROJECT_NAME} PRIVATE
        llama
        Drogon::Drogon
        spdlog::spdlog_header_only
        fmt::fmt-header-only
        SQLite3_lib
        OpenSSL::Crypto
        PNG::PNG JPEG::JPEG
        ${GIFLIB_LIBRARY}
)
target_link_libraries(${PROJECT_NAME}_backend PRIVATE
        llama
        Drogon::Drogon
        spdlog::spdlog_header_only
        fmt::fmt-header-only
        SQLite3_lib
        OpenSSL::Crypto
        PNG::PNG JPEG::JPEG
        ${GIFLIB_LIBRARY}
)
target_include_directories(${PROJECT_NAME}_backend PRIVATE ${GIFLIB_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME}_backend PRIVATE ${LUA_INCLUDE_DIR})
target_include_directories(${PROJECT_NAME} PRIVATE ${LUA_INCLUDE_DIR})
target_link_libraries(${PROJECT_NAME}_backend PRIVATE ${LUA_LIBRARIES})
target_link_libraries(${PROJECT_NAME} PRIVATE ${LUA_LIBRARIES})

# 静态链接 C++ 标准库和 GCC 运行时
if (CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
    set_target_properties(${PROJECT_NAME} PROPERTIES
            LINK_FLAGS "-static-libgcc -static-libstdc++"
    )
endif ()

# nlohmann/json（header-only）：优先 CMake 包，Homebrew 纯头文件安装时退化为包含目录
if (nlohmann_json_FOUND)
    target_link_libraries(${PROJECT_NAME}_backend PRIVATE nlohmann_json::nlohmann_json)
    target_link_libraries(${PROJECT_NAME} PRIVATE nlohmann_json::nlohmann_json)
else ()
    find_path(NLOHMANN_JSON_INCLUDE_DIR NAMES nlohmann/json.hpp
            HINTS /opt/homebrew/include /usr/local/include /usr/include)
    if (NOT NLOHMANN_JSON_INCLUDE_DIR)
        message(FATAL_ERROR "未找到 nlohmann/json.hpp：macOS 执行 `brew install nlohmann-json`，Debian/Ubuntu 执行 `apt install nlohmann-json3-dev`")
    endif ()
    target_include_directories(${PROJECT_NAME}_backend PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    target_include_directories(${PROJECT_NAME} PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif ()

if (SQLITE3_STATIC_LIB)
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SQLITE3_STATIC_LIB})
endif ()

include(CTest)

# 手动构建的 MCP 交互示例，不启动机器人、不进入默认发布产物。
add_executable(insoulforge_mcp_example EXCLUDE_FROM_ALL
        tests/manual/McpClientExample.cpp
        src/infrastructure/mcp/Client.cpp
        src/infrastructure/mcp/Protocol.cpp)
target_include_directories(insoulforge_mcp_example PRIVATE include)
target_link_libraries(insoulforge_mcp_example PRIVATE Drogon::Drogon)
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_mcp_example PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_mcp_example PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()

if (BUILD_TESTING)
    add_subdirectory(tests)
endif ()

# 前端构建 (Vue3) - 前端源码变化时自动重新构建，输出到 build/insoulforge/public
find_program(NPM_EXECUTABLE NAMES npm npm.cmd REQUIRED)
file(GLOB_RECURSE FRONTEND_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/frontend/src/*)
set(FRONTEND_DEPS
        ${FRONTEND_SOURCES}
        ${CMAKE_CURRENT_SOURCE_DIR}/frontend/index.html
        ${CMAKE_CURRENT_SOURCE_DIR}/frontend/package.json
        ${CMAKE_CURRENT_SOURCE_DIR}/frontend/package-lock.json
        ${CMAKE_CURRENT_SOURCE_DIR}/frontend/tsconfig.json
        ${CMAKE_CURRENT_SOURCE_DIR}/frontend/vite.config.ts)
add_custom_command(
        OUTPUT ${DEPLOY_DIR}/public/index.html
        COMMAND ${NPM_ENV_CMD} ${NPM_EXECUTABLE} install --no-audit --no-fund
        COMMAND ${NPM_ENV_CMD} ${NPM_EXECUTABLE} run build
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/frontend
        DEPENDS ${FRONTEND_DEPS}
        COMMENT "Installing deps and building frontend"
)

add_custom_target(frontend ALL
        DEPENDS ${DEPLOY_DIR}/public/index.html)
add_dependencies(${PROJECT_NAME} frontend)

# 可执行文件直接输出到部署目录 build/insoulforge/exe
set_target_properties(${PROJECT_NAME} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${DEPLOY_DIR}/exe)

# 复用固定上游版本自带的纯词表资源，不需要模型权重。
set(TOKENIZER_RESOURCE_DIR "${DEPLOY_DIR}/resources/tokenizers")
add_custom_target(tokenizer_resources ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory "${TOKENIZER_RESOURCE_DIR}")
add_custom_command(TARGET tokenizer_resources POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${llama_cpp_SOURCE_DIR}/models/ggml-vocab-qwen2.gguf"
        "${TOKENIZER_RESOURCE_DIR}/ggml-vocab-qwen2.gguf")
add_dependencies(${PROJECT_NAME} tokenizer_resources)
