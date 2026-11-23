# 前端源码变化时重新构建管理后台。
find_program(NPM_EXECUTABLE NAMES npm npm.cmd REQUIRED)
set(frontend_dir "${CMAKE_CURRENT_SOURCE_DIR}/frontend")
file(GLOB_RECURSE frontend_sources CONFIGURE_DEPENDS "${frontend_dir}/src/*")
add_custom_command(
    OUTPUT "${DEPLOY_DIR}/public/index.html"
    COMMAND ${NPM_ENV_CMD} "${NPM_EXECUTABLE}" install --no-audit --no-fund
    COMMAND ${NPM_ENV_CMD} "${NPM_EXECUTABLE}" run build
    WORKING_DIRECTORY "${frontend_dir}"
    DEPENDS
        ${frontend_sources}
        "${frontend_dir}/index.html"
        "${frontend_dir}/package.json"
        "${frontend_dir}/package-lock.json"
        "${frontend_dir}/tsconfig.json"
        "${frontend_dir}/vite.config.ts"
    COMMENT "Installing deps and building frontend"
    VERBATIM
)

add_custom_target(frontend ALL DEPENDS "${DEPLOY_DIR}/public/index.html")

# 复用固定版本的 Qwen2 词表，无需模型权重。
set(TOKENIZER_RESOURCE_DIR "${DEPLOY_DIR}/resources/tokenizers")
add_custom_target(tokenizer_resources ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${TOKENIZER_RESOURCE_DIR}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${llama_cpp_SOURCE_DIR}/models/ggml-vocab-qwen2.gguf"
        "${TOKENIZER_RESOURCE_DIR}/ggml-vocab-qwen2.gguf"
    VERBATIM
)
