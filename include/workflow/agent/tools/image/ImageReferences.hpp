/// @file ImageReferences.hpp
/// @brief 工具共用的当前会话图片引用解析
#pragma once

#include <expected>
#include <string>
#include <vector>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge {
    struct ToolCallContext;
    namespace ImageReferences {
        [[nodiscard]] auto schema() -> json;
        /// @brief 按引用顺序解析可见图片 ID 或旧消息/索引；返回缓存 ID 或回退 URL，去重。
        [[nodiscard]] auto resolve(const json &images, const ToolCallContext &ctx)
          -> std::expected<std::vector<std::string>, std::string>;
    }
}
