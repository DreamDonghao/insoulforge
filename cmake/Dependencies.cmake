include(FetchContent)

# 源码压缩包共用下载设置，各依赖单独固定 URL 和 SHA-256。
set(dependency_archive_options
    TLS_VERIFY TRUE
    TIMEOUT 600
    INACTIVITY_TIMEOUT 60
)

# llama.cpp b11278 兼容 AppleClang，仅构建词表所需的 CPU 库。
foreach(setting IN ITEMS
    LLAMA_BUILD_COMMON
    LLAMA_BUILD_TESTS
    LLAMA_BUILD_TOOLS
    LLAMA_BUILD_EXAMPLES
    LLAMA_BUILD_SERVER
    LLAMA_BUILD_APP
    LLAMA_BUILD_UI
    LLAMA_CONNECT
    GGML_CUDA
    GGML_METAL
    GGML_VULKAN
    GGML_BLAS
    GGML_OPENMP
    GGML_NATIVE
)
    set(${setting} OFF CACHE BOOL "" FORCE)
endforeach()

FetchContent_Declare(llama_cpp
    EXCLUDE_FROM_ALL
    URL https://codeload.github.com/ggml-org/llama.cpp/tar.gz/931351ea50dfdd3ee249606f655eef2e9a629daf
    URL_HASH SHA256=d095b9128252182ee78ff1e18865eba44f58b8d6b446ee073e8ccda9c7ee8f6b
    DOWNLOAD_NAME llama.cpp-931351ea50dfdd3ee249606f655eef2e9a629daf.tar.gz
    ${dependency_archive_options}
)
message(STATUS "Preparing llama.cpp (b11278): downloading/verifying source if needed")
FetchContent_MakeAvailable(llama_cpp)
message(STATUS "llama.cpp source ready: ${llama_cpp_SOURCE_DIR}")

# spdlog 1.15.3 使用 C++ 标准库格式化。
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
set(SPDLOG_FMT_EXTERNAL_HO OFF CACHE BOOL "" FORCE)
FetchContent_Declare(spdlog
    EXCLUDE_FROM_ALL
    URL https://codeload.github.com/gabime/spdlog/tar.gz/6fa36017cfd5731d617e1a934f0e5ea9c4445b13
    URL_HASH SHA256=5097fb362e79a2bd7247beaf1f8377ed60e274fbe83a4b33e7b73383f0279022
    DOWNLOAD_NAME spdlog-6fa36017cfd5731d617e1a934f0e5ea9c4445b13.tar.gz
    ${dependency_archive_options}
)
FetchContent_MakeAvailable(spdlog)

# nlohmann/json 3.12.0 用多头文件版，项目通过 get<T>() 和 JsonUtil 显式取值。
set(JSON_MultipleHeaders ON CACHE BOOL "" FORCE)
set(JSON_ImplicitConversions OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nlohmann_json
    EXCLUDE_FROM_ALL
    URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
    URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
    DOWNLOAD_NAME nlohmann-json-3.12.0.tar.xz
    ${dependency_archive_options}
)
FetchContent_MakeAvailable(nlohmann_json)

# Lua 5.4.9，供自定义工具在进程内执行。
FetchContent_Declare(lua
    URL https://www.lua.org/ftp/lua-5.4.9.tar.gz
    URL_HASH SHA256=2335b6c582a52654f94612bf10d2f4672805d05329aa6568b1d8cd9e5c6fb8e6
    ${dependency_archive_options}
)
FetchContent_MakeAvailable(lua)

# 源文件按上游 Makefile 的解释器核心和标准库分组。
set(lua_sources
    lapi.c lcode.c lctype.c ldebug.c ldo.c ldump.c lfunc.c lgc.c llex.c lmem.c
    lobject.c lopcodes.c lparser.c lstate.c lstring.c ltable.c ltm.c lundump.c lvm.c lzio.c

    lauxlib.c lbaselib.c lcorolib.c ldblib.c liolib.c lmathlib.c loadlib.c loslib.c
    lstrlib.c ltablib.c lutf8lib.c linit.c
)
list(TRANSFORM lua_sources PREPEND "${lua_SOURCE_DIR}/src/")
add_library(lua STATIC ${lua_sources})
add_library(Lua::Lua ALIAS lua)
target_include_directories(lua SYSTEM PUBLIC "${lua_SOURCE_DIR}/src")
target_compile_definitions(lua PRIVATE
    $<$<PLATFORM_ID:Darwin>:LUA_USE_MACOSX>
    $<$<PLATFORM_ID:Linux>:LUA_USE_LINUX>
)
if(UNIX)
    target_link_libraries(lua PRIVATE m ${CMAKE_DL_LIBS})
