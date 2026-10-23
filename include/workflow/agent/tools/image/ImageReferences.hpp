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
        /// @brief 按引用顺序解析当前会话原图地址，重复的消息 ID 和索引只保留一次。
        [[nodiscard]] auto resolve(const json &images, const ToolCallContext &ctx)
          -> std::expected<std::vector<std::string>, std::string>;
    }
}
