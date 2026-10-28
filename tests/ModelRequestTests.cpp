/// @file ModelRequestTests.cpp
/// @brief 模型请求控制：配置迁移、单次超时、重试边界与取消。
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

#include <drogon/drogon.h>
#include <openssl/evp.h>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/MessageRouter.hpp>
#include <workflow/agent/llm/JevClient.hpp>
#include <workflow/agent/llm/LlmClient.hpp>
#include <workflow/agent/tools/image/ImageGenerationService.hpp>
#include <workflow/message/image/ImageDescriptionService.hpp>

namespace {
    using namespace insoulforge;

    void require(bool value, const char *message) {
        if (!value) throw std::runtime_error(message);
    }

    struct Fixture {
        std::mutex mutex;
        std::unordered_map<std::string, int> attempts;
        std::unordered_map<std::string, json> bodies;
        bool consistent = true;

        auto record(const std::string &key, const json &body) -> int {
            std::lock_guard lock(mutex);
            if (const auto found = bodies.find(key); found != bodies.end())
                consistent = consistent && found->second == body;
            bodies[key] = body;
            consistent = consistent && !body.contains("timeoutSeconds") && !body.contains("maxRetries");
            return ++attempts[key];
        }
        auto count(const std::string &key) -> int {
            std::lock_guard lock(mutex);
            return attempts[key];
        }
    } fixture;

    void testConfiguration(const std::filesystem::path &path) {
        auto &config = Config::instance();
        { std::ofstream file(path); file << R"({"llm":{"memory":{}}})"; }
        require(config.initialize(path.string()).has_value(), "old configuration failed to load");
        for (const auto *role: {"router", "executor", "executorFallback", "executorThinking", "image",
               "memory", "imageGeneration", "embedding", "jev"}) {
            const std::string name(role);
            const int timeout = name == "embedding" ? 30 : name == "jev" ? 10 :
              (name == "memory" || name == "imageGeneration") ? 180 : 90;
            const int retries = name == "router" || name == "executor" || name == "executorFallback" ||
              name == "executorThinking" ? 3 : 0;
            const auto settings = config.getLLMConfig(name);
            require(settings["timeoutSeconds"] == timeout && settings["maxRetries"] == retries,
              "legacy model defaults changed");
        }
        require(config.jevWindowSize.load() == 10 && config.getMemoryConfig()["jevWindowSize"] == 10,
          "legacy Jev window did not default to ten");
        auto memorySettings = config.getMemoryConfig();
        memorySettings["jevWindowSize"] = 4;
        require(config.saveMemoryConfig(memorySettings).has_value() && config.jevWindowSize.load() == 4,
          "Jev window did not hot update");
        for (const auto &bad: json::array({0, -1, 1.5, true, "10", nullptr, 2147483648ULL})) {
            auto invalid = memorySettings;
            invalid["jevWindowSize"] = bad;
            require(!config.saveMemoryConfig(invalid) && config.getMemoryConfig() == memorySettings &&
                      config.jevWindowSize.load() == 4, "invalid Jev window changed settings");
        }
        memorySettings.erase("jevWindowSize");
        require(config.saveMemoryConfig(memorySettings).has_value() && config.jevWindowSize.load() == 4 &&
                  config.initialize(path.string()).has_value() && config.jevWindowSize.load() == 4,
          "old client or restart reset Jev window");
        const auto originalMemorySettings = config.getMemoryConfig();
        require(originalMemorySettings["conversationSummaryMaxChars"] == 100 &&
                  originalMemorySettings["conversationSummaryMaxCount"] == 10,
          "legacy summary defaults changed");
        for (const auto *key: {"conversationSummaryMaxChars", "conversationSummaryMaxCount",
               "personMemoryEntryMaxChars", "longTermMemoryEntryMaxChars", "personProfileEntryMaxChars",
               "personProfileMaxChars", "personRelationshipEntryMaxChars", "personRelationshipMaxChars",
               "personInteractionEntryMaxChars", "personInteractionMaxChars"}) {
            auto updated = config.getMemoryConfig();
            updated[key] = 7;
            require(config.saveMemoryConfig(updated).has_value(), "summary setting did not save");
            for (const auto &bad: json::array({0, -1, 1.5, true, "10", nullptr, 2147483648ULL})) {
                auto invalid = updated;
                invalid[key] = bad;
                require(!config.saveMemoryConfig(invalid) && config.getMemoryConfig() == updated,
                  "invalid summary setting changed config");
            }
            updated.erase(key);
            require(config.saveMemoryConfig(updated).has_value() && config.getMemoryConfig()[key] == 7 &&
                      config.initialize(path.string()).has_value() && config.getMemoryConfig()[key] == 7,
              "old client or restart reset summary setting");
        }
        require(config.saveMemoryConfig(originalMemorySettings).has_value(), "summary config restore failed");
        auto settings = config.getLLMConfig("memory");
        settings["timeoutSeconds"] = 600;
        settings["maxRetries"] = 2;
        require(config.saveLLMConfig("memory", settings).has_value() &&
                  config.memory.timeoutSeconds == 600 && config.memory.maxRetries == 2,
          "request settings did not hot update");
        for (const auto *key: {"timeoutSeconds", "maxRetries"}) {
            for (const auto &bad: json::array({nullptr, "3", true, 1.5, -1, 3601, 18446744073709551615ULL})) {
                auto invalid = settings;
                invalid[key] = bad;
                require(!config.saveLLMConfig("memory", invalid) && config.getLLMConfig("memory") == settings,
                  "invalid request setting modified stored configuration");
            }
        }
        auto invalid = settings;
        invalid["timeoutSeconds"] = 0;
        require(!config.saveLLMConfig("memory", invalid), "zero timeout was accepted");
        invalid = settings;
        invalid["maxRetries"] = 11;
        require(!config.saveLLMConfig("memory", invalid), "excess retries were accepted");
        for (const auto timeout: {1, 3600}) {
            auto boundary = settings;
            boundary["timeoutSeconds"] = timeout;
            boundary["maxRetries"] = timeout == 1 ? 0 : 10;
            require(config.saveLLMConfig("memory", boundary).has_value() &&
                      config.memory.timeoutSeconds == timeout && config.memory.maxRetries == (timeout == 1 ? 0 : 10),
              "valid boundary request settings were rejected");
        }
        require(config.saveLLMConfig("memory", settings).has_value(), "could not restore request settings");
        auto oldClient = settings;
        oldClient.erase("timeoutSeconds"); oldClient.erase("maxRetries");
        require(config.saveLLMConfig("memory", oldClient).has_value() && config.getLLMConfig("memory") == settings,
          "old client reset existing request settings");
        require(config.initialize(path.string()).has_value() && config.memory.timeoutSeconds == 600 &&
                  config.memory.maxRetries == 2, "request settings did not persist");
        const auto invalidPath = path.string() + ".invalid";
        { std::ofstream file(invalidPath); file << R"({"llm":{"memory":{"timeoutSeconds":"600"}}})"; }
        require(!config.initialize(invalidPath) && config.memory.timeoutSeconds == 600,
          "startup silently repaired an explicitly invalid request setting");
        std::filesystem::remove(invalidPath);
    }

