# 手动构建的 MCP 交互示例始终可用。
add_executable(insoulforge_mcp_example EXCLUDE_FROM_ALL
    manual/McpClientExample.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/mcp/Client.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/mcp/Protocol.cpp
)
target_include_directories(insoulforge_mcp_example PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_mcp_example PRIVATE Drogon::Drogon nlohmann_json::nlohmann_json)
set_target_properties(insoulforge_mcp_example PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}")

if(NOT BUILD_TESTING)
    return()
endif()

# 集成测试复用后端对象文件及其依赖。
function(add_backend_test target source name)
    add_executable(${target} EXCLUDE_FROM_ALL ${source})
    target_link_libraries(${target} PRIVATE insoulforge_backend)
    add_test(NAME ${name} COMMAND ${target})
    if(ARGC GREATER 3)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${ARGV3})
    endif()
endfunction()

add_backend_test(insoulforge_activity_tests ActivityTests.cpp activities 45)
add_backend_test(insoulforge_message_contract_tests MessageContractTests.cpp message_contracts)
add_backend_test(insoulforge_mcp_tests McpClientTests.cpp mcp_client 30)
add_backend_test(insoulforge_memory_tests MemoryMaintenanceTests.cpp memory_maintenance 30)
add_backend_test(insoulforge_cross_group_tests CrossGroupMessageTests.cpp cross_group_messages 30)
add_backend_test(insoulforge_model_request_tests ModelRequestTests.cpp model_requests 60)
add_backend_test(insoulforge_onebot_reconnect_tests OneBotReconnectTests.cpp onebot_reconnect 30)
add_backend_test(insoulforge_image_tests ImageTests.cpp image_cache)
add_backend_test(insoulforge_favorite_emoji_tests FavoriteEmojiTests.cpp favorite_emojis 45)

if(SQLITE3_STATIC_LIB)
    target_link_libraries(insoulforge_message_contract_tests PRIVATE SQLite3::SQLite3 "${SQLITE3_STATIC_LIB}")
endif()
target_compile_definitions(insoulforge_memory_tests PRIVATE TEST_TOKENIZER_DIRECTORY="${llama_cpp_SOURCE_DIR}/models")

add_executable(insoulforge_process_metrics_tests EXCLUDE_FROM_ALL
    ProcessMetricsTests.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/monitoring/ProcessMetrics.cpp
)
target_include_directories(insoulforge_process_metrics_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_process_metrics_tests PRIVATE Drogon::Drogon)
add_test(NAME process_metrics COMMAND insoulforge_process_metrics_tests)

add_executable(insoulforge_log_buffer_tests EXCLUDE_FROM_ALL
    LogBufferTests.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/UserId.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/SessionId.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/logging/LogBuffer.cpp
)
target_include_directories(insoulforge_log_buffer_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
add_test(NAME log_buffer COMMAND insoulforge_log_buffer_tests)

add_executable(insoulforge_token_counter_tests EXCLUDE_FROM_ALL
    TokenCounterTests.cpp
    ${PROJECT_SOURCE_DIR}/src/infrastructure/tokenizer/TokenCounter.cpp
)
target_include_directories(insoulforge_token_counter_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_token_counter_tests PRIVATE llama)
add_test(NAME token_counter COMMAND insoulforge_token_counter_tests "${llama_cpp_SOURCE_DIR}/models")

add_test(NAME token_counter_missing COMMAND insoulforge_token_counter_tests "${CMAKE_CURRENT_BINARY_DIR}/missing-tokenizer")
set_tests_properties(token_counter_missing PROPERTIES WILL_FAIL TRUE)
