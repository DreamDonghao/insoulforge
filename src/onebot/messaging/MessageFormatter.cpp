/// @file MessageFormatter.cpp
/// @brief OneBot 消息文本的 CQ 码转换

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <regex>

#include <onebot/messaging/MessageFormatter.hpp>
#include <workflow/session/QQNameDirectory.hpp>

namespace insoulforge {
    auto MessageFormatter::convertAtToCQCode(std::string text) -> std::string {
        std::string result = std::move(text);

        // 格式 @[...数字...] → 提取数字转为 [CQ:at,qq=数字]
        const std::regex atPattern(R"(@\[.*?(\d{5,11}).*?\])");
        result = std::regex_replace(result, atPattern, "[CQ:at,qq=$1]");

        // 按已知昵称查找 @目标。
        auto nameToQQ = QQNameDirectory::nameToQQMap();

        // 按昵称长度降序排序，避免短昵称先匹配
        std::vector<std::pair<std::string, u64>> sortedNames(nameToQQ.begin(), nameToQQ.end());
        std::ranges::sort(
          sortedNames, [](const auto &a, const auto &b) -> auto { return a.first.length() > b.first.length(); });

        for (const auto &[name, qq]: sortedNames) {
            const std::string mention = "@" + name;
            size_t pos = 0;
            while ((pos = result.find(mention, pos)) != std::string::npos) {
                size_t endPos = pos + mention.length();
                bool isComplete =
                  endPos >= result.length() || (!std::isalnum(static_cast<unsigned char>(result[endPos])) &&
                                                 result[endPos] != '_' && result[endPos] != '-');

                if (isComplete) {
                    // 跳过已经转换的 CQ 码。
                    if (pos >= 4 && result.substr(pos - 4, 4) == "qq=") {
                        pos = endPos;
                        continue;
                    }
                    std::string cqCode = fmt::format("[CQ:at,qq={}]", qq);
                    result.replace(pos, mention.length(), cqCode);
                    pos += cqCode.length();
                } else {
                    pos = endPos;
                }
            }
        }

        return result;
    }

} // namespace insoulforge