    auto apiFor(const std::string &endpoint, const std::string &model, int retries = 1, int timeout = 2)
      -> LLMApiConfig {
        return {.apiKey = "fixture", .baseUrl = endpoint, .path = "/model-fixture", .model = model,
          .timeoutSeconds = timeout, .maxRetries = retries};
    }

    auto testJevWindow(const std::string &endpoint) -> drogon::Task<> {
        auto &config = Config::instance();
        config.jevWindowSize.store(10);
        for (const int size: {5, 20, 21, 22, 23}) {
            const auto key = "jev-window-" + std::to_string(size);
            config.jev = apiFor(endpoint, key, 0);
            json records = json::array();
            for (int index = 0; index < size; ++index) {
                records.push_back({{"message_id", std::to_string(index)},
                  {"sender", {{"qq", index == 12 ? "self" : "11"}}},
                  {"segments", json::array({{{"type", "text"}, {"text", "test"}}})}});
            }
            const auto decision = co_await MessageRouter::route(100, std::to_string(size - 1), records);
            require(!decision.shouldReply && fixture.count(key) == 1, "Jev decision unexpectedly fell back");
            json state;
            { std::lock_guard lock(fixture.mutex); state = fixture.bodies.at(key).at("state"); }
            const auto &window = state.at("chat_records");
            const int start = std::max(0, size - 10);
            require(window.size() == static_cast<size_t>(size - start), "wrong Jev window size");
            for (int index = start; index < size; ++index) {
                const auto &record = window.at(static_cast<size_t>(index - start));
                require(record.at("message_id") == std::to_string(index) &&
                          record.value("is_current", false) == (index == size - 1),
                  "Jev window order or trigger marker changed");
            }
            const bool spoke = start <= 12 && size > 12;
            require(state["bot_silence"]["spoke_in_window"] == spoke &&
                      state["bot_silence"]["messages_since_last_speak"] == (spoke ? size - 13 : size - start),
              "Jev silence statistics used a different window");
        }
        // 自定义窗口独立于 Router；低置信度时 Router 仍采用原有分段窗口。
        config.jevWindowSize.store(4);
        config.jev = apiFor(endpoint, "jev-window-fallback", 0);
        config.router = apiFor(endpoint, "router-window", 0);
        config.routerWindowTriggerCount = 20;
        config.routerWindowKeepCount = 10;
        json records = json::array();
        for (int index = 0; index < 25; ++index)
            records.push_back({{"message_id", std::to_string(index)}, {"sender", {{"qq", "11"}}},
              {"segments", json::array({{{"type", "text"}, {"text", "test"}}})}});
        (void) co_await MessageRouter::route(100, "24", records);
        {
            std::lock_guard lock(fixture.mutex);
            require(fixture.bodies.at("jev-window-fallback")["state"]["chat_records"].size() == 4,
              "custom Jev window was ignored");
            const auto &messages = fixture.bodies.at("router-window").at("messages");
            const auto context = json::parse(messages.back().at("content").get<std::string>());
            require(context["chat_records"].size() == 15 && context["chat_records"][0]["message_id"] == "10",
              "Router fallback stopped using its batched window");
        }
    }

