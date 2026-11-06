#include <workflow/agent/llm/prompts/ExecutorSystemPrompt.hpp>
/// @file FavoriteEmojiCache.cpp
/// @brief QQ 收藏表情查询与缓存实现


#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/SessionId.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/image/Image.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>

namespace insoulforge {
    namespace {


        struct FavoriteEmojiState {
            std::mutex mutex;
            json connection;
            json cache = nullptr;
            bool dirty = true;
            u64 generation = 0;
            u64 request = 0;
        };

        auto favoriteEmojiState() -> FavoriteEmojiState & {
            static FavoriteEmojiState state;
            return state;
        }

        /// @brief 清理 CQ 码参数中不允许出现的字符
        [[nodiscard]] auto sanitizeCqParameter(std::string value) -> std::string {
            for (char &character: value) {
                if (character == ',' || character == '[' || character == ']') {
                    character = ' ';
                }
            }
            return value;
        }
    } // namespace

    auto ToolRuntime::fetchFavoriteEmojis(const std::optional<SessionId> sessionId, json connection)
      -> drogon::Task<json> {
        if (connection.is_null())
            connection = Config::instance().oneBotSnapshot();
        if (connection != Config::instance().oneBotSnapshot())
            throw std::runtime_error("OneBot 配置已切换，请刷新收藏列表后重试");
        auto &state = favoriteEmojiState();
        u64 generation, request;
        {
            std::lock_guard lock(state.mutex);
            if (state.connection == connection && !state.cache.is_null() && !state.dirty) {
                co_return state.cache;
            }
            generation = state.generation;
            request = ++state.request;
        }

        json result(json::array());
        auto data = co_await OneBotClient::fetchCustomFaceDetail(sessionId, connection);
        if (connection != Config::instance().oneBotSnapshot())
            throw std::runtime_error("OneBot 配置已切换，请刷新收藏列表后重试");
        if (!data.is_array())
            throw std::runtime_error("收藏表情获取失败");
        for (size_t index = 0; const auto &item: data) {
            if (!item.is_object())
                continue;
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
            std::lock_guard lock(state.mutex);
            if (generation == state.generation && request == state.request &&
                connection == Config::instance().oneBotSnapshot()) {
                state.connection = connection;
                state.cache = result;
                state.dirty = false;
                auto names = json::array();
                for (const auto &emoji: result)
                    names.push_back(getStr(emoji, "name"));
                ExecutorSystemPrompt::instance().updateStickerNames(std::move(names));
            }
        }
        co_return result;
    }

    auto ToolRuntime::findFavoriteEmoji(
      const std::string name, const std::optional<SessionId> sessionId, json connection) -> drogon::Task<json> {
        if (connection.is_null())
            connection = Config::instance().oneBotSnapshot();
        json match = nullptr;
        for (const json emojis = co_await fetchFavoriteEmojis(sessionId, connection); const auto &emoji: emojis) {
            if (getStr(emoji, "name") == name || getStr(emoji, "summary") == name) {
                if (!match.is_null())
                    co_return json(nullptr); // Ambiguous names must never select an arbitrary resource.
                match = emoji;
            }
        }
        co_return match;
    }

    auto ToolRuntime::addFavoriteEmoji(std::string url, const std::optional<SessionId> sessionId, json connection)
      -> drogon::Task<std::expected<void, std::string>> {
        if (connection.is_null())
            connection = Config::instance().oneBotSnapshot();
        const auto prefix = url.starts_with("https://") ? 8u : url.starts_with("http://") ? 7u : 0u;
        const auto hostEnd = url.find_first_of("/?#", prefix);
        const auto authority = url.substr(prefix, hostEnd == std::string::npos ? hostEnd : hostEnd - prefix);
        if (!prefix || authority.empty() || authority.find('@') != std::string::npos || url.size() > 4096 ||
            url.find_first_of(" \t\r\n") != std::string::npos || url.find('\0') != std::string::npos) {
            co_return std::unexpected("请输入有效的 HTTP(S) 图片直链（不含账号密码，最多4096字符）");
        }
        const auto path = co_await OneBotClient::downloadFile(url, sessionId, connection);
        if (!path || path->empty()) {
            co_return std::unexpected("下载图片失败，请确认图片直链可由 QQ 客户端所在主机访问");
        }
        // Invalidate even on failure: a timeout can occur after QQ has applied the change.
        const bool added = co_await OneBotClient::addCustomFace(*path, sessionId, connection);
        invalidateFavoriteEmojiCache();
        if (!added)
            co_return std::unexpected("收藏请求失败或结果未确认，请刷新列表核对后再试；确认 QQ 在线且支持收藏接口");
        co_return std::expected<void, std::string>{};
    }

    auto ToolRuntime::uploadFavoriteEmoji(std::string bytes, const std::optional<SessionId> sessionId, json connection)
      -> drogon::Task<std::expected<void, std::string>> {
        if (connection.is_null())
            connection = Config::instance().oneBotSnapshot();
        if (bytes.empty() || bytes.size() > kMaxFavoriteEmojiBytes)
            co_return std::unexpected("请选择非空图片，大小不得超过 8 MiB");
        const auto mime = Image::mimeType(bytes);
        if (!mime || (*mime != "image/gif" && *mime != "image/png" && *mime != "image/jpeg" && *mime != "image/webp"))
            co_return std::unexpected("仅支持有效的 GIF、PNG、JPEG 或 WebP 图片");
        const auto path = co_await OneBotClient::uploadFile(std::move(bytes), sessionId, connection);
        if (!path || path->empty())
            co_return std::unexpected("上传图片失败，请确认 QQ 在线且协议端支持 download_file.base64");
        const bool added = co_await OneBotClient::addCustomFace(*path, sessionId, connection);
        invalidateFavoriteEmojiCache();
        if (!added)
            co_return std::unexpected("收藏请求失败或结果未确认，请刷新列表核对后再试；确认 QQ 在线且支持收藏接口");
        co_return std::expected<void, std::string>{};
    }

    auto ToolRuntime::deleteFavoriteEmoji(std::string resId, const std::optional<SessionId> sessionId, json connection)
      -> drogon::Task<std::expected<void, std::string>> {
        if (connection.is_null())
            connection = Config::instance().oneBotSnapshot();
        if (resId.empty() || resId.size() > 1024 || resId.find_first_of("\r\n") != std::string::npos ||
            resId.find('\0') != std::string::npos) {
            co_return std::unexpected("请选择有效的收藏表情");
        }
        const bool deleted = co_await OneBotClient::deleteCustomFace(resId, sessionId, connection);
        invalidateFavoriteEmojiCache();
        if (!deleted)
            co_return std::unexpected("删除请求失败或结果未确认，请刷新列表核对；确认 QQ 在线且支持收藏接口");
        co_return std::expected<void, std::string>{};
    }

    void ToolRuntime::invalidateFavoriteEmojiCache() {
        auto &state = favoriteEmojiState();
        std::lock_guard lock(state.mutex);
        state.dirty = true;
        ExecutorSystemPrompt::instance().updateStickerNames(json::array());
        ++state.generation;
    }
} // namespace insoulforge
