/// @brief Admin authentication, real OneBot requests and favorite cache invalidation.
#include <iostream>
#include <stdexcept>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <admin/access/AdminAccessToken.hpp>
#include <admin/http/AdminController.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/http/HttpTrace.hpp>
#include <infrastructure/image/Image.hpp>
#include <infrastructure/logging/LogBuffer.hpp>
#include <onebot/messaging/MessageFormatter.hpp>
#include <onebot/transport/OneBotClient.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/ToolRuntime.hpp>
#include <workflow/agent/tools/plugins/ActionToolsPlugin.hpp>
#include <workflow/agent/tools/plugins/InfoToolsPlugin.hpp>

using namespace insoulforge;
namespace {
    json favorites = json::array({json{{"resId", "old"}, {"desc", "same"}, {"md5", "abc"}}});
    const std::string gif = drogon::utils::base64Decode("R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==");
    int uploads = 0;
    bool snowMode = false;
    bool switchDuringDownload = false;
    bool switchDuringList = false;
    int savedSequence = 0;
    int snowRenames = 0;
    int napRenames = 0;
    bool failUpload = false;
    bool failRename = false;
    bool malformedList = false;
    int downloads = 0, adds = 0, deletes = 0, lists = 0;
    bool failDownload = false, failAdd = false, failDelete = false;
    void require(bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }
    auto request(const drogon::HttpClientPtr &client, const std::string &path, json body, bool authenticated = true)
      -> drogon::Task<drogon::HttpResponsePtr> {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath(path);
        req->setMethod(drogon::Post);
        req->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        req->setBody(body.dump());
        if (authenticated)
            req->addCookie("insoulforge_admin_session", AdminAccessToken::token());
        co_return co_await client->sendRequestCoro(req);
    }
    auto upload(const drogon::HttpClientPtr &client, std::string bytes, bool authenticated = true,
      std::string field = "image", bool malformed = false) -> drogon::Task<drogon::HttpResponsePtr> {
        auto req = drogon::HttpRequest::newHttpRequest();
        req->setPath("/admin/api/emoji/add");
        req->setMethod(drogon::Post);
        req->setContentTypeString("multipart/form-data; boundary=emoji-test-boundary");
        req->setBody(malformed ? "broken multipart"
                               : "--emoji-test-boundary\r\nContent-Disposition: form-data; name=\"" + field +
                                   "\"; filename=\"misleading.txt\"\r\nContent-Type: text/plain\r\n\r\n" + bytes +
                                   "\r\n--emoji-test-boundary--\r\n");
        if (authenticated)
            req->addCookie("insoulforge_admin_session", AdminAccessToken::token());
        co_return co_await client->sendRequestCoro(req);
    }
    auto runTests(const std::string host, int &exitCode) -> drogon::Task<> {
        try {
            auto client = drogon::HttpClient::newHttpClient(host);
            for (const auto &route: {"/admin/api/emoji/add", "/admin/api/emoji/delete"}) {
                auto resp = co_await request(client, route, json::object(), false);
                require(resp->statusCode() == drogon::k401Unauthorized, "mutation requires admin authentication");
            }
            require(downloads == 0 && deletes == 0, "unauthorized requests do not reach QQ");
            for (const auto &url:
              {"file:///etc/passwd", "base64://abc", "https://", "https://u:p@host/a", "https://a/b\nc"}) {
                auto resp = co_await request(client, "/admin/api/emoji/add", {{"url", url}});
                require(!json::parse(resp->body()).value("success", true), "invalid URL rejected");
            }
            for (const json body: {json{{"url", 1}}, json::object(), json::array()}) {
                auto resp = co_await request(client, "/admin/api/emoji/add", body);
                require(!json::parse(resp->body()).value("success", true), "invalid add body rejected");
            }
            require(downloads == 0, "validation precedes download");
            auto before = co_await ToolRuntime::fetchFavoriteEmojis();
            require(before.size() == 1 && lists == 1, "initial favorites fetched");
            co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == 1, "list uses cache");
            auto added = co_await request(client, "/admin/api/emoji/add", {{"url", "https://example.com/image.gif"}});
            require(json::parse(added->body()).value("success", false), "add succeeds");
            require(downloads == 1 && adds == 1, "add downloads and saves once");
            auto after = co_await ToolRuntime::fetchFavoriteEmojis();
            require(after.size() == 2 && lists == 2, "add invalidates cached list");
            auto removed = co_await request(client, "/admin/api/emoji/delete", {{"res_id", "new"}});
            require(json::parse(removed->body()).value("success", false), "delete succeeds");
            after = co_await ToolRuntime::fetchFavoriteEmojis();
            require(after.size() == 1 && after[0]["res_id"] == "old" && lists == 3,
              "delete uses resource ID despite identical names and invalidates cache");
            for (const json body: {json{{"res_id", ""}}, json{{"res_id", 5}}, json::object()}) {
                auto resp = co_await request(client, "/admin/api/emoji/delete", body);
                require(!json::parse(resp->body()).value("success", true), "invalid delete body rejected");
            }
            require(deletes == 1, "invalid deletes never reach QQ");
            failDownload = true;
            auto failed = co_await request(client, "/admin/api/emoji/add", {{"url", "https://example.com/image.gif"}});
            require(!json::parse(failed->body()).value("success", true) && adds == 1, "download failure prevents save");
            failDownload = false;
            failAdd = true;
            failed = co_await request(client, "/admin/api/emoji/add", {{"url", "https://example.com/image.gif"}});
            require(!json::parse(failed->body()).value("success", true), "QQ save failure reported");
            co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == 4, "uncertain add invalidates cache for reconciliation");
            failDelete = true;
            failed = co_await request(client, "/admin/api/emoji/delete", {{"res_id", "old"}});
            require(!json::parse(failed->body()).value("success", true), "QQ delete failure reported");
            after = co_await ToolRuntime::fetchFavoriteEmojis();
            require(after.size() == 1 && lists == 5, "failed delete preserves favorite and clears stale cache");
            const auto originalAdds = adds;
            auto uploaded = co_await upload(client, gif, false);
            require(uploaded->statusCode() == drogon::k401Unauthorized, "upload requires authentication");
            for (const auto &bytes: {std::string{}, std::string("not an image"), std::string("GIF89a"),
                   std::string(ToolRuntime::kMaxFavoriteEmojiBytes + 1, 'x'),
                   std::string(ToolRuntime::kMaxFavoriteEmojiBytes + 1024 * 1024 + 1, 'x')}) {
                uploaded = co_await upload(client, bytes);
                require(!json::parse(uploaded->body()).value("success", true), "invalid or oversized upload rejected");
            }
            uploaded = co_await upload(client, gif, true, "wrong");
            require(uploaded->statusCode() == drogon::k400BadRequest, "wrong file field rejected");
            uploaded = co_await upload(client, gif, true, "image", true);
            require(uploaded->statusCode() == drogon::k400BadRequest, "malformed multipart rejected");
            require(uploads == 0 && adds == originalAdds, "invalid uploads never call OneBot");
            failAdd = false;
            uploaded = co_await upload(client, gif);
            require(json::parse(uploaded->body()).value("success", false),
              "GIF bytes accepted despite filename and declared MIME");
            require(uploads == 1 && adds == originalAdds + 1, "upload transfers bytes and then saves remote path");
            const auto previousLists = lists;
            co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == previousLists + 1, "upload invalidates list cache");
            failUpload = true;
            uploaded = co_await upload(client, gif);
            require(!json::parse(uploaded->body()).value("success", true) && adds == originalAdds + 1,
              "upload failure prevents add");
            failUpload = false;
            failAdd = true;
            uploaded = co_await upload(client, gif);
            require(!json::parse(uploaded->body()).value("success", true), "uploaded image save failure reported");
            co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == previousLists + 2, "uncertain uploaded save invalidates cache");
            co_await OneBotClient::addCustomFace("base64://PRIVATE_IMAGE_PAYLOAD");
            for (const auto &entry: HttpTrace::instance().query(0, 50)) {
                require(
                  entry.requestBody.find(Image::encode(gif)) == std::string::npos, "upload trace redacts raw Base64");
                require(entry.requestBody.find("PRIVATE_IMAGE_PAYLOAD") == std::string::npos,
                  "save trace redacts Base64 URI");
            }
            for (const auto &entry: LogBuffer::instance().query({}).entries) {
                require(entry.content.find(Image::encode(gif)) == std::string::npos &&
                          entry.content.find("PRIVATE_IMAGE_PAYLOAD") == std::string::npos,
                  "error logs redact image payloads");
            }
            auto &registry = ToolRegistry::instance();
            require(registry.registerPlugin(
                      "builtin.info", [](ToolRegistry &staged) { InfoToolsPlugin{}.registerTools(staged); }),
              "info plugin registration succeeds");
            require(registry.registerPlugin("builtin.action",
                      [](ToolRegistry &staged) { ActionToolsPlugin{"fixture"}.registerTools(staged); }),
              "action plugin registration succeeds");
            require(!registry.hasTool("list_stickers"), "list_stickers is not registered");
            for (const auto &tool: registry.getTools({}))
                require(tool["function"]["name"] != "list_stickers", "list_stickers is not advertised");
            require(registry.hasTool("send_sticker"), "name-based sticker sender remains registered");
            const auto ambiguous = co_await registry.executeTool(
              "rename_sticker", {{"name", "same"}, {"new_name", "unsafe"}}, {.sessionId = SessionId::group("100")});
            require(ambiguous.find("名称不唯一") != std::string::npos, "ambiguous rename is rejected");
            favorites = json::array({json{{"resId", "old"}, {"desc", "same"}, {"md5", "abc"}}});
            ToolRuntime::invalidateFavoriteEmojiCache();
            co_await ToolRuntime::fetchFavoriteEmojis();
            const auto beforeRenameLists = lists;
            auto renamed = co_await registry.executeTool(
              "rename_sticker", {{"name", "same"}, {"new_name", "updated"}}, {.sessionId = SessionId::group("100")});
            require(renamed.find("已改名") != std::string::npos, "rename succeeds");
            auto refreshed = co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == beforeRenameLists + 1 && refreshed[0]["name"] == "updated",
              "rename invalidates cache and exposes updated name");
            failRename = true;
            renamed = co_await registry.executeTool("rename_sticker", {{"name", "updated"}, {"new_name", "uncertain"}},
              {.sessionId = SessionId::group("100")});
            require(renamed.find("结果未确认") != std::string::npos, "failed rename reported");
            refreshed = co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == beforeRenameLists + 2 && refreshed[0]["name"] == "updated",
              "uncertain rename invalidates cache for reconciliation");
            malformedList = true;
            ToolRuntime::invalidateFavoriteEmojiCache();
            bool rejected = false;
            try {
                co_await ToolRuntime::fetchFavoriteEmojis();
            } catch (const std::exception &) {
                rejected = true;
            }
            require(rejected, "malformed list response must report failure");
            malformedList = false;
            require(!(co_await ToolRuntime::fetchFavoriteEmojis()).empty(), "failed refresh can be retried");
            failRename = false;
            failDelete = false;
            failAdd = false;
            snowMode = true;
            const auto oldConnection = Config::instance().oneBotSnapshot();
            Config::instance().oneBotImplementation = "snowluma";
            favorites = json::array({json{{"emoji_id", "snow-1"}, {"resId", "snow-1"}, {"desc", "snow"},
              {"url", "https://example.com/image.gif"}, {"md5", "abc"}}});
            auto market = json{{"is_mark_face", true}, {"emoji_id", "market-id"}, {"emoji_package_id", "123"},
              {"key", "market-key"}, {"summary", "market"}, {"url", "https://example.com/image.gif"}};
            auto formatted = MessageFormatter::favoriteEmojiToCQCode(market);
            require(formatted && formatted->starts_with("[CQ:mface,"), "complete market fields retain native mface");
            market.erase("key");
            formatted = MessageFormatter::favoriteEmojiToCQCode(market);
            require(formatted && formatted->starts_with("[CQ:image,"), "incomplete market fields fall back to URL");
            market.erase("url");
            require(!MessageFormatter::favoriteEmojiToCQCode(market), "missing sending fields report failure");
            const auto listsBeforeSwitch = lists;
            auto snowFavorites = co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == listsBeforeSwitch + 1 && snowFavorites[0]["emoji_id"] == "snow-1" &&
                      snowFavorites[0]["res_id"] == "snow-1",
              "provider switch refreshes and maps SnowLuma IDs");
            require(!getBool(snowFavorites[0], "is_mark_face"), "missing market fields do not fabricate mface");
            const auto napBefore = napRenames;
            renamed = co_await registry.executeTool("rename_sticker", {{"name", "snow"}, {"new_name", "snow renamed"}},
              {.sessionId = SessionId::group("100")});
            require(renamed.find("已改名") != std::string::npos && snowRenames == 1 && napRenames == napBefore,
              "SnowLuma rename uses modify_custom_face only");
            auto adminRename = co_await request(client, "/admin/api/emoji/desc",
              {{"emoji_id", "snow-1"}, {"res_id", "snow-1"}, {"md5", "abc"}, {"desc", "admin snow"}});
            require(json::parse(adminRename->body()).value("success", false), "SnowLuma admin rename succeeds");
            require(co_await OneBotClient::setCustomFaceDesc("snow-1", "snow-1", "abc", "snow final"),
              "SnowLuma adapter accepts unified rename arguments");
            added = co_await request(client, "/admin/api/emoji/add", {{"url", "https://example.com/image.gif"}});
            require(json::parse(added->body()).value("success", false), "SnowLuma URL add succeeds");
            uploaded = co_await upload(client, gif);
            require(json::parse(uploaded->body()).value("success", false), "SnowLuma upload succeeds");
            removed = co_await request(client, "/admin/api/emoji/delete", {{"res_id", "snow-1"}});
            require(
              json::parse(removed->body()).value("success", false), "SnowLuma delete maps resource ID to emoji_id");
            const auto snapshot = json::array({json{{"message_id", "123"}, {"sender", {{"user_id", "42"}}},
              {"assets", {{"images", json::array({json{{"source",
                                       {{"file", "cached-image"}, {"url", "https://example.com/image.gif"}}}}})}}}}});
            const auto saveArgs = json{{"message_id", "123"}, {"image_index", 0}, {"name", "saved snow"}};
            auto savedSticker = co_await registry.executeTool(
              "save_sticker", saveArgs, {.sessionId = SessionId::group("100"), .messageSnapshot = snapshot});
            require(savedSticker.find("名称: saved snow") != std::string::npos, "SnowLuma save sets requested name");
            failRename = true;
            savedSticker = co_await registry.executeTool(
              "save_sticker", saveArgs, {.sessionId = SessionId::group("100"), .messageSnapshot = snapshot});
            require(savedSticker.find("未确认名称设置成功") != std::string::npos &&
                      savedSticker.find("名称: saved snow") == std::string::npos,
              "save reports partial success on rename failure");
            failRename = false;
            const auto deletesBeforeStale = deletes;
            require(!co_await OneBotClient::deleteCustomFace("old", std::nullopt, oldConnection) &&
                      deletes == deletesBeforeStale,
              "stale operation cannot write after implementation switch");
            const auto addsBeforeSwitch = adds;
            switchDuringDownload = true;
            added = co_await request(client, "/admin/api/emoji/add", {{"url", "https://example.com/image.gif"}});
            require(!json::parse(added->body()).value("success", true) && adds == addsBeforeSwitch,
              "switch during download prevents subsequent add on new provider");
            Config::instance().oneBotImplementation = "snowluma";
            switchDuringList = true;
            ToolRuntime::invalidateFavoriteEmojiCache();
            rejected = false;
            try {
                co_await ToolRuntime::fetchFavoriteEmojis();
            } catch (const std::exception &) {
                rejected = true;
            }
            require(rejected, "old response after switch is rejected");
            const auto listsAfterStale = lists;
            co_await ToolRuntime::fetchFavoriteEmojis();
            require(lists == listsAfterStale + 1, "old response cannot populate current cache");
            std::cout << "Favorite emoji tests passed\n";
            exitCode = 0;
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        drogon::app().quit();
    }
} // namespace
int main() {
    if (!AdminAccessToken::initialize())
        return 1;
    drogon::app().registerPreRoutingAdvice(AdminAccessToken::checkRequestAccess);
    drogon::app().registerHandler("/get_image",
      [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          callback(jsonResponse({{"status", "ok"}, {"data", {{"file", "/napcat/image.gif"}}}}));
      },
      {drogon::Post});
    drogon::app().registerHandler("/download_file",
      [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          const auto body = json::parse(req->body());
          if (body.contains("base64")) {
              ++uploads;
              require(body["base64"] == Image::encode(gif), "upload contains exact binary Base64");
              require(!body.contains("url"), "upload uses documented base64 field");
              callback(
                jsonResponse({{"status", failUpload ? "failed" : "ok"}, {"data", {{"file", "/napcat/image.gif"}}}}));
              return;
          }
          if (switchDuringDownload) {
              switchDuringDownload = false;
              Config::instance().oneBotImplementation = "napcat";
          }
          ++downloads;
          require(body["url"] == "https://example.com/image.gif", "download receives original URL");
          callback(
            jsonResponse({{"status", failDownload ? "failed" : "ok"}, {"data", {{"file", "/napcat/image.gif"}}}}));
      },
      {drogon::Post});
    drogon::app().registerHandler("/add_custom_face",
      [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++adds;
          require(json::parse(req->body())["file"] == (snowMode ? "file:///napcat/image.gif" : "/napcat/image.gif") ||
                    (failAdd && json::parse(req->body())["file"] == "base64://PRIVATE_IMAGE_PAYLOAD"),
            "save receives NapCat local path");
          if (!failAdd) {
              if (snowMode) {
                  const auto id = "snow-new-" + std::to_string(++savedSequence);
                  favorites.push_back(
                    {{"emoji_id", id}, {"resId", id}, {"desc", "same"}, {"url", "https://example.com/image.gif"}});
              } else
                  favorites.push_back({{"resId", "new"}, {"desc", "same"}});
          }
          callback(jsonResponse({{"status", failAdd ? "failed" : "ok"}}));
      },
      {drogon::Post});
    drogon::app().registerHandler("/delete_custom_face",
      [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++deletes;
          const auto body = json::parse(req->body());
          require(!snowMode || !body.contains("res_id"), "SnowLuma delete must not send res_id");
          const auto id = body.at(snowMode ? "emoji_id" : "res_id");
          if (!failDelete) {
              for (auto it = favorites.begin(); it != favorites.end(); ++it) {
                  if ((*it)["resId"] == id) {
                      favorites.erase(it);
                      break;
                  }
              }
          }
          callback(jsonResponse({{"status", failDelete ? "failed" : "ok"}}));
      },
      {drogon::Post});
    drogon::app().registerHandler("/set_custom_face_desc",
      [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++napRenames;
          require(!snowMode, "SnowLuma never uses NapCat rename action");
          const auto body = json::parse(req->body());
          if (!failRename) {
              for (auto &item: favorites) {
                  if (item["resId"] == body["res_id"])
                      item["desc"] = body["desc"];
              }
          }
          callback(jsonResponse({{"status", failRename ? "failed" : "ok"}}));
      },
      {drogon::Post});
    drogon::app().registerHandler("/modify_custom_face",
      [](const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++snowRenames;
          const auto body = json::parse(req->body());
          require(
            body.size() == 2 && body.contains("emoji_id") && body.contains("desc"), "SnowLuma rename wire contract");
          if (!failRename)
              for (auto &item: favorites)
                  if (getStr(item, "emoji_id") == getStr(body, "emoji_id"))
                      item["desc"] = body["desc"];
          callback(jsonResponse({{"status", failRename ? "failed" : "ok"}}));
      },
      {drogon::Post});
    drogon::app().registerHandler("/fetch_custom_face_detail",
      [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++lists;
          if (switchDuringList) {
              switchDuringList = false;
              Config::instance().oneBotImplementation = "napcat";
          }
          callback(
            jsonResponse({{"status", "ok"}, {"data", malformedList ? json{{"error", "unavailable"}} : favorites}}));
      },
      {drogon::Post});
    drogon::app()
      .setClientMaxBodySize(25 * 1024 * 1024)
      .setClientMaxMemoryBodySize(25 * 1024 * 1024)
      .addListener("127.0.0.1", 0)
      .setThreadNum(1);
    trantor::EventLoopThread worker;
    worker.run();
    int exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto host = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        Config::instance().oneBotTransport = "http";
        Config::instance().qqHttpHost = host;
        worker.getLoop()->queueInLoop([&, host] {
            drogon::async_run([&, host]() -> drogon::Task<> {
                co_await runTests(host, exitCode);
                worker.getLoop()->quit();
            });
        });
    });
    drogon::app().run();
    worker.wait();
    return exitCode;
}