    auto runTests(std::string endpoint, int &exitCode) -> drogon::Task<> {
        try {
            auto &config = Config::instance();
            require(Database::instance().initialize(":memory:").has_value(), "test database failed");
            co_await testJevWindow(endpoint);
            const json messages = json::array({{{"role", "user"}, {"content", "test"}}});
            for (const auto *key: {"chat", "fallback", "status429", "status500", "status503"}) {
                const auto response = co_await LlmClient::requestChat("test", "executor", apiFor(endpoint, key),
                  LLMModelParams{}, messages);
                require(response.has_value() && fixture.count(key) == 2, "recoverable error did not retry once");
            }
            for (const auto *key: {"status400", "status401", "status403", "malformed"}) {
                const auto response = co_await LlmClient::requestChat("test", "executor", apiFor(endpoint, key, 3),
                  LLMModelParams{}, messages);
                require(!response && fixture.count(key) == 1, "permanent or malformed response was retried");
            }
            for (const auto retries: {0, 2}) {
                const auto key = "exhaust" + std::to_string(retries);
                const auto response = co_await LlmClient::requestChat("test", "executor", apiFor(endpoint, key, retries),
                  LLMModelParams{}, messages);
                require(!response && fixture.count(key) == retries + 1, "retry limit excluded the first attempt");
            }
            const auto started = std::chrono::steady_clock::now();
            const auto timed = co_await LlmClient::requestChat("test", "executor", apiFor(endpoint, "timeout", 1, 1),
              LLMModelParams{}, messages);
            require(!timed && fixture.count("timeout") == 2 &&
                      std::chrono::steady_clock::now() - started < std::chrono::seconds{5},
              "configured timeout did not apply to each attempt");
            const auto slow = co_await LlmClient::requestChat("test", "executor", apiFor(endpoint, "slow", 0, 2),
              LLMModelParams{}, messages);
            require(slow.has_value() && fixture.count("slow") == 1, "longer timeout did not permit a slow response");

            auto frozen = apiFor(endpoint, "frozen");
            trantor::EventLoop::getEventLoopOfCurrentThread()->runAfter(0.2, [&frozen] {
                frozen.baseUrl = "http://127.0.0.1:1"; frozen.maxRetries = 0; frozen.model = "changed";
            });
            const auto frozenResponse = co_await LlmClient::requestChat("test", "executor", frozen,
              LLMModelParams{}, messages);
            require(frozenResponse.has_value() && fixture.count("frozen") == 2 && frozen.model == "changed",
              "running retries did not retain their configuration snapshot");

            config.memory = apiFor(endpoint, "memory");
            require((co_await LlmClient::requestMaintenance(messages, 0)).has_value() && fixture.count("memory") == 2,
              "maintenance ignored retry settings");
            config.embedding = apiFor(endpoint, "embedding");
            require((co_await LlmClient::requestEmbedding("test")).has_value() && fixture.count("embedding") == 2,
              "embedding ignored retry settings");
            auto jev = apiFor(endpoint, "jev");
            require((co_await JevClient::requestSystemOne(jev, json::object(), json::object())).has_value() &&
                      fixture.count("jev") == 2, "Jev ignored retry settings");
            config.image = apiFor(endpoint, "image");
            const auto image = co_await ImageDescriptionService::ask("test", {endpoint + "/pixel"}, 0);
            require(image.has_value() && fixture.count("image") == 2, "image question ignored retry settings");
            for (const auto *key: {"generation", "edit"}) {
                static_cast<LLMApiConfig &>(config.imageGeneration) = apiFor(endpoint, key);
                config.imageGeneration.apiType = "openai";
                config.imageGeneration.editPath = "/edit-fixture";
                const std::vector<std::string> references = std::string(key) == "edit" ?
                  std::vector<std::string>{"data:image/png;base64,fixture"} : std::vector<std::string>{};
                const auto result = co_await ImageGenerationService::generate("test", "1024x1024", 0, references);
                require(result.source == "https://example.com/image.png" && fixture.count(key) == 2,
                  "image generation/edit ignored retry settings");
            }
            config.memory = apiFor(endpoint, "cancel", 3);
            auto control = std::make_shared<LlmClient::MaintenanceRequestControl>();
            trantor::EventLoop::getEventLoopOfCurrentThread()->runAfter(0.2, [control] { control->cancel(); });
            const auto cancelled = co_await LlmClient::requestMaintenance(messages, 0, "memory", std::nullopt, control);
            require(!cancelled && fixture.count("cancel") == 1, "cancelled maintenance retried its request");
            { std::lock_guard lock(fixture.mutex); require(fixture.consistent, "retry body changed or local settings leaked"); }
            exitCode = 0;
            std::cout << "All model request tests passed\n";
        } catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
        drogon::app().quit();
    }
}

