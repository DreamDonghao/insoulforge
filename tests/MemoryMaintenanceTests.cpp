/// @file MemoryMaintenanceTests.cpp
/// @brief 使用本地模型接口验证记忆维护的成功与失败边界

#include <atomic>
#include <infrastructure/logging/Logger.hpp>
#include <iostream>
#include <stdexcept>
#include <string>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/affinity/AffinityMaintenanceService.hpp>
#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <workflow/MessageList.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/storage/Database.hpp>

namespace {
    using namespace insoulforge;

    enum class Mode { Success, ExtractFailure, ReconcileFailure, AffinityFailure, Empty, RecordLimit };

    void require(const bool condition, const char *message) {
        if (!condition) {
            throw std::runtime_error(message);
        }
    }

    auto runTests(const std::string endpoint, std::atomic<Mode> &mode, std::atomic<i32> &requests, i32 &exitCode)
      -> drogon::Task<> {
        try {
            auto &database = Database::instance();
            auto &config = Config::instance();
            require(database.initialize(":memory:").has_value(), "database initialization failed");
            config.memory.baseUrl = endpoint;
            config.memory.path = "/maintenance-fixture";
            config.memory.model = "fixture";
            config.embedding = {};
            config.memorySummaryTriggerCount = 3;
            config.memorySummaryBatchSize = 2;
            config.memorySummaryContextCount = 1;
            MemoryMaintenanceStore::apply(database, 100, {.shortTerm = "原记忆"}, {{11, 1}});

            MessageList list(100, database, config);
            for (i32 index = 0; index < 3; ++index) {
                std::ignore =
                  list.append({{"message_id", std::to_string(index)}, {"sender", {{"qq", "11"}, {"name", "Alice"}}},
                    {"segments", {{{"type", "text"}, {"text", "我喜欢编程"}}}}});
            }
            for (const auto failure: {Mode::ExtractFailure, Mode::ReconcileFailure, Mode::AffinityFailure}) {
                mode.store(failure);
                const auto batch = list.createSummaryBatch();
                require(batch.has_value(), "failed batch was not released");
                require(!list.createSummaryBatch(), "duplicate active batch was allowed");
                const i32 before = requests.load();
                const bool completed = co_await MemoryMaintenanceCoordinator::process(
                  database, 100, batch->messages, batch->contextMessages);
                require(!completed, "failed model output was accepted");
                require(requests.load() - before == (failure == Mode::ExtractFailure      ? 1
                                                      : failure == Mode::ReconcileFailure ? 2
                                                                                          : 3),
                  "failure retried automatically or advanced to another stage");
                list.finishSummaryBatch(0);
                require(list.fullSnapshot().size() == 3 && MemoryStore::getShortTermMemory(100) == "原记忆" &&
                          AffinityStore::getAffinityMap(100)[11] == 1,
                  "partial failure changed messages or committed results");
            }

            config.embedding.baseUrl = endpoint;
            config.embedding.path = "/embedding-fixture";
            config.embedding.model = "fixture";
            mode.store(Mode::Success);
            const auto embeddingBatch = list.createSummaryBatch();
            require(embeddingBatch.has_value(), "cannot reserve embedding failure batch");
            const bool embeddingCompleted = co_await MemoryMaintenanceCoordinator::process(
              database, 100, embeddingBatch->messages, embeddingBatch->contextMessages);
            list.finishSummaryBatch(embeddingCompleted ? embeddingBatch->messages.size() : 0);
            require(!embeddingCompleted && list.fullSnapshot().size() == 3 &&
                      MemoryStore::getShortTermMemory(100) == "原记忆" && AffinityStore::getAffinityMap(100)[11] == 1,
              "embedding failure committed a partial result");
            config.embedding = {};

            mode.store(Mode::Success);
            const auto batch = list.createSummaryBatch();
            require(batch.has_value(), "new message check cannot retry");
            const bool completed =
              co_await MemoryMaintenanceCoordinator::process(database, 100, batch->messages, batch->contextMessages);
            require(completed, "valid maintenance batch failed");
            list.finishSummaryBatch(batch->messages.size());
            require(list.fullSnapshot().size() == 1 && MemoryStore::getShortTermMemory(100) == "Alice 喜欢编程\n" &&
                      AffinityStore::getAffinityMap(100)[11] == 3,
              "successful batch did not commit both results or remove its prefix");

            mode.store(Mode::Empty);
            const bool emptyCompleted =
              co_await MemoryMaintenanceCoordinator::process(database, 100, batch->messages, batch->contextMessages);
            require(emptyCompleted && MemoryStore::getShortTermMemory(100) == "Alice 喜欢编程\n" &&
                      AffinityStore::getAffinityMap(100)[11] == 3,
              "empty extraction or empty affinity was treated as failure");

            mode.store(Mode::RecordLimit);
            auto manyMessages = json::array({nullptr, "invalid"});
            for (i32 index = 0; index < 305; ++index) {
                manyMessages.push_back({{"message_id", std::to_string(index)},
                  {"sender", {{"qq", "11"}, {"name", "Alice"}, {"card", "群名片"}}},
                  {"segments", {{{"type", "text"}, {"text", "引号\"与换行\n"}}}},
                  {"assets", {{"images", {{{"source", {{"url", "https://example.com/private.png"}}}}}}}},
                  {"tool_history", {{{"name", "hidden"}}}}});
            }
            const auto limited = co_await AffinityMaintenanceService::prepare(100, std::move(manyMessages));
            require(limited && limited->size() == 1, "bounded message projection failed");
            exitCode = 0;
            std::cout << "All memory maintenance tests passed\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        drogon::app().quit();
    }
} // namespace

auto main() -> int {
    std::atomic mode{Mode::Success};
    std::atomic<insoulforge::i32> requests{0};
    drogon::app().setLogLevel(trantor::Logger::kWarn).addListener("127.0.0.1", 0);
    drogon::app().registerHandler("/embedding-fixture",
      [](const drogon::HttpRequestPtr &, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          auto reply = drogon::HttpResponse::newHttpResponse();
          reply->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          reply->setBody(R"({"data":[]})");
          callback(reply);
      },
      {drogon::Post});
    drogon::app().registerHandler("/maintenance-fixture",
      [&mode, &requests](
        const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++requests;
          const auto body = insoulforge::json::parse(request->body());
          const auto prompt = body["messages"][0]["content"].get<std::string>();
          const bool extraction = prompt.find("群聊记忆提取器") != std::string::npos;
          const bool reconcile = prompt.find("群聊记忆整理器") != std::string::npos;
          const auto current = mode.load();
          if (current == Mode::RecordLimit) {
              const auto input = body["messages"][1]["content"].get<std::string>();
              const std::string prefix = "=== 群聊记录 ===\n";
              const auto end = input.find("\n\n请输出好感度变化 JSON：");
              const auto records = insoulforge::json::parse(input.substr(prefix.size(), end - prefix.size()));
              const bool valid = records.size() == 300 && records[0]["message_id"] == "0" &&
                                 records[299]["message_id"] == "299" && !records[0].contains("assets") &&
                                 !records[0].contains("tool_history") && !records[0]["sender"].contains("card") &&
                                 records[0]["segments"][0]["text"] == "引号\"与换行\n";
              if (!valid) {
                  auto reply = drogon::HttpResponse::newHttpResponse();
                  reply->setStatusCode(drogon::k400BadRequest);
                  callback(reply);
                  return;
              }
          }
          std::string output;
          if ((extraction && current == Mode::ExtractFailure) || (reconcile && current == Mode::ReconcileFailure) ||
              (!extraction && !reconcile && current == Mode::AffinityFailure)) {
              output = "invalid result";
          } else if (extraction) {
              output = current == Mode::Empty ? R"({"memories":[]})" : R"({"memories":["Alice 喜欢编程"]})";
          } else if (reconcile) {
              output = R"({"shortTerm":["Alice 喜欢编程"],"longTerm":[]})";
          } else {
              output = current == Mode::Empty ? "{}" : R"({"11":2})";
          }
          const insoulforge::json response = {{"choices", {{{"message", {{"content", output}}}}}}};
          auto reply = drogon::HttpResponse::newHttpResponse();
          reply->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          reply->setBody(response.dump());
          callback(reply);
      },
      {drogon::Post});
    auto clientThread = trantor::EventLoopThread{};
    clientThread.run();
    insoulforge::i32 exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        clientThread.getLoop()->queueInLoop([&, endpoint] {
            drogon::async_run([&, endpoint]() -> drogon::Task<> {
                co_await runTests(endpoint, mode, requests, exitCode);
                clientThread.getLoop()->quit();
            });
        });
    });
    drogon::app().run();
    clientThread.wait();
    insoulforge::Database::instance().close();
    return exitCode;
}
