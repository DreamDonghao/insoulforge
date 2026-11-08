# Backend integration tests reuse production objects and their common dependencies.
function(add_backend_test target source name)
    add_executable(${target} EXCLUDE_FROM_ALL ${source} $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
    target_compile_features(${target} PRIVATE cxx_std_23)
    target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(${target} PRIVATE
            Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
            SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES} llama)
    if(nlohmann_json_FOUND)
        target_link_libraries(${target} PRIVATE nlohmann_json::nlohmann_json)
    else()
        target_include_directories(${target} PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
    endif()
    add_test(NAME ${name} COMMAND ${target})
    if(ARGC GREATER 3)
        set_tests_properties(${name} PROPERTIES TIMEOUT ${ARGV3})
    endif()
endfunction()

add_backend_test(insoulforge_activity_tests ActivityTests.cpp activities 45)
add_backend_test(insoulforge_message_contract_tests MessageContractTests.cpp message_contracts)
target_include_directories(insoulforge_message_contract_tests PRIVATE ${PROJECT_SOURCE_DIR})
add_backend_test(insoulforge_mcp_tests McpClientTests.cpp mcp_client 30)
target_include_directories(insoulforge_mcp_tests PRIVATE ${PROJECT_SOURCE_DIR})
add_backend_test(insoulforge_memory_tests MemoryMaintenanceTests.cpp memory_maintenance 30)
target_include_directories(insoulforge_memory_tests PRIVATE ${PROJECT_SOURCE_DIR})
add_backend_test(insoulforge_cross_group_tests CrossGroupMessageTests.cpp cross_group_messages 30)
target_include_directories(insoulforge_cross_group_tests PRIVATE ${PROJECT_SOURCE_DIR})
add_backend_test(insoulforge_model_request_tests ModelRequestTests.cpp model_requests 60)
target_include_directories(insoulforge_model_request_tests PRIVATE ${PROJECT_SOURCE_DIR})
add_backend_test(insoulforge_onebot_reconnect_tests OneBotReconnectTests.cpp onebot_reconnect 30)
target_include_directories(insoulforge_onebot_reconnect_tests PRIVATE ${PROJECT_SOURCE_DIR})
add_backend_test(insoulforge_image_tests ImageTests.cpp image_cache)
add_backend_test(insoulforge_favorite_emoji_tests FavoriteEmojiTests.cpp favorite_emojis 45)
target_include_directories(insoulforge_message_contract_tests PRIVATE ${LUA_INCLUDE_DIR})
if(SQLITE3_STATIC_LIB)
    target_link_libraries(insoulforge_message_contract_tests PRIVATE ${SQLITE3_STATIC_LIB})
endif()

add_executable(insoulforge_process_metrics_tests EXCLUDE_FROM_ALL
        ProcessMetricsTests.cpp
        ${PROJECT_SOURCE_DIR}/src/infrastructure/monitoring/ProcessMetrics.cpp)
target_include_directories(insoulforge_process_metrics_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_process_metrics_tests PRIVATE Drogon::Drogon)
add_test(NAME process_metrics COMMAND insoulforge_process_metrics_tests)

add_executable(insoulforge_log_buffer_tests EXCLUDE_FROM_ALL
        LogBufferTests.cpp
        ${PROJECT_SOURCE_DIR}/src/infrastructure/UserId.cpp
        ${PROJECT_SOURCE_DIR}/src/infrastructure/SessionId.cpp
        ${PROJECT_SOURCE_DIR}/src/infrastructure/logging/LogBuffer.cpp)
target_compile_features(insoulforge_log_buffer_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_log_buffer_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_log_buffer_tests PRIVATE fmt::fmt-header-only)
add_test(NAME log_buffer COMMAND insoulforge_log_buffer_tests)

add_executable(insoulforge_token_counter_tests EXCLUDE_FROM_ALL TokenCounterTests.cpp
        ${PROJECT_SOURCE_DIR}/src/infrastructure/tokenizer/TokenCounter.cpp)
target_include_directories(insoulforge_token_counter_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_token_counter_tests PRIVATE llama)
add_test(NAME token_counter COMMAND insoulforge_token_counter_tests "${llama_cpp_SOURCE_DIR}/models")

add_test(NAME token_counter_missing COMMAND insoulforge_token_counter_tests "${CMAKE_CURRENT_BINARY_DIR}/missing-tokenizer")
set_tests_properties(token_counter_missing PROPERTIES WILL_FAIL TRUE)

target_compile_definitions(insoulforge_memory_tests PRIVATE TEST_TOKENIZER_DIRECTORY="${llama_cpp_SOURCE_DIR}/models")
