/// @file MemoryMaintenanceTests.cpp
/// @brief 本地模型接口验证联合提取、旧记忆整理和事务提交边界。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/config/Config.hpp>
#include <admin/http/AdminController.hpp>
#include <infrastructure/storage/Database.hpp>
#include <workflow/MessageList.hpp>
#include <workflow/agent/llm/prompts/PromptService.hpp>
#include <workflow/agent/memory/MemoryManager.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/history/ChatRecordManager.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/session/QQNameDirectory.hpp>

namespace {
    using namespace insoulforge;
    enum class Mode {
        Success,
        ExtractFailure,
        ReconcileFailure,
        ManyShortTerm,
        InvalidSessionMemory,
        EventOnly,
        AliasCorrection,
        EventRetired,
        LegacyForget,
        LegacyInjection,
        Empty,
        AffinityOnly,
        MemoryOnly,
        InvalidQq,
        DuplicateAffinity,
        MalformedDelta,
        ContextOnlyQq,
        DuplicateMemory,
        Projection
    };

    auto sessionMemory(const bool legacy = false) -> json {
        return {{"aliases", {{{"qq", "11"}, {"names", {"阿梨"}}}}},
          {"recent_events", {"正在组织周末联机，时间尚未确定"}},
          {"legacy_entries", legacy ? json::array({"原记忆"}) : json::array()}};
    }