endif()

# Drogon 1.9.14 及其依赖。

# JsonCpp 1.9.8，用于 Drogon 的 JSON 接口。
FetchContent_Declare(jsoncpp
    EXCLUDE_FROM_ALL
    URL https://codeload.github.com/open-source-parsers/jsoncpp/tar.gz/8519b8381f3c741ad1421f88237b1deda0b11412
    URL_HASH SHA256=d9008556c83a4be2827e126643e5037a09dede2f6c312bd6a8da7a7d3b1d0298
    DOWNLOAD_NAME jsoncpp-1.9.8.tar.gz
    ${dependency_archive_options}
    OVERRIDE_FIND_PACKAGE
)
unset(dependency_archive_options)

function(add_jsoncpp_dependency)
    set(JSONCPP_WITH_TESTS OFF)
    set(JSONCPP_WITH_PKGCONFIG_SUPPORT OFF)
    set(BUILD_SHARED_LIBS OFF)
    set(BUILD_STATIC_LIBS ON)
    set(BUILD_OBJECT_LIBS OFF)
    FetchContent_MakeAvailable(jsoncpp)

    # Drogon 默认链接 jsoncpp_lib，这里将该名称指向静态库。
    add_library(jsoncpp_lib ALIAS jsoncpp_static)
endfunction()
add_jsoncpp_dependency()

option(OPENSSL_USE_STATIC_LIBS "Prefer static OpenSSL libraries" ON)
find_package(OpenSSL REQUIRED COMPONENTS SSL)

FetchContent_Declare(drogon
    EXCLUDE_FROM_ALL
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG 3aae769cc405cf5abb9d477675c6430924f02c7c # v1.9.14
    GIT_SUBMODULES trantor
    GIT_PROGRESS TRUE
    TLS_VERIFY TRUE
)

function(add_drogon_dependency)
    # 将上游的通用构建选项限制在 Drogon 和 Trantor 的子目录中。
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    set(CMAKE_CXX_STANDARD 20)
    set(BUILD_CTL OFF)
    set(BUILD_EXAMPLES OFF)
    set(BUILD_ORM OFF)
    set(BUILD_TESTING OFF)
    set(BUILD_BROTLI OFF)
    set(BUILD_YAML_CONFIG OFF)
    set(USE_SPDLOG OFF)
    set(USE_SUBMODULE ON)
    set(BUILD_C-ARES ON)
    set(TRANTOR_USE_TLS openssl)
    FetchContent_MakeAvailable(drogon)

    # 新版 libc++ 为 future::get() 标记了 nodiscard，Trantor 的任务队列仍丢弃该返回值。
    target_compile_options(trantor PRIVATE
        $<$<CXX_COMPILER_ID:AppleClang>:-Wno-error=unused-result>
    )
    if(OpenSSL_VERSION VERSION_GREATER_EQUAL 4)
        # OpenSSL 4 仍提供这些证书校验接口，但已将其标记为弃用。
        target_compile_options(trantor PRIVATE
            $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wno-error=deprecated-declarations>
        )
    endif()

    # Trantor 找不到 c-ares 时会回退到按 CPU 核心数建线程池的 NormalResolver。
    get_target_property(trantor_dns_sources trantor SOURCES)
    list(FILTER trantor_dns_sources INCLUDE REGEX "(^|/)AresResolver[.]cc$")
    if(NOT trantor_dns_sources)
        message(FATAL_ERROR
            "Trantor requires c-ares for asynchronous DNS. "
            "Install libc-ares-dev or c-ares and reconfigure."
        )
    endif()
endfunction()
add_drogon_dependency()

# 系统提供的存储和图片解码库。
find_package(SQLite3 REQUIRED)
find_package(PNG REQUIRED)
find_package(JPEG REQUIRED)
find_package(GIF REQUIRED)

# 主程序和消息契约测试在系统提供 SQLite 静态库时也会链接该库。
find_library(SQLITE3_STATIC_LIB NAMES libsqlite3.a)
