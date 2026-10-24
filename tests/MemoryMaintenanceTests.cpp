/// @file MemoryMaintenanceTests.cpp
/// @brief 本地模型接口验证联合提取、旧记忆整理和事务提交边界。

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

#include <drogon/drogon.h>
#include <trantor/net/EventLoopThread.h>

#include <infrastructure/config/Config.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <admin/http/AdminController.hpp>
#include <admin/http/PersonNameResolver.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/MessageList.hpp>
#include <workflow/agent/llm/prompts/PromptService.hpp>
#include <workflow/agent/memory/MemoryManager.hpp>
#include <workflow/agent/memory/MemoryStore.hpp>
#include <workflow/agent/memory/LongTermMemoryStore.hpp>
#include <workflow/agent/memory/PersonMemoryStore.hpp>
#include <workflow/agent/memory/affinity/AffinityStore.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceCoordinator.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceService.hpp>
#include <workflow/agent/memory/maintenance/MemoryMaintenanceStore.hpp>
#include <workflow/agent/memory/maintenance/PersonMemoryConsolidator.hpp>
#include <workflow/agent/runtime/ExecutorAgent.hpp>
#include <workflow/agent/tools/ToolRegistry.hpp>
#include <workflow/agent/tools/plugins/InfoToolsPlugin.hpp>
#include <workflow/history/ChatRecordManager.hpp>
#include <workflow/message/SessionId.hpp>
#include <workflow/message/MessageRecord.hpp>
#include <workflow/session/SessionStore.hpp>
#include <workflow/session/QQNameDirectory.hpp>

namespace {
    using namespace insoulforge;
    struct PersonFixture {
        std::atomic<i32> requests{0};
        std::atomic_bool fail{true};
        std::atomic_bool injectNew{true};
        std::atomic_bool editRelationship{false};
        std::atomic_bool omitRelationship{false};
        std::atomic_bool delay{false};
        std::atomic<PersonMemoryConsolidator *> stopService{nullptr};
        std::atomic<LlmClient::MaintenanceRequestControl *> cancelRequest{nullptr};
    } personFixture;
    std::atomic<i32> thinkingRequests{0};
    const std::string thinkingQuestion = "已知 x + 2 = 5，求 x。\n查询结果：x 为实数。";

