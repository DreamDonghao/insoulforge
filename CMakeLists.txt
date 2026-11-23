cmake_minimum_required(VERSION 4.3)
project(insoulforge VERSION 1.8.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_SHARED_LIBS OFF)
set(DEPLOY_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/insoulforge")
option(INSOULFORGE_ENABLE_PCH "Enable precompiled headers for the backend target" ON)

include(cmake/Dependencies.cmake)

file(GLOB_RECURSE backend_sources CONFIGURE_DEPENDS src/*.cpp)
list(REMOVE_ITEM backend_sources "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

add_library(insoulforge_backend OBJECT ${backend_sources})
target_include_directories(insoulforge_backend PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" include)
target_link_libraries(insoulforge_backend PUBLIC
    Drogon::Drogon
    GIF::GIF
    JPEG::JPEG
    Lua::Lua
    OpenSSL::Crypto
    PNG::PNG
    SQLite3::SQLite3
    llama
    nlohmann_json::nlohmann_json
    spdlog::spdlog_header_only
)

# 预编译稳定的第三方头文件，源码仍须显式包含所需头文件。
if(INSOULFORGE_ENABLE_PCH)
    target_precompile_headers(insoulforge_backend PRIVATE
        <drogon/drogon.h>
        <nlohmann/json.hpp>
        <spdlog/spdlog.h>
    )
endif()

add_executable(insoulforge src/main.cpp)
target_link_libraries(insoulforge PRIVATE insoulforge_backend)
set_target_properties(insoulforge PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DEPLOY_DIR}/exe")

if(SQLITE3_STATIC_LIB)
    target_link_libraries(insoulforge PRIVATE SQLite3::SQLite3 "${SQLITE3_STATIC_LIB}")
endif()

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND NOT APPLE)
    target_link_options(insoulforge PRIVATE -static-libgcc -static-libstdc++)
endif()

include(cmake/DeploymentAssets.cmake)
add_dependencies(insoulforge frontend tokenizer_resources)

include(CTest)
add_subdirectory(tests)
