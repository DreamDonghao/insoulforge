/// @file FavoriteEmojiCache.cpp
/// @brief QQ 收藏表情查询与缓存实现


#include <infrastructure/NumericTypes.hpp>

#include <agent/tools/ToolRuntime.hpp>
#include <onebot/OneBotClient.hpp>

namespace insoulforge {
    namespace {
        constexpr auto kFavoriteEmojiCacheTtl = std::chrono::seconds(60);

        std::mutex favoriteEmojiCacheMutex;
        json favoriteEmojiCache(nullptr);
        std::chrono::steady_clock::time_point favoriteEmojiCacheTime{};

        /// @brief 清理 CQ 码参数中不允许出现的字符
        [[nodiscard]] std::string sanitizeCqParameter(std::string value) {
            for (char &character: value) {
                if (character == ',' || character == '[' || character == ']') {
                    character = ' ';
                }
            }
            return value;
        }
    } // namespace

    drogon::Task<json> ToolRuntime::fetchFavoriteEmojis(const std::optional<u64> sessionId) {
        {
            std::lock_guard lock(favoriteEmojiCacheMutex);
            if (!favoriteEmojiCache.is_null() &&
                std::chrono::steady_clock::now() - favoriteEmojiCacheTime < kFavoriteEmojiCacheTtl) {
                co_return favoriteEmojiCache;
            }
        }

        json result(json::array());
        const json data = co_await OneBotClient::fetchCustomFaceDetail(sessionId);
        for (size_t index = 0; const auto &item: data) {
            json emoji;
            const std::string description = sanitizeCqParameter(getStr(item, "desc"));
            const std::string md5 = getStr(item, "md5");
            const std::string fallback = md5.size() >= 6 ? "表情" + md5.substr(0, 6) : fmt::format("表情{}", index + 1);
            emoji["name"] = description.empty() ? fallback : description;
            emoji["summary"] = description;
            emoji["emoji_id"] = getStr(item, "eId");
            emoji["emoji_package_id"] = getStr(item, "epId");
            emoji["key"] = getStr(item, "key");
            emoji["url"] = getStr(item, "url");
            emoji["md5"] = md5;
            emoji["res_id"] = getStr(item, "resId");
            emoji["is_mark_face"] = getBool(item, "isMarkFace");
            result.push_back(std::move(emoji));
            ++index;
        }

        {
            std::lock_guard lock(favoriteEmojiCacheMutex);
            favoriteEmojiCache = result;
            favoriteEmojiCacheTime = std::chrono::steady_clock::now();
        }
        co_return result;
    }

    drogon::Task<json> ToolRuntime::findFavoriteEmoji(std::string name, const std::optional<u64> sessionId) {
        for (const json emojis = co_await fetchFavoriteEmojis(sessionId); const auto &emoji: emojis) {
            if (getStr(emoji, "name") == name || getStr(emoji, "summary") == name) {
                co_return emoji;
            }
        }
        co_return json(nullptr);
    }

    void ToolRuntime::invalidateFavoriteEmojiCache() {
        std::lock_guard lock(favoriteEmojiCacheMutex);
        favoriteEmojiCache = json(nullptr);
    }
} // namespace insoulforge
