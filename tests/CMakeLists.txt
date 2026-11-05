# Tests are organized as a CMake subproject to keep production targets separate.

add_executable(insoulforge_activity_tests EXCLUDE_FROM_ALL
        ActivityTests.cpp $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_activity_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_activity_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_activity_tests PRIVATE
        Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
        SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES})
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_activity_tests PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_activity_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
add_test(NAME activities COMMAND insoulforge_activity_tests)
set_tests_properties(activities PROPERTIES TIMEOUT 45)

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

add_executable(insoulforge_message_contract_tests EXCLUDE_FROM_ALL
        MessageContractTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_message_contract_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_message_contract_tests PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/include
        ${LUA_INCLUDE_DIR})
target_link_libraries(insoulforge_message_contract_tests PRIVATE
        Drogon::Drogon
        spdlog::spdlog_header_only
        fmt::fmt-header-only
        SQLite3_lib
        OpenSSL::Crypto
        PNG::PNG JPEG::JPEG
        ${GIFLIB_LIBRARY}
        ${LUA_LIBRARIES})

if (nlohmann_json_FOUND)
    target_link_libraries(insoulforge_message_contract_tests PRIVATE nlohmann_json::nlohmann_json)
else ()
    target_include_directories(insoulforge_message_contract_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif ()

if (SQLITE3_STATIC_LIB)
    target_link_libraries(insoulforge_message_contract_tests PRIVATE ${SQLITE3_STATIC_LIB})
endif ()

add_test(NAME message_contracts COMMAND insoulforge_message_contract_tests)

add_executable(insoulforge_mcp_tests EXCLUDE_FROM_ALL
        McpClientTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_mcp_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_mcp_tests PRIVATE
        ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_mcp_tests PRIVATE
        Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
        SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES})
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_mcp_tests PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_mcp_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
add_test(NAME mcp_client COMMAND insoulforge_mcp_tests)
set_tests_properties(mcp_client PROPERTIES TIMEOUT 30)

add_executable(insoulforge_memory_tests EXCLUDE_FROM_ALL
        MemoryMaintenanceTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_memory_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_memory_tests PRIVATE
        ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_memory_tests PRIVATE
        Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
        SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES})
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_memory_tests PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_memory_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
add_test(NAME memory_maintenance COMMAND insoulforge_memory_tests)
set_tests_properties(memory_maintenance PROPERTIES TIMEOUT 30)

add_executable(insoulforge_cross_group_tests EXCLUDE_FROM_ALL
        CrossGroupMessageTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_cross_group_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_cross_group_tests PRIVATE
        ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_cross_group_tests PRIVATE
        Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
        SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES})
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_cross_group_tests PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_cross_group_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
add_test(NAME cross_group_messages COMMAND insoulforge_cross_group_tests)
set_tests_properties(cross_group_messages PROPERTIES TIMEOUT 30)

add_executable(insoulforge_model_request_tests EXCLUDE_FROM_ALL
        ModelRequestTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_model_request_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_model_request_tests PRIVATE
        ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_model_request_tests PRIVATE
        Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
        SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES})
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_model_request_tests PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_model_request_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
add_test(NAME model_requests COMMAND insoulforge_model_request_tests)
set_tests_properties(model_requests PROPERTIES TIMEOUT 60)

add_executable(insoulforge_onebot_reconnect_tests EXCLUDE_FROM_ALL
        OneBotReconnectTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_onebot_reconnect_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_onebot_reconnect_tests PRIVATE
        ${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_onebot_reconnect_tests PRIVATE
        Drogon::Drogon spdlog::spdlog_header_only fmt::fmt-header-only
        SQLite3_lib OpenSSL::Crypto PNG::PNG JPEG::JPEG ${GIFLIB_LIBRARY} ${LUA_LIBRARIES})
if(nlohmann_json_FOUND)
    target_link_libraries(insoulforge_onebot_reconnect_tests PRIVATE nlohmann_json::nlohmann_json)
else()
    target_include_directories(insoulforge_onebot_reconnect_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif()
add_test(NAME onebot_reconnect COMMAND insoulforge_onebot_reconnect_tests)
set_tests_properties(onebot_reconnect PROPERTIES TIMEOUT 30)

add_executable(insoulforge_image_tests EXCLUDE_FROM_ALL
        ImageTests.cpp $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_image_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_image_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
get_target_property(IMAGE_TEST_LIBRARIES insoulforge_activity_tests LINK_LIBRARIES)
target_link_libraries(insoulforge_image_tests PRIVATE ${IMAGE_TEST_LIBRARIES})
add_test(NAME image_cache COMMAND insoulforge_image_tests)

add_executable(insoulforge_favorite_emoji_tests EXCLUDE_FROM_ALL
        FavoriteEmojiTests.cpp $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_favorite_emoji_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_favorite_emoji_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_favorite_emoji_tests PRIVATE ${IMAGE_TEST_LIBRARIES})
add_test(NAME favorite_emojis COMMAND insoulforge_favorite_emoji_tests)
set_tests_properties(favorite_emojis PROPERTIES TIMEOUT 45)

# 后端对象包含 TokenCounter 的 llama API 引用。
get_property(test_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(test_target IN LISTS test_targets)
    get_target_property(test_type ${test_target} TYPE)
    get_target_property(test_sources ${test_target} SOURCES)
    if(test_type STREQUAL "EXECUTABLE" AND "${test_sources}" MATCHES "TARGET_OBJECTS:.*_backend")
        target_link_libraries(${test_target} PRIVATE llama)
    endif()
endforeach()
add_executable(insoulforge_token_counter_tests EXCLUDE_FROM_ALL TokenCounterTests.cpp
        ${PROJECT_SOURCE_DIR}/src/infrastructure/tokenizer/TokenCounter.cpp)
target_include_directories(insoulforge_token_counter_tests PRIVATE ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_token_counter_tests PRIVATE llama)
add_test(NAME token_counter COMMAND insoulforge_token_counter_tests "${llama_cpp_SOURCE_DIR}/models")
