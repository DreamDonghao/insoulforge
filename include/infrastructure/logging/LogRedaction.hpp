/// @file LogRedaction.hpp
/// @brief 隐去管理后台启动凭据，供新日志与历史日志使用。
#pragma once

#include <string>
#include <string_view>

namespace insoulforge {
    /// @brief 启动令牌仅在服务终端展示；日志中保留提示和链接地址，不保留凭据。
    [[nodiscard]] inline auto redactAdminLogCredentials(std::string content) -> std::string {
        constexpr std::string_view label = "管理后台访问令牌";
        if (const auto start = content.find(label); start != std::string::npos) {
            content.replace(start + label.size(), std::string::npos, "：[已隐藏]");
        }
        constexpr std::string_view marker = "#token=";
        constexpr std::string_view hidden = "[已隐藏]";
        size_t cursor = 0;
        while ((cursor = content.find(marker, cursor)) != std::string::npos) {
            cursor += marker.size();
            if (content.compare(cursor, hidden.size(), hidden) == 0) {
                cursor += hidden.size();
                continue;
            }
            const auto end = content.find_first_of(" \t\r\n&\"'<>]", cursor);
            content.replace(cursor, end == std::string::npos ? end : end - cursor, hidden);
            cursor += hidden.size();
        }
        return content;
    }
} // namespace insoulforge