    void require(const bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    auto runTests(const std::string endpoint, std::atomic<Mode> &mode, std::atomic<i32> &requests,
      std::atomic<i32> &nameRequests, i32 &exitCode)
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
            MemoryMaintenanceStore::apply(database, 100, {.shortTerm = "原记忆", .affinity = {{22, 1}}});
            MessageList list(100, database, config);
            for (i32 index = 0; index < 3; ++index) {
                std::ignore = list.append({{"message_id", std::to_string(index)},
                  {"sender", {{"qq", index == 0 ? "11" : "22"}, {"name", "Alice"}}},
                  {"segments", {{{"type", "text"}, {"text", "我喜欢编程"}}}}});
            }
            const auto original = list.fullSnapshot();
            for (const auto failure: {Mode::ExtractFailure, Mode::ReconcileFailure, Mode::InvalidQq,
                   Mode::DuplicateAffinity, Mode::MalformedDelta, Mode::ContextOnlyQq, Mode::InvalidSessionMemory}) {
                mode.store(failure);
                const auto batch = list.createSummaryBatch();
                require(batch.has_value() && !list.createSummaryBatch(), "maintenance reservation failed");
                auto context = batch->contextMessages;
                context.push_back(
                  {{"sender", {{"qq", "33"}}}, {"segments", {{{"type", "text"}, {"text", "context only"}}}}});
                const auto before = requests.load();
                const bool completed =
                  co_await MemoryMaintenanceCoordinator::process(database, 100, batch->messages, std::move(context));
                require(
                  !completed && requests.load() - before ==
                                  (failure == Mode::ReconcileFailure || failure == Mode::InvalidSessionMemory ? 2 : 1),
                  "invalid extraction advanced or retried");
                list.finishSummaryBatch(0);
                require(list.fullSnapshot() == original && MemoryStore::getShortTermMemory(100) == "原记忆" &&
                          AffinityStore::getAffinityMap()[22] == 1 &&
                          PersonMemoryStore::getForSenders(original).empty(),
                  "failure committed partial memory or affinity");
            }

            config.embedding.baseUrl = endpoint;
            config.embedding.path = "/embedding-fixture";
            config.embedding.model = "fixture";
            mode.store(Mode::Success);
            const auto embeddingBatch = list.createSummaryBatch();
            const bool embeddingCompleted = co_await MemoryMaintenanceCoordinator::process(
              database, 100, embeddingBatch->messages, embeddingBatch->contextMessages);
            list.finishSummaryBatch(embeddingCompleted ? embeddingBatch->messages.size() : 0);
            require(!embeddingCompleted && list.fullSnapshot() == original &&
                      PersonMemoryStore::getForSenders(original).empty() && AffinityStore::getAffinityMap()[22] == 1,
              "embedding failure committed partial extraction");
            config.embedding = {};

            mode.store(Mode::Success);
            const auto batch = list.createSummaryBatch();
            const auto before = requests.load();
            const bool completed =
              co_await MemoryMaintenanceCoordinator::process(database, 100, batch->messages, batch->contextMessages);
            require(completed && requests.load() - before == 2, "extraction and affinity were not one shared call");
            list.finishSummaryBatch(batch->messages.size());
            const auto profiles = PersonMemoryStore::getForSenders(original);
            require(list.fullSnapshot().size() == 1 &&
                      json::parse(MemoryStore::getShortTermMemory(100)) == sessionMemory(true) &&
                      AffinityStore::getAffinityMap()[22] == 3 && profiles.size() == 1 && profiles[0]["qq"] == "11" &&
                      profiles[0]["pending_entries"].size() == 1 && profiles[0]["stable_memory"].empty() &&
                      profiles[0]["recent_interactions"].empty(),
              "success failed to atomically save independent people and preserve old reconciliation");

            mode.store(Mode::Empty);
            const auto emptyBefore = requests.load();
            require(co_await MemoryMaintenanceCoordinator::process(database, 100, original, json::array()),
              "empty results were treated as failure");
            require(requests.load() - emptyBefore == 2 && AffinityStore::getAffinityMap()[22] == 3 &&
                      PersonMemoryStore::getForSenders(original) == profiles &&
                      json::parse(MemoryStore::getShortTermMemory(100)) == sessionMemory(true),
              "empty person extraction skipped session summary or changed global state");

            const auto privateId = SessionId::fromPrivateUser(22);
            mode.store(Mode::AffinityOnly);
            require(co_await MemoryMaintenanceCoordinator::process(database, privateId, original, json::array()),
              "private affinity-only batch failed");
            require(AffinityStore::getAffinityMap()[22] == 5 && PersonMemoryStore::getForSenders(original) == profiles,
              "private score did not update globally or created unwanted memory");
            mode.store(Mode::MemoryOnly);
            require(co_await MemoryMaintenanceCoordinator::process(database, privateId, original, json::array()),
              "private memory-only batch failed");
            const auto privateProfiles = PersonMemoryStore::getForSenders(original);
            require(AffinityStore::getAffinityMap()[22] == 5 && privateProfiles[0]["pending_entries"].size() == 2 &&
                      privateProfiles[0]["pending_entries"][1]["source_session"] == std::to_string(privateId),
              "private entries were not globally shared or altered affinity");

            config.embedding.baseUrl = endpoint;
            config.embedding.path = "/embedding-fixture";
            config.embedding.model = "fixture";
            mode.store(Mode::EventOnly);
            const auto eventMessages = json::array(
              {{{"sender", {{"qq", "11"}}}, {"segments", {{{"type", "text"}, {"text", "联机改到周六晚上"}}}}}});
            require(co_await MemoryMaintenanceCoordinator::process(database, 100, eventMessages, json::array()),
              "event-only summary failed or attempted unrelated long-term work");
            require(json::parse(MemoryStore::getShortTermMemory(100))["recent_events"][0] == "联机已定周六晚上" &&
                      PersonMemoryStore::getForSenders(original) == privateProfiles &&
                      AffinityStore::getAffinityMap()[22] == 5,
              "session event did not update independently of global extraction");
            config.embedding = {};
            mode.store(Mode::AliasCorrection);
            const auto correction = json::array(
              {{{"sender", {{"qq", "11"}}}, {"segments", {{{"type", "text"}, {"text", "以后只叫我摄影佬"}}}}}});
            require(co_await MemoryMaintenanceCoordinator::process(database, 100, correction, json::array()),
              "alias correction failed");
            const auto corrected = json::parse(MemoryStore::getShortTermMemory(100));
            require(corrected["aliases"][0]["names"] == json::array({"摄影佬"}) &&
                      corrected["recent_events"] == json::array({"联机已定周六晚上"}) &&
                      PersonMemoryStore::getForSenders(original) == privateProfiles,
              "alias correction affected unrelated events or global memory");
            mode.store(Mode::EventRetired);
            require(co_await MemoryMaintenanceCoordinator::process(database, 100,
                      json::array({{{"sender", {{"qq", "11"}}},
                        {"segments", {{{"type", "text"}, {"text", "联机已经结束，没有后续安排"}}}}}}),
                      json::array()),
              "completed event summary failed");
            const auto retired = json::parse(MemoryStore::getShortTermMemory(100));
            require(retired["recent_events"].empty() && retired["aliases"] == corrected["aliases"] &&
                      PersonMemoryStore::getForSenders(original) == privateProfiles &&
                      AffinityStore::getAffinityMap()[22] == 5,
              "event retirement removed aliases or global state");

            mode.store(Mode::DuplicateMemory);
            const auto deduplicated = co_await MemoryMaintenanceService::prepare(100, original, json::array());
            require(deduplicated && deduplicated->personEntries.size() == 1, "duplicate memory was appended twice");

            mode.store(Mode::Projection);
            auto many = json::array({nullptr, "invalid"});
            for (i32 index = 0; index < 305; ++index) {
                many.push_back({{"message_id", std::to_string(index)},
                  {"sender", {{"qq", "11"}, {"name", "Alice"}, {"card", "群名片"}}},
                  {"segments", {{{"type", "text"}, {"text", "引号\"与换行\n"}}}},
                  {"assets", {{"images", {{{"source", {{"url", "https://example.com/private.png"}}}}}}}},
                  {"tool_history", {{{"name", "hidden"}}}}});
            }
            require((co_await MemoryMaintenanceService::prepare(100, std::move(many), json::array())).has_value(),
              "joint extraction lost batch records or leaked media sources");

            PromptService::initialize(database);
            auto &registry = ToolRegistry::instance();
            require(registry.registerPlugin("contract.context",
                      [](ToolRegistry &staged) {
                          staged.registerTool({.name = "contract_context",
                                                .description = "fixture",
                                                .handler = [](json, ToolCallContext) -> drogon::Task<std::string> {
                                                    co_return "source";
                                                }},
                            ToolCategory::INFORMATION);
                      }),
              "context fixture tool registration failed");
            config.executor.baseUrl = endpoint;
            config.executor.path = "/executor-fixture";
            config.executor.model = "fixture";
            config.execution.maxToolRounds.store(3);
            MemoryMaintenanceStore::apply(database, 100, {.personEntries = {{99, "older sender must not be loaded"}}});
            std::deque<json> chat;
            for (i32 index = 0; index < 13; ++index) {
                const json record = {{"sender", {{"qq", index == 0 ? "99" : (index % 2 ? "11" : "22")}}},
                  {"segments", {{{"type", "text"}, {"text", "context"}}}}};
                chat.push_back({{"role", "user"}, {"content", record.dump()}});
            }
            const ChatRecordManager records(privateId, std::move(chat));
            const MemoryManager memory(privateId);
            const auto reply = co_await ExecutorAgent::execute(records, memory, {.isPrivate = true});
            require(
              reply && reply->content == "context verified", "executor context ordering or global injection failed");
            registry.unregisterPlugin("contract.context");
            auto transitional = sessionMemory();
            transitional["legacy_entries"] = {"旧偏好一", "旧偏好二"};
            MemoryStore::updateShortTermMemory(600, transitional.dump());
            const auto beforeLegacy = PersonMemoryStore::getForSenders(original);
            mode.store(Mode::LegacyForget);
            MemoryStore::updateShortTermMemory(700, "小王喜欢摄影\n某人的职业尚未关联QQ\n");
            require(co_await MemoryMaintenanceCoordinator::process(database, 700, original, json::array()),
              "initial legacy preference conversion failed");
            require(json::parse(MemoryStore::getShortTermMemory(700))["legacy_entries"] ==
                      json::array({"小王喜欢摄影", "某人的职业尚未关联QQ"}),
              "initial conversion lost preferences or ambiguous legacy ownership");
            require(co_await MemoryMaintenanceCoordinator::process(database, 600, original, json::array()),
              "gradual legacy cleanup failed");
            const auto afterLegacy = MemoryStore::getShortTermMemory(600);
            require(json::parse(afterLegacy)["legacy_entries"] == json::array({"旧偏好二"}) &&
                      PersonMemoryStore::getForSenders(original) == beforeLegacy,
              "legacy cleanup removed unrelated legacy or global memory");
            mode.store(Mode::LegacyInjection);
            require(!(co_await MemoryMaintenanceCoordinator::process(database, 600, original, json::array())) &&
                      MemoryStore::getShortTermMemory(600) == afterLegacy &&
                      PersonMemoryStore::getForSenders(original) == beforeLegacy,
              "new information was allowed into legacy storage");
            mode.store(Mode::ManyShortTerm);
            require(co_await MemoryMaintenanceCoordinator::process(database, 500, original, json::array()),
              "large short-term memory result failed");
            const auto storedShortTerm = json::parse(MemoryStore::getShortTermMemory(500));
            require(storedShortTerm["recent_events"].size() == 20 && storedShortTerm["recent_events"][19] == "event 19",
              "short-term memory was truncated to the former 15-entry limit");
            config.oneBotTransport = "http";
            config.qqHttpHost = endpoint;
            const AdminController controller;
            drogon::HttpResponsePtr response;
            const auto callback = [&](const drogon::HttpResponsePtr &value) { response = value; };
            co_await controller.getPeople(drogon::HttpRequest::newHttpRequest(), callback);
            require(!json::parse(response->body()).empty() && nameRequests.load() > 0,
              "people list did not resolve missing nicknames");
            const auto queried = nameRequests.load();
            co_await controller.getPeople(drogon::HttpRequest::newHttpRequest(), callback);
            require(nameRequests.load() == queried, "people list repeated cached nickname queries");
            co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "22");
            require(json::parse(response->body())["name"] == "Fixture 22" && nameRequests.load() == queried,
              "person detail did not reuse resolved nickname");
            QQNameDirectory::setCustomName(22, "自定义昵称");
            co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "22");
            require(json::parse(response->body())["name"] == "自定义昵称" && nameRequests.load() == queried,
              "custom nickname was overwritten or queried unnecessarily");
            MemoryMaintenanceStore::apply(database, 100, {.affinity = {{991, 1}, {992, 2}}});
            for (const auto *qq: {"991", "992"}) {
                co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, qq);
                const auto person = json::parse(response->body());
                require(person["name"] == "未知" && person.contains("affinity") && person.contains("pending_entries"),
                  "failed or empty nickname hid person data");
            }
            const auto failedQueries = nameRequests.load();
            co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "991");
            require(nameRequests.load() == failedQueries + 1, "failed nickname query could not retry");
            config.oneBotTransport = "websocket";
            MemoryMaintenanceStore::apply(database, 100, {.affinity = {{993, 1}}});
            co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "993");
            require(json::parse(response->body())["name"] == "未知", "disconnected OneBot broke person detail");
            exitCode = 0;
            std::cout << "All memory maintenance tests passed\n";
        } catch (const std::exception &error) {
            std::cerr << error.what() << '\n';
        }
        drogon::app().quit();
    }
} // namespace