int main() {
    const auto path = std::filesystem::temp_directory_path() /
      ("insoulforge-model-requests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    testConfiguration(path);
    drogon::app().addListener("127.0.0.1", 0).setThreadNum(1);
    const auto handle = [](const drogon::HttpRequestPtr &request,
      std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const auto body = json::parse(request->body());
        const auto key = body["model"].get<std::string>();
        const int attempt = fixture.record(key, body);
        const auto response = drogon::HttpResponse::newHttpResponse();
        response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
        response->setBody(R"({"choices":[{"message":{"content":"answer"}}],"answers":{"answer":{}},"data":[{"embedding":[1,0],"url":"https://example.com/image.png"}]})");
        if (key.starts_with("jev-window-")) {
            response->setBody(key == "jev-window-fallback"
              ? R"({"answers":{"action":{"choice":"skip","confidence":0.1}}})"
              : R"({"answers":{"action":{"choice":"skip","confidence":1.0}}})");
            callback(response);
            return;
        }
        if (key == "router-window") {
            response->setBody(R"({"choices":[{"message":{"content":"{\"action\":\"skip\"}"}}]})");
            callback(response);
            return;
        }
        if (key == "malformed") response->setBody("invalid JSON");
        else if (key.starts_with("exhaust")) response->setStatusCode(drogon::k500InternalServerError);
        else if (key.starts_with("status")) {
            const int status = std::stoi(key.substr(6));
            if (status < 429 || attempt == 1) response->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
        } else if (key != "timeout" && key != "slow" && attempt == 1)
            response->setStatusCode(drogon::k503ServiceUnavailable);
        if (key == "edit" && request->getPath() != "/edit-fixture") response->setStatusCode(drogon::k400BadRequest);
        if (key == "timeout" || key == "slow")
            drogon::app().getLoop()->runAfter(1.3, [callback = std::move(callback), response] { callback(response); });
        else callback(response);
    };
    drogon::app().registerHandler("/model-fixture", handle, {drogon::Post});
    drogon::app().registerHandler("/edit-fixture", handle, {drogon::Post});
    drogon::app().registerHandler("/pixel", [](const drogon::HttpRequestPtr &,
      std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
        const std::string encoded = "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Y9d1QAAAABJRU5ErkJggg==";
        std::string bytes(encoded.size() / 4 * 3, '\0');
        const int size = EVP_DecodeBlock(reinterpret_cast<unsigned char *>(bytes.data()),
          reinterpret_cast<const unsigned char *>(encoded.data()), static_cast<int>(encoded.size()));
        bytes.resize(static_cast<size_t>(size) - 2);
        const auto response = drogon::HttpResponse::newHttpResponse(); response->setBody(bytes); callback(response);
    }, {drogon::Get});
    trantor::EventLoopThread client;
    client.run();
    int exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        client.getLoop()->queueInLoop([&, endpoint] {
            drogon::async_run([&, endpoint]() -> drogon::Task<> {
                co_await runTests(endpoint, exitCode); client.getLoop()->quit();
            });
        });
    });
    drogon::app().run(); client.wait(); Database::instance().close(); std::filesystem::remove(path);
    return exitCode;
}