    auto testPersonConsolidation(const std::string &endpoint) -> drogon::Task<> {
        auto &db = Database::instance();
        db.close();
        const auto path = std::filesystem::temp_directory_path() /
          ("insoulforge-person-stop-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db");
        if (!db.initialize(path.string()))
            throw std::runtime_error("person test database initialization failed");
        auto &config = Config::instance();
        config.memory.baseUrl = endpoint;
        config.memory.path = "/person-consolidation-fixture";
        MemoryMaintenanceStore::apply(db, 100, {
          .personEntries = {{22, "已有条目"}, {88001, "喜欢摄影"}, {88002, "继续保留"}, {88003, "请求失败"}},
          .affinity = {{22, 3}}});
        std::tm local{};
        local.tm_year = 126;
        local.tm_mon = 9;
        local.tm_mday = 20;
        local.tm_hour = 3;
        local.tm_isdst = -1;
        const auto today = std::mktime(&local);
        PersonMemoryConsolidator service;
        co_await service.runDue(today);
        auto person = PersonMemoryStore::getPerson(88001);
        if (personFixture.requests != 4 || person["profile"] != json::array({"整理后的人物档案"}) ||
            person["relationship"] != json::array({"相处随意"}) ||
            person["pending_entries"].size() != 1 || person["pending_entries"][0]["content"] != "请求期间新增" ||
            PersonMemoryStore::getPerson(88002)["pending_entries"].size() != 1 ||
            PersonMemoryStore::getPerson(88003)["pending_entries"].size() != 1 ||
            PersonMemoryStore::dailyState().completedDay != "2026-10-20" ||
            PersonMemoryStore::getPerson(22)["affinity"] != 3)
            throw std::runtime_error("daily consolidation lost entries, failed isolation, or changed affinity");
        co_await service.runDue(today);
        if (personFixture.requests != 4)
            throw std::runtime_error("completed daily run repeated");
        personFixture.fail = false;
        ++local.tm_mday;
        const auto tomorrow = std::mktime(&local);
        PersonMemoryConsolidator restarted;
        co_await restarted.runDue(tomorrow);
        if (personFixture.requests != 7 || !PersonMemoryStore::pendingPeople().empty())
            throw std::runtime_error("next daily run did not consume remaining entries only");
        ++local.tm_mday;
        co_await restarted.runDue(std::mktime(&local));
        if (personFixture.requests != 7)
            throw std::runtime_error("empty daily run called model");

        using Result = PersonMemoryConsolidator::Result;
        const auto checkpoint = PersonMemoryStore::dailyState();
        const auto beforeManual = personFixture.requests.load();
        if (co_await restarted.runPerson(999999) != Result::NoMemory ||
            co_await restarted.runPerson(88001) != Result::Updated ||
            personFixture.requests != beforeManual + 1 ||
            PersonMemoryStore::dailyState().completedDay != checkpoint.completedDay ||
            PersonMemoryStore::dailyState().runningDay != checkpoint.runningDay)
            throw std::runtime_error("manual consolidation cannot reorganize existing memory or changes daily checkpoint");
        personFixture.editRelationship = true;
        if (co_await restarted.runPerson(88001) != Result::Conflict ||
            PersonMemoryStore::getPerson(88001)["relationship"] != json::array({"人工修改关系"}))
            throw std::runtime_error("manual consolidation overwrote concurrent relationship edit");
        personFixture.omitRelationship = true;
        const auto beforeInvalid = PersonMemoryStore::getPerson(88001);
        if (co_await restarted.runPerson(88001) != Result::Failed ||
            PersonMemoryStore::getPerson(88001) != beforeInvalid)
            throw std::runtime_error("invalid model profile overwrote original data");
        personFixture.delay = true;
        std::atomic_bool manualDone{false};
        Result manualResult = Result::Failed;
        const auto beforeBusy = personFixture.requests.load();
        drogon::async_run([&]() -> drogon::Task<> {
            manualResult = co_await restarted.runPerson(88001);
            manualDone = true;
        });
        while (personFixture.requests == beforeBusy)
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.001);
        const auto busy = co_await restarted.runPerson(88002);
        co_await restarted.runDue(today);
        while (!manualDone)
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.001);
        personFixture.delay = false;
        if (busy != Result::Busy || manualResult != Result::Updated || personFixture.requests != beforeBusy + 1)
            throw std::runtime_error("manual and daily consolidation overlap");
        const AdminController controller;
        drogon::HttpResponsePtr apiResponse;
        const auto callback = [&](const drogon::HttpResponsePtr &response) { apiResponse = response; };
        auto request = drogon::HttpRequest::newHttpRequest();
        co_await controller.consolidatePerson(request, callback, "invalid");
        if (apiResponse->statusCode() != drogon::k400BadRequest)
            throw std::runtime_error("manual API accepts invalid QQ");
        co_await controller.consolidatePerson(request, callback, "999999");
        if (apiResponse->statusCode() != drogon::k404NotFound)
            throw std::runtime_error("manual API accepts absent person");
        MemoryMaintenanceStore::apply(db, 100, {.affinity = {{88005, 1}}});
        const auto beforeEmpty = personFixture.requests.load();
        co_await controller.consolidatePerson(request, callback, "88005");
        if (apiResponse->statusCode() != drogon::k200OK || personFixture.requests != beforeEmpty)
            throw std::runtime_error("empty manual API calls model");
        co_await controller.consolidatePerson(request, callback, "88001");
        if (apiResponse->statusCode() != drogon::k200OK || personFixture.requests != beforeEmpty + 1)
            throw std::runtime_error("manual API did not consolidate existing profile");
        personFixture.fail = true;
        const auto beforeFailure = PersonMemoryStore::getPerson(88003);
        co_await controller.consolidatePerson(request, callback, "88003");
        if (apiResponse->statusCode() != drogon::k502BadGateway ||
            PersonMemoryStore::getPerson(88003) != beforeFailure)
            throw std::runtime_error("manual API failed to preserve original data on model failure");
        personFixture.fail = false;
        MemoryMaintenanceStore::apply(db, 100, {.personEntries = {{88004, "退出期间保留"}}});
        ++local.tm_mday;
        const auto interruptedDay = std::mktime(&local);
        PersonMemoryConsolidator interrupted;
        const auto usageCount = [&db] {
            const Statement query(db.handle(), "SELECT COUNT(*) FROM llm_usage");
            return query.step() ? query.getInt(0) : -1;
        };
        const auto beforeStop = usageCount();
        if (beforeStop == 0)
            throw std::runtime_error("normal consolidation did not record usage");
        personFixture.stopService = &interrupted;
        co_await interrupted.runDue(interruptedDay);
        if (db.handle() || !db.initialize(path.string()) || usageCount() != beforeStop)
            throw std::runtime_error("late response wrote usage after service stop and database closure");
        if (PersonMemoryStore::getPerson(88004)["pending_entries"].empty() ||
            PersonMemoryStore::dailyState().runningDay != "2026-10-23")
            throw std::runtime_error("stopped service committed late result or completed unfinished run");
        PersonMemoryConsolidator resumed;
        co_await resumed.runDue(interruptedDay);
        if (!PersonMemoryStore::pendingPeople().empty() || PersonMemoryStore::dailyState().completedDay != "2026-10-23")
            throw std::runtime_error("unfinished daily run did not resume");
        const auto afterResume = usageCount();
        auto control = std::make_shared<LlmClient::MaintenanceRequestControl>();
        personFixture.cancelRequest = control.get();
        const auto cancelled = co_await LlmClient::requestMaintenance(
          json::array({{{"role", "system"}, {"content", "fixture"}},
            {{"role", "user"}, {"content", R"({"qq":"88004","profile":[],"relationship":[],"recent_interactions":[],"pending_entries":[]})"}}}), 0, "memory", std::nullopt, control);
        const auto requestCount = personFixture.requests.load();
        const auto alreadyCancelled = co_await LlmClient::requestMaintenance(
          json::array(), 0, "memory", std::nullopt, control);
        if (cancelled || alreadyCancelled || personFixture.requests != requestCount || db.handle() ||
            !db.initialize(path.string()) || usageCount() != afterResume)
            throw std::runtime_error("cancelled maintenance touched closed database or returned content");
        db.close();
        if (!db.initialize(":memory:"))
            throw std::runtime_error("could not restore test database");
        MemoryMaintenanceStore::apply(db, 100, {.affinity = {{22, 3}}});
        std::filesystem::remove(path);
    }

    struct NameFixture {
        std::atomic<i32> active{0};
        std::atomic<i32> peak{0};
        std::mutex mutex;
        std::unordered_map<u64, i32> requests;
        auto count(const u64 qq) -> i32 {
            std::lock_guard lock(mutex);
            return requests[qq];
        }
    } nameFixture;

    auto waitFor(std::function<bool()> condition) -> drogon::Task<> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline)
                throw std::runtime_error("timed out waiting for nickname fixture");
            co_await drogon::sleepCoro(drogon::app().getLoop(), 0.01);
        }
    }
    enum class Mode {
        Success,
        ExtractFailure,
        ReconcileFailure,
        OutputEmbeddingFailure,
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
        json result = {{"aliases", {{{"qq", "11"}, {"names", {"阿梨"}}}}}};
        if (legacy)
            result["legacy_entries"] = {"原记忆"};
        return result;
    }

    void require(const bool condition, const char *message) {
        if (!condition)
            throw std::runtime_error(message);
    }

    auto testPersonNames(const std::string &endpoint, std::atomic<i32> &nameRequests) -> drogon::Task<> {
        auto &config = Config::instance();
        auto &database = Database::instance();
        config.oneBotTransport = "http";
        config.qqHttpHost = endpoint;
        for (u64 qq = 91000; qq < 91012; ++qq)
            AffinityStore::adjustAffinity(qq, 1);
        const AdminController controller;
        drogon::HttpResponsePtr response;
        const auto callback = [&](const drogon::HttpResponsePtr &value) { response = value; };
        const auto started = std::chrono::steady_clock::now();
        co_await controller.getPeople(drogon::HttpRequest::newHttpRequest(), callback);
        const auto initialPeople = json::parse(response->body());
        require(!initialPeople.empty() && initialPeople[0]["name_pending"] == true &&
                  std::chrono::steady_clock::now() - started < std::chrono::milliseconds(100),
          "people list waited for slow nickname queries");
        co_await controller.getPeople(drogon::HttpRequest::newHttpRequest(), callback);
        co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "22");
        co_await waitFor([&initialPeople] {
            return std::ranges::all_of(initialPeople, [](const json &person) {
                return !PersonNameResolver::instance().lookup(std::stoull(person["qq"].get<std::string>())).pending;
            });
        });
        require(nameFixture.peak == 4 && nameFixture.count(22) == 1 &&
                  nameFixture.count(91000) == 1 && nameFixture.count(91011) == 1,
          "nickname query concurrency limit or request deduplication failed");
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
        co_await waitFor([] {
            return !PersonNameResolver::instance().lookup(991).pending &&
              !PersonNameResolver::instance().lookup(992).pending;
        });
        const auto failedQueries = nameRequests.load();
        co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "991");
        require(nameRequests.load() == failedQueries && json::parse(response->body())["name_pending"] == false,
          "failed nickname query bypassed cooldown");
        require(PersonNameResolver::instance().lookup(991,
                  PersonNameResolver::Clock::now() + std::chrono::seconds(61)).pending,
          "failed nickname query could not retry after cooldown");
        co_await waitFor([] { return !PersonNameResolver::instance().lookup(991).pending; });
        require(nameRequests.load() == failedQueries + 1, "cooldown retry did not issue one request");
        require(PersonNameResolver::instance().lookup(995).pending, "timeout test did not start");
        co_await waitFor([] { return !PersonNameResolver::instance().lookup(995).pending; });
        require(QQNameDirectory::getName(995) == "未知", "timed out nickname query updated cache");
        config.oneBotTransport = "websocket";
        MemoryMaintenanceStore::apply(database, 100, {.affinity = {{993, 1}}});
        co_await controller.getPerson(drogon::HttpRequest::newHttpRequest(), callback, "993");
        require(json::parse(response->body())["name"] == "未知", "disconnected OneBot broke person detail");
        co_await waitFor([] { return !PersonNameResolver::instance().lookup(993).pending; });
        config.oneBotTransport = "http";
        require(PersonNameResolver::instance().lookup(996).pending, "stop test did not queue nickname");
        co_await waitFor([] { return nameFixture.count(996) == 1; });
        require(PersonNameResolver::instance().lookup(997).pending, "stop test did not queue next nickname");
        PersonNameResolver::instance().stop();
        co_await waitFor([] { return nameFixture.active == 0; });
        require(QQNameDirectory::getName(996) == "未知" && QQNameDirectory::getName(997) == "未知" &&
                  !PersonNameResolver::instance().lookup(998).pending,
          "stopped resolver updated nickname cache or accepted new work");
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
                      profiles[0]["pending_entries"].size() == 1 && profiles[0]["profile"].empty() &&
                      profiles[0]["recent_interactions"].empty(),
              "success failed to atomically save independent people and preserve old reconciliation");

            mode.store(Mode::Empty);
            const auto emptyBefore = requests.load();
            require(co_await MemoryMaintenanceCoordinator::process(database, 100, original, json::array()),
              "empty results were treated as failure");
            require(requests.load() - emptyBefore == 1 && AffinityStore::getAffinityMap()[22] == 3 &&
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

            mode.store(Mode::EventOnly);
            const auto eventBefore = requests.load();
            require(co_await MemoryMaintenanceCoordinator::process(database, 100, original, json::array()),
              "empty event extraction failed");
            require(requests.load() - eventBefore == 1 &&
                      json::parse(MemoryStore::getShortTermMemory(100)) == sessionMemory(true),
              "empty extraction still generated a session event summary");
            mode.store(Mode::AliasCorrection);
            require(co_await MemoryMaintenanceCoordinator::process(database, 100, original, json::array()),
              "alias correction failed");
            require(json::parse(MemoryStore::getShortTermMemory(100))["aliases"][0]["names"] == json::array({"摄影佬"}) &&
                      MemoryStore::pendingLegacyEntries(MemoryStore::getShortTermMemory(100)) ==
                        std::vector<std::string>{"原记忆"},
              "alias correction lost deferred historical memory");

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
                  {"memories", {"stale-recalled-memory"}},
                  {"tool_history", {{{"name", "hidden"}}}}});
            }
            const auto normalProjection = MessageRecord::projectForAgent(many[2], false, true);
            require(normalProjection["sender"].contains("card") && normalProjection.contains("memories"),
              "normal reply projection unexpectedly strips memory or card");
            const auto savedConfig = SessionStore::getSessionConfig(100);
            auto withCard = savedConfig;
            withCard.sendGroupCard = true;
            SessionStore::saveSessionConfig(100, withCard);
            auto context = json::array({many[2]});
            require((co_await MemoryMaintenanceService::prepare(100, std::move(many), std::move(context))).has_value(),
              "joint extraction lost batch records or leaked media sources");
            SessionStore::saveSessionConfig(100, savedConfig);

            PromptService::initialize(database);
            auto &registry = ToolRegistry::instance();
            require(registry.registerPlugin("contract.context",
                      [](ToolRegistry &staged) {
                          staged.registerTool({.name = "contract_context",
                                                .description = "fixture",
                                                .handler = [](json, ToolCallContext ctx) -> drogon::Task<std::string> {
                                                    const auto &snapshot = ctx.messageSnapshot;
                                                    if (snapshot.size() != 14 || snapshot[0] != "legacy text" ||
                                                        snapshot[1]["segments"][0]["text"] != std::string(501, 'x') ||
                                                        !MessageRecord::findImageSource(snapshot.back(), 0)) {
                                                        throw std::runtime_error("tool lost the complete frozen snapshot");
                                                    }
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
            auto chat = json::array({"legacy text"});
            for (i32 index = 0; index < 13; ++index) {
                const json record = {{"time", "2026-10-20 12:00:00"},
                  {"sender", {{"qq", index == 0 ? "99" : (index % 2 ? "11" : "22")}}},
                  {"memories", {{{"content", "legacy recalled fact"}, {"similarity", 0.8}}}},
                  {"segments", {{{"type", "text"}, {"text", std::string(501, 'x')}},
                                 {{"type", "image"}, {"image_index", 0}}}},
                  {"assets", {{"images", {{{"recognition_status", "succeeded"}, {"description", "fixture image"},
                    {"source", {{"url", "https://example.com/private.png"}}}}}}}}};
                chat.push_back(record);
            }
            const ChatRecordManager records(privateId, std::move(chat));
            MemoryStore::updateShortTermMemory(privateId, sessionMemory().dump());
            const MemoryManager memory(privateId);
            const auto reply = co_await ExecutorAgent::execute(records, memory);
            require(
              reply && reply->content == "context verified", "executor context ordering or global injection failed");
            const ChatRecordManager groupRecords(100, records.getRecords());
            const auto groupReply = co_await ExecutorAgent::execute(groupRecords, memory,
              RouterDecision{.reason = "group strategy", .tone = "serious", .maxLength = 123});
            require(groupReply && groupReply->content == "context verified", "group Router strategy was lost");
            registry.unregisterPlugin("contract.context");
            require(registry.registerPlugin("contract.thinking", [](ToolRegistry &staged) {
                InfoToolsPlugin{}.registerTools(staged);
            }), "thinking fixture tool registration failed");
            config.executorThinking.baseUrl = endpoint;
            config.executorThinking.path = "/thinking-fixture";
            config.executorThinking.model = "fixture";
            for (const auto &args : {json::object(), json{{"question", ""}},
                     json{{"question", " \t\n"}}, json{{"question", 42}}}) {
                const auto error = co_await registry.executeTool("deep_think", args, {.sessionId = privateId});
                require(error.find("非空 question") != std::string::npos && thinkingRequests == 0,
                  "invalid thinking question reached the model");
            }
            const auto thought = co_await registry.executeTool("deep_think", {{"question", thinkingQuestion}},
              {.sessionId = privateId, .messageSnapshot = records.getRecords()});
            require(thought == "【深度思考结果】\nx = 3" && thinkingRequests == 1,
              "thinking request leaked context or lost the question");
            const auto reasoning = co_await registry.executeTool("deep_think", {{"question", "reasoning"}},
              {.sessionId = privateId});
            require(reasoning == "【深度思考结果】\nreasoning answer", "thinking reasoning fallback changed");
            for (const auto &question : {"failure", "empty"}) {
                const auto error = co_await registry.executeTool("deep_think", {{"question", question}},
                  {.sessionId = privateId});
                require(error == "深度思考暂时不可用，请直接根据已有信息回复", "thinking failure handling changed");
            }
            registry.unregisterPlugin("contract.thinking");
            auto transitional = sessionMemory();
            transitional["recent_events"] = {"旧事件"};
            transitional["legacy_entries"] = {"旧偏好一", "旧偏好二"};
            MemoryStore::updateShortTermMemory(600, transitional.dump());
            MemoryStore::updateShortTermMemory(700, "小王喜欢摄影\n某人的职业尚未关联QQ\n");
            const auto beforeLegacy = PersonMemoryStore::getForSenders(original);
            mode.store(Mode::LegacyForget);
            require(co_await MemoryMaintenanceCoordinator::process(database, 600, original, json::array()) &&
                      MemoryStore::getShortTermMemory(600) == transitional.dump(),
              "unconfigured embedding consumed historical memory");
            config.embedding.baseUrl = endpoint;
            config.embedding.path = "/embedding-fixture";
            config.embedding.model = "fixture";
            mode.store(Mode::OutputEmbeddingFailure);
            require(!(co_await MemoryMaintenanceCoordinator::process(database, 600, original, json::array())) &&
                      MemoryStore::getShortTermMemory(600) == transitional.dump() &&
                      PersonMemoryStore::getForSenders(original) == beforeLegacy,
              "failed migration consumed historical contents or committed person entries");
            mode.store(Mode::LegacyForget);
            for (const auto id: {600ULL, 700ULL}) {
                require(co_await MemoryMaintenanceCoordinator::process(database, id, original, json::array()),
                  "historical migration failed");
                require(MemoryStore::pendingLegacyEntries(MemoryStore::getShortTermMemory(id)).empty() &&
                          LongTermMemoryStore::countMemories(id) > 0,
                  "historical contents were not converted into long-term memory");
                const auto count = LongTermMemoryStore::countMemories(id);
                const auto before = requests.load();
                require(co_await MemoryMaintenanceCoordinator::process(database, id, original, json::array()) &&
                          LongTermMemoryStore::countMemories(id) == count && requests.load() - before == 1,
                  "historical migration ran more than once");
            }
            require(PersonMemoryStore::getForSenders(original) == beforeLegacy,
              "historical session migration changed global person memory");

            // Whole-set replacement: repeated recalls are deduplicated; only supplied old IDs disappear.
            MemoryMaintenanceStore::apply(database, 800, {.longTerm = {
              {.content = "11: 喜欢旧语言", .embedding = {1.0F, 0.0F}},
              {.content = "11: 使用 C++", .embedding = {1.0F, 0.0F}},
              {.content = "不相关的旧记忆", .embedding = {0.0F, 1.0F}}}});
            const auto old = LongTermMemoryStore::listMemories(800, 10, 0);
            mode.store(Mode::MemoryOnly);
            require(co_await MemoryMaintenanceCoordinator::process(database, 800, original, json::array()),
              "long-term replacement failed");
            const auto replaced = LongTermMemoryStore::listMemories(800, 10, 0);
            require(replaced.size() == 4 && std::ranges::all_of(old, [&](const auto &entry) {
                      const bool retained = std::ranges::any_of(replaced, [&](const auto &next) { return next.id == entry.id; });
                      return retained == (entry.content == "不相关的旧记忆");
                    }),
              "recalled old IDs survived or unrelated memory was removed");
            const auto snapshot = MemoryStore::getShortTermMemory(800);
            for (const auto failure: {Mode::ReconcileFailure, Mode::InvalidSessionMemory, Mode::LegacyInjection,
                   Mode::OutputEmbeddingFailure}) {
                mode.store(failure);
                require(!(co_await MemoryMaintenanceCoordinator::process(database, 800, original, json::array())) &&
                          LongTermMemoryStore::listMemories(800, 10, 0).size() == replaced.size() &&
                          MemoryStore::getShortTermMemory(800) == snapshot,
                  "failed reconciliation deleted existing memory");
            }
            mode.store(Mode::ManyShortTerm);
            require(co_await MemoryMaintenanceCoordinator::process(database, 500, original, json::array()) &&
                      LongTermMemoryStore::countMemories(500) == 20,
              "long-term entries were truncated to a former short-term limit");
            co_await testPersonConsolidation(endpoint);
            co_await testPersonNames(endpoint, nameRequests);
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
    drogon::app().registerHandler("/thinking-fixture",
      [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++thinkingRequests;
          const auto body = json::parse(request->body());
          const auto &messages = body["messages"];
          const auto question = messages.back().value("content", "");
          const bool valid = messages.size() == 2 && messages[0]["role"] == "system" &&
              messages[1]["role"] == "user" &&
              messages[0]["content"].get<std::string>().find(question) == std::string::npos &&
              (question == thinkingQuestion || question == "reasoning" || question == "failure" || question == "empty");
          auto response = drogon::HttpResponse::newHttpResponse();
          response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          if (!valid || question == "failure") {
              response->setStatusCode(drogon::k400BadRequest);
          } else {
              json message = {{"role", "assistant"}, {"content", question == "empty" ? "" : "x = 3"}};
              if (question == "reasoning") {
                  message["content"] = nullptr;
                  message["reasoning_content"] = "reasoning answer";
              }
              response->setBody(json{{"choices", {{{"message", std::move(message)}}}}}.dump());
          }
          callback(response);
      }, {drogon::Post});
    drogon::app().registerHandler("/person-consolidation-fixture",
      [](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++personFixture.requests;
          const auto body = json::parse(request->body());
          const auto profile = json::parse(body["messages"][1]["content"].get<std::string>());
          const auto qq = profile["qq"].get<std::string>();
          if (!profile.contains("relationship") || profile.contains("stable_memory"))
              throw std::runtime_error("consolidation receives obsolete person schema");
          if (qq == "88001" && personFixture.injectNew.exchange(false)) {
              MemoryMaintenanceStore::apply(Database::instance(), 200, {.personEntries = {{88001, "请求期间新增"}}});
          }
          if (personFixture.editRelationship.exchange(false)) {
              const auto original = PersonMemoryStore::getPerson(parseUInt64(qq));
              auto edited = original;
              edited["relationship"] = {"人工修改关系"};
              PersonMemoryStore::updatePerson(parseUInt64(qq), original, edited);
          }
          if (auto *service = personFixture.stopService.exchange(nullptr)) {
              service->stop();
              Database::instance().close();
          }
          if (auto *control = personFixture.cancelRequest.exchange(nullptr)) {
              control->cancel();
              Database::instance().close();
          }
          auto response = drogon::HttpResponse::newHttpResponse();
          response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          if (qq == "88003" && personFixture.fail) {
              response->setStatusCode(drogon::k400BadRequest);
              response->setBody("{}");
          } else {
              const auto content = personFixture.omitRelationship.exchange(false) ? R"({"profile":[],"recent_interactions":[]})" :
                qq == "88002" && personFixture.fail ? R"({"profile":"invalid"})" :
                R"({"profile":["整理后的人物档案"],"relationship":["相处随意"],"recent_interactions":["待跟进的近期事项"]})";
              response->setBody(json{{"choices", {{{"message", {{"content", content}}}}}},
                {"usage", {{"prompt_tokens", 10}, {"completion_tokens", 5}, {"total_tokens", 15}}}}.dump());
          }
          if (personFixture.delay)
              drogon::app().getLoop()->runAfter(0.05, [callback = std::move(callback), response] { callback(response); });
          else
              callback(response);
      }, {drogon::Post});
    drogon::app().registerHandler("/get_stranger_info",
      [&nameRequests](const drogon::HttpRequestPtr &request,
        std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          ++nameRequests;
          const auto qq = json::parse(request->body())["user_id"].get<u64>();
          {
              std::lock_guard lock(nameFixture.mutex);
              ++nameFixture.requests[qq];
          }
          const auto active = ++nameFixture.active;
          nameFixture.peak.store(std::max(nameFixture.peak.load(), active));
          auto response = drogon::HttpResponse::newHttpResponse();
          response->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          response->setBody(qq == 991 ? json{{"status", "failed"}, {"retcode", 1}}.dump() :
            json{{"status", "ok"}, {"data", {{"nickname", qq == 992 ? "" : "Fixture " + std::to_string(qq)}}}}.dump());
          drogon::app().getLoop()->runAfter(qq == 995 ? 5.5 : 0.1,
            [callback = std::move(callback), response] {
                --nameFixture.active;
                callback(response);
            });
      }, {drogon::Post});
    drogon::app().registerHandler("/embedding-fixture",
      [&mode](const drogon::HttpRequestPtr &request, std::function<void(const drogon::HttpResponsePtr &)> &&callback) {
          auto reply = drogon::HttpResponse::newHttpResponse();
          reply->setContentTypeCode(drogon::CT_APPLICATION_JSON);
          const auto input = json::parse(request->body())["input"][0].get<std::string>();
          const bool failed = mode.load() == Mode::Success || input == "prepared embedding failure";
          reply->setBody(failed ? R"({"data":[]})" : R"({"data":[{"embedding":[1.0,0.0]}]})");
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
              const std::string heading = extraction ? "=== 待提取消息 ===\n" : "=== 待总结消息 ===\n";
              const std::string contextHeading = "\n=== 补充上下文（不得提取） ===\n";
              const auto begin = input.find(heading) + heading.size();
              const auto end = input.find(contextHeading, begin);
              const auto records = json::parse(input.substr(begin, end - begin));
              const auto contextBegin = end + contextHeading.size();
              const auto contextEnd = input.find(extraction ? "\n\n请输出" : "\n=== 新提取的记忆", contextBegin);
              const auto context = json::parse(input.substr(contextBegin, contextEnd - contextBegin));
              const auto validRecord = [](const json &record) {
                  return !record.contains("assets") && !record.contains("tool_history") &&
                    !record.contains("memories") && !record["sender"].contains("card") &&
                    record["sender"]["name"] == "Alice" &&
                    record["segments"][0]["text"] == "引号\"与换行\n";
              };
              if (records.size() != 305 || records[304]["message_id"] != "304" ||
                  !std::ranges::all_of(records, validRecord) || context.size() != 1 || !validRecord(context[0]) ||
                  prompt.find(extraction ? "昵称、群名片和 @ 名称只用于识别人" : "身份元数据只用于识别人") == std::string::npos ||
                  prompt.find(extraction ? "只有聊天正文明确说明称呼时才记录" : "只有聊天正文明确表达称呼约定时才记录别称") == std::string::npos ||
                  (extraction && (prompt.find("## 信息来源") == std::string::npos ||
                    prompt.find("机器人名称：" + json(Config::instance().botName).dump()) == std::string::npos))) {
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
              const auto input = json::parse(body["messages"][1]["content"].get<std::string>());
              auto aliases = input["aliases"];
              if (current == Mode::Success)
                  aliases = sessionMemory()["aliases"];
              if (current == Mode::AliasCorrection)
                  aliases = {{{"qq", "11"}, {"names", {"摄影佬"}}}};
              auto entries = json::array();
              if (prompt.find("向量服务未配置") == std::string::npos) {
                  for (const auto *key: {"pending_entries", "legacy_entries", "recalled_memories"})
                      for (const auto &entry: input[key])
                          entries.push_back(entry);
              }
              if (current == Mode::InvalidSessionMemory)
                  aliases = "invalid";
              if (current == Mode::LegacyInjection)
                  entries = {42};
              if (current == Mode::OutputEmbeddingFailure)
                  entries = {"prepared embedding failure"};
              if (current == Mode::ManyShortTerm) {
                  entries = json::array();
                  for (int i = 0; i < 20; ++i)
                      entries.push_back("entry " + std::to_string(i));
              }
              output = json{{"aliases", std::move(aliases)}, {"longTerm", std::move(entries)}}.dump();
          } else {
              json extracted = {{"memories", json::array()}, {"affinity_changes", json::array()}};
              if (current != Mode::Empty && current != Mode::Projection && current != Mode::AffinityOnly &&
                  current != Mode::EventOnly && current != Mode::EventRetired &&
                  current != Mode::LegacyForget) {
                  extracted["memories"].push_back({{"qq", "11"}, {"entries", {"喜欢编程"}}});
              }
              if (current != Mode::Empty && current != Mode::Projection && current != Mode::MemoryOnly &&
                  current != Mode::DuplicateMemory && current != Mode::EventOnly && current != Mode::AliasCorrection &&
                  current != Mode::EventRetired && current != Mode::LegacyForget && current != Mode::LegacyInjection) {
                  extracted["affinity_changes"].push_back({{"qq", "22"}, {"delta", 2}});
              }
              if (current == Mode::AliasCorrection)
                  extracted["memories"][0]["entries"] = {"以后只叫我摄影佬"};
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
          const bool isPrivate = context["current_session"]["type"] == "private";
          std::vector<std::string> expected = {"current_session", "session_memory", "earlier_messages",
            "person_memories", "shared_conversations", "recent_messages"};
          if (!isPrivate) expected.push_back("response_requirements");
          const auto state = json::parse(messages.back()["content"].get<std::string>());
          bool valid = keys == expected && messages.back()["role"] == "system" && state.size() == 1 &&
                       state["execution_state"]["current_round"] == (isPrivate ? executorRequests : 1) &&
                       context["person_memories"].size() == 1 && context["person_memories"][0]["qq"] == "11" &&
                       context["person_memories"][0]["pending_entries"].size() == 3 &&
                       context["shared_conversations"].empty();
          valid = valid && context["session_memory"] == sessionMemory() &&
              (isPrivate ? context["current_session"]["qq"] == 22 :
                context["current_session"]["group_id"] == 100) &&
              context["earlier_messages"].size() == 2 && context["earlier_messages"][0] == "legacy text" &&
              context["earlier_messages"][1]["segments"][0]["text"] == std::string(500, 'x') + "…" &&
              context["recent_messages"].size() == 12 &&
              context["recent_messages"][0]["segments"][0]["text"] == std::string(501, 'x');
          for (const auto key : {"earlier_messages", "recent_messages"}) {
              for (const auto &record: context[key]) {
                  if (record.is_string()) continue;
                  valid = valid && !record.contains("memories") &&
                      record["recalled_memories"] == json::array({"legacy recalled fact"}) &&
                      record["time"] == "2026-10-20 12:00:00" && !record.contains("assets") &&
                      !record.dump().contains("https://example.com/private.png") &&
                      record["segments"][1]["description"] == "fixture image";
                  if (record["sender"]["qq"] == "22")
                      valid = valid && record["sender"]["affinity"] == 5;
              }
          }
          for (const auto &entry : context["person_memories"][0]["pending_entries"]) {
              valid = valid && entry.contains("created_at") && !entry.contains("time") &&
                  !entry["created_at"].get<std::string>().empty();
          }
          const auto systemPrompt = messages[0]["content"].get<std::string>();
          const auto formatBegin = systemPrompt.find("## 聊天记录格式说明");
          const auto exampleBegin = systemPrompt.find("```json\n", formatBegin) + std::string("```json\n").size();
          const auto exampleEnd = systemPrompt.find("\n```", exampleBegin);
          const auto example = json::parse(systemPrompt.substr(exampleBegin, exampleEnd - exampleBegin));
          std::vector<std::string> exampleKeys;
          for (const auto &[key, value] : example.items()) exampleKeys.push_back(key);
          valid = valid && exampleKeys == expected;
          if (isPrivate) {
              valid = valid && !context.contains("response_requirements") &&
                systemPrompt.contains("这是私聊，直接回复即可");
          } else {
              valid = valid && context["response_requirements"] == json({{"tone", "serious"},
                {"max_length", 123}, {"reply_reason", "group strategy"}}) &&
                !systemPrompt.contains("这是私聊，直接回复即可");
          }
          if (executorRequests == 1)
              frozenUser = messages[1];
          else if (isPrivate)
              valid = valid && messages[1] == frozenUser && messages.size() == 5 && messages[3]["role"] == "tool" &&
                  messages[3]["content"] == "source";
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