auto main() -> int {
    const auto configPath = std::filesystem::temp_directory_path() /
                            ("insoulforge-joint-memory-" +
                              std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".json");
    require(Config::instance().initialize(configPath.string()).has_value(), "test configuration initialization failed");
    std::atomic mode{Mode::Success};
    std::atomic<i32> requests{0};
    std::atomic<i32> nameRequests{0};
    i32 executorRequests = 0;
    json frozenUser;
    drogon::app().setLogLevel(trantor::Logger::kWarn).addListener("127.0.0.1", 0);
    drogon::app().registerHandler("/get_stranger_info",
      [&nameRequests](const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++nameRequests;
          const auto qq = json::parse(request->body())["user_id"].get<u64>();
          auto response = drogon::HttpResponse::newHttpResponse();
          response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          response->setBody(qq == 991 ? json{{"status", "failed"}, {"retcode", 1}}.dump() :
            json{{"status", "ok"}, {"data", {{"nickname", qq == 992 ? "" : "Fixture " + std::to_string(qq)}}}}.dump());
          callback(response);
      }, {drogon::Post});
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
          const auto body = json::parse(request->body());
          const auto prompt = body["messages"][0]["content"].get<std::string>();
          const bool extraction = prompt.find("人物记忆提取器") != std::string::npos;
          const auto current = mode.load();
          if (current == Mode::Projection && extraction) {
              const auto input = body["messages"][1]["content"].get<std::string>();
              const std::string prefix = "=== 待提取消息 ===\n";
              const auto end = input.find("\n=== 补充上下文");
              const auto records = json::parse(input.substr(prefix.size(), end - prefix.size()));
              if (!extraction || records.size() != 305 || records[304]["message_id"] != "304" ||
                  records[0].contains("assets") || records[0].contains("tool_history") ||
                  records[0]["sender"].contains("card")) {
                  auto reply = drogon::HttpResponse::newHttpResponse();
                  reply->setStatusCode(drogon::k400BadRequest);
                  callback(reply);
                  return;
              }
          }
          std::string output;
          if ((extraction && current == Mode::ExtractFailure) || (!extraction && current == Mode::ReconcileFailure)) {
              output = "invalid result";
          } else if (!extraction) {
              const auto input = body["messages"][1]["content"].get<std::string>();
              if (input.find("=== 待总结消息 ===") == std::string::npos ||
                  input.find("=== 补充上下文（不得提取） ===") == std::string::npos ||
                  prompt.find("新消息的个人喜好") == std::string::npos ||
                  (current == Mode::EventOnly && input.find("联机改到周六晚上") == std::string::npos) ||
                  (current == Mode::AliasCorrection && input.find("以后只叫我摄影佬") == std::string::npos)) {
                  output = "missing session summary inputs";
              } else if (current == Mode::InvalidSessionMemory) {
                  output = R"({"shortTerm":{"aliases":[],"recent_events":"invalid"},"longTerm":[]})";
              } else if (current == Mode::ManyShortTerm) {
                  json entries = json::array();
                  for (int index = 0; index < 20; ++index)
                      entries.push_back("event " + std::to_string(index));
                  output = prompt.find("最多") == std::string::npos
                             ? json{{"shortTerm", {{"aliases", json::array()}, {"recent_events", std::move(entries)}}},
                                 {"longTerm", json::array()}}
                                 .dump()
                             : "obsolete prompt limit";
              } else {
                  auto shortTerm = sessionMemory();
                  shortTerm.erase("legacy_entries");
                  if (current == Mode::LegacyForget)
                      shortTerm["legacy_entries"] = {"旧偏好二"};
                  if (current == Mode::LegacyInjection)
                      shortTerm["legacy_entries"] = {"新喜好"};
                  if (current == Mode::EventOnly || current == Mode::AliasCorrection) {
                      shortTerm["recent_events"] = {"联机已定周六晚上"};
                  }
                  if (current == Mode::AliasCorrection)
                      shortTerm["aliases"][0]["names"] = {"摄影佬"};
                  if (current == Mode::EventRetired) {
                      shortTerm["aliases"][0]["names"] = {"摄影佬"};
                      shortTerm["recent_events"] = json::array();
                  }
                  output = json{{"shortTerm", std::move(shortTerm)}, {"longTerm", json::array()}}.dump();
              }
          } else {
              json extracted = {{"memories", json::array()}, {"affinity_changes", json::array()}};
              if (current != Mode::Empty && current != Mode::Projection && current != Mode::AffinityOnly &&
                  current != Mode::EventOnly && current != Mode::AliasCorrection && current != Mode::EventRetired &&
                  current != Mode::LegacyForget && current != Mode::LegacyInjection) {
                  extracted["memories"].push_back({{"qq", "11"}, {"entries", {"喜欢编程"}}});
              }
              if (current != Mode::Empty && current != Mode::Projection && current != Mode::MemoryOnly &&
                  current != Mode::DuplicateMemory && current != Mode::EventOnly && current != Mode::AliasCorrection &&
                  current != Mode::EventRetired && current != Mode::LegacyForget && current != Mode::LegacyInjection) {
                  extracted["affinity_changes"].push_back({{"qq", "22"}, {"delta", 2}});
              }
              if (current == Mode::InvalidQq)
                  extracted["memories"][0]["qq"] = "self";
              if (current == Mode::ContextOnlyQq)
                  extracted["memories"][0]["qq"] = "33";
              if (current == Mode::MalformedDelta)
                  extracted["affinity_changes"][0]["delta"] = "2";
              if (current == Mode::DuplicateAffinity)
                  extracted["affinity_changes"].push_back(extracted["affinity_changes"][0]);
              if (current == Mode::DuplicateMemory)
                  extracted["memories"].push_back(extracted["memories"][0]);
              output = extracted.dump();
          }
          const json response = {{"choices", {{{"message", {{"content", output}}}}}}};
          auto reply = drogon::HttpResponse::newHttpResponse();
          reply->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          reply->setBody(response.dump());
          callback(reply);
      },
      {drogon::Post});
    drogon::app().registerHandler("/executor-fixture",
      [&executorRequests, &frozenUser](
        const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++executorRequests;
          const auto body = json::parse(request->body());
          const auto &messages = body["messages"];
          const auto context = json::parse(messages[1]["content"].get<std::string>());
          std::vector<std::string> keys;
          for (const auto &[key, value]: context.items())
              keys.push_back(key);
          const std::vector<std::string> expected = {"session", "short_term_memory", "earlier_conversation",
            "recent_conversation", "person_memories", "other_conversations", "response_requirements"};
          const auto state = json::parse(messages.back()["content"].get<std::string>());
          bool valid = keys == expected && messages.back()["role"] == "system" && state.size() == 1 &&
                       state["execution_state"]["current_round"] == executorRequests &&
                       context["person_memories"].size() == 1 && context["person_memories"][0]["qq"] == "11" &&
                       context["person_memories"][0]["pending_entries"].size() == 2 &&
                       context["other_conversations"].empty();
          valid = valid && context["short_term_memory"] == sessionMemory();
          for (const auto &record: context["recent_conversation"]) {
              if (record["sender"]["qq"] == "22")
                  valid = valid && record["sender"]["affinity"] == 5;
          }
          if (executorRequests == 1)
              frozenUser = messages[1];
          else
              valid = valid && messages[1] == frozenUser && messages.size() == 5 && messages[3]["role"] == "tool";
          auto reply = drogon::HttpResponse::newHttpResponse();
          reply->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          if (!valid) {
              reply->setStatusCode(drogon::k400BadRequest);
          } else {
              json message = {{"role", "assistant"}, {"content", "context verified"}};
              if (executorRequests == 1) {
                  message["tool_calls"] = {{{"id", "fixture-call"}, {"type", "function"},
                    {"function", {{"name", "contract_context"}, {"arguments", "{}"}}}}};
              }
              reply->setBody(json{{"choices", {{{"message", std::move(message)}}}}}.dump());
          }
          callback(reply);
      },
      {drogon::Post});
    auto clientThread = trantor::EventLoopThread{};
    clientThread.run();
    i32 exitCode = 1;
    drogon::app().registerBeginningAdvice([&] {
        const auto endpoint = "http://127.0.0.1:" + std::to_string(drogon::app().getListeners().front().toPort());
        clientThread.getLoop()->queueInLoop([&, endpoint] {
            drogon::async_run([&, endpoint]() -> drogon::Task<> {
                co_await runTests(endpoint, mode, requests, nameRequests, exitCode);
                clientThread.getLoop()->quit();
            });
        });
    });
    drogon::app().run();
    clientThread.wait();
    Database::instance().close();
    std::filesystem::remove(configPath);
    return exitCode;
}
