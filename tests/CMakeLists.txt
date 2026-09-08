# Tests are organized as a CMake subproject to keep production targets separate.

add_executable(insoulforge_message_contract_tests EXCLUDE_FROM_ALL
        MessageContractTests.cpp
        $<TARGET_OBJECTS:${PROJECT_NAME}_backend>)
target_compile_features(insoulforge_message_contract_tests PRIVATE cxx_std_23)
target_include_directories(insoulforge_message_contract_tests PRIVATE
        ${PROJECT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(insoulforge_message_contract_tests PRIVATE
        Drogon::Drogon
        spdlog::spdlog_header_only
        fmt::fmt-header-only
        SQLite3_lib
        OpenSSL::Crypto
        PNG::PNG
        ${GIFLIB_LIBRARY})

if (nlohmann_json_FOUND)
    target_link_libraries(insoulforge_message_contract_tests PRIVATE nlohmann_json::nlohmann_json)
else ()
    target_include_directories(insoulforge_message_contract_tests PRIVATE ${NLOHMANN_JSON_INCLUDE_DIR})
endif ()

if (SQLITE3_STATIC_LIB)
    target_link_libraries(insoulforge_message_contract_tests PRIVATE ${SQLITE3_STATIC_LIB})
endif ()

add_test(NAME message_contracts COMMAND insoulforge_message_contract_tests)
