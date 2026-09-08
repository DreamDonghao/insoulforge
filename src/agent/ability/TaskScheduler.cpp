/// @file TaskScheduler.cpp
/// @brief 定时任务调度器 - 实现
/// @author donghao
/// @date 2026-08-27

#include <include/agent/ability/TaskScheduler.hpp>
#include <infrastructure/config/Config.hpp>
#include <drogon/drogon.h>
#include <iomanip>
#include <conversation/message/SessionId.hpp>
#include <spdlog/spdlog.h>
#include <sstream>
#include <agent/ability/TaskStore.hpp>
#include <infrastructure/CommonUtil.hpp>
#include <infrastructure/http/HttpUtil.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {
        /// @brief 触发提前量：提前这么多秒注入消息，补偿 Router+Executor 的回复生成耗时。
        /// 不宜过大，否则提醒会比用户指定时刻明显提前
        constexpr std::chrono::seconds kFireLead{5};

        /// @brief 注入事件使用的本机接收接口地址（与 main.cpp 监听端口一致）
        constexpr auto kSelfBaseUrl = "http://127.0.0.1:7778";

        /// @brief 合成系统消息的发送者昵称与正文前缀
        constexpr std::string_view kSystemTaskLabel = "系统定时任务";

        /// @brief 计算每日任务的下次触发时刻：自上次时刻起逐日推进到严格晚于当前
        /// （mktime 归一化跨月/跨年，tm_isdst=-1 交给系统处理夏令时偏移）
        std::time_t nextDailyFire(const std::time_t lastTime) {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_r(&lastTime, &tm);
            std::time_t next{};
            do {
                tm.tm_mday += 1;
                tm.tm_isdst = -1;
                next = mktime(&tm);
            } while (next <= now);
            return next;
        }

        std::string buildText(const TaskStore::ScheduledTask &task, const bool delayed) {
            // 正文必须是对机器人下达的指令而非对用户的陈述，
            // content 是备忘而非现成回复，具体怎么说由到点时的 AI 结合上下文自行决定
            const std::string when = task.isDaily ? fmt::format("你设定的每日 {}", formatTimeOfDay(task.remindTime))
                                                  : fmt::format("你在 {} 设定的", formatUnixTime(task.remindTime));
            std::string text = fmt::format("【{}】{}，{}定时任务到点了。你留下的备忘：「{}」。"
                                           "请结合会话上下文自行决定如何完成这件事并作出回应",
              kSystemTaskLabel, Config::instance().botName, when, task.content);
            if (delayed) {
                text += "（已超过原定时刻送达，因程序当时未运行）";
            }
            return text;
        }

        json buildSystemEvent(const TaskStore::ScheduledTask &task, const bool delayed) {
            const auto &config = Config::instance();
            const std::string text = buildText(task, delayed);

            // 合成消息 ID 用远离真实 ID 的固定区段，避免与 NapCat 分配的冲突
            static std::atomic<int64_t> s_syntheticMsgId{0};
            const auto msgId = 9000000000LL + s_syntheticMsgId.fetch_add(1);

            json body;
            body["post_type"] = "message";
            body["self_id"] = config.selfQQNumber;
            body["time"] = std::time(nullptr);
            body["message_id"] = fmt::to_string(msgId);
            body["raw_message"] = text;
            body["sender"]["user_id"] = SessionId::kSystemAccountId;
            body["sender"]["nickname"] = std::string(kSystemTaskLabel);
            if (task.sessionType == "private") {
                body["message_type"] = "private";
                body["user_id"] = task.targetId;
            } else {
                body["message_type"] = "group";
                body["group_id"] = task.targetId;
            }
            json item;
            item["type"] = "text";
            item["data"]["text"] = text;
            body["message"].push_back(item);
            return body;
        }
    } // namespace

    TaskScheduler &TaskScheduler::instance() {
        static TaskScheduler scheduler;
        return scheduler;
    }

    TaskScheduler::~TaskScheduler() { stop(); }

    void TaskScheduler::start() {
        if (bool expected = false; !m_running.compare_exchange_strong(expected, true)) {
            return;
        }

        restorePendingTasks();
        m_thread = std::jthread([this] { runLoop(); });
    }

    void TaskScheduler::stop() {
        {
            std::lock_guard lock(m_mutex);
            if (!m_running.exchange(false)) {
                return;
            }
        }
        m_cv.notify_all();
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    int64_t TaskScheduler::schedule(TaskStore::ScheduledTask task) {
        const int64_t id = TaskStore::addScheduledTask(task);

        Entry entry;
        entry.task = std::move(task);
        entry.task.id = id;
        entry.fireTime = entry.task.remindTime - kFireLead.count();

        spdlog::info("[Scheduler] 已创建{}定时任务 #{}: {}({}) 于 {}", entry.task.isDaily ? "每日" : "", id,
          entry.task.sessionType == "private" ? "私聊" : "群聊", entry.task.targetId,
          formatUnixTime(entry.task.remindTime));

        pushEntry(std::move(entry));
        return id;
    }

    void TaskScheduler::pushEntry(Entry entry) {
        {
            std::lock_guard lock(m_mutex);
            m_heap.push(std::move(entry));
        }
        m_cv.notify_all();
    }

    bool TaskScheduler::cancel(const int64_t id) {
        // 先登记取消集合再写库：缩小"弹出时既不在集合里、库里也已非 pending"的竞态窗口
        {
            std::lock_guard lock(m_mutex);
            m_cancelledIds.insert(id);
        }
        const bool ok = TaskStore::cancelScheduledTask(id);
        if (!ok) {
            std::lock_guard lock(m_mutex);
            m_cancelledIds.erase(id);
        }
        return ok;
    }

    void TaskScheduler::restorePendingTasks() {
        auto tasks = TaskStore::getPendingScheduledTasks();
        size_t overdue = 0;
        {
            std::lock_guard lock(m_mutex);
            const std::time_t now =
              std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
                .count();
            for (auto &task: tasks) {
                Entry entry;
                entry.task = std::move(task);
                // 已过期的任务钳制到当前时刻：恢复后立即补发，而不是按过期时间连发
                entry.fireTime = std::max<std::time_t>(entry.task.remindTime - kFireLead.count(), now);
                if (entry.task.remindTime <= now) {
                    overdue++;
                }
                m_heap.push(std::move(entry));
            }
        }
        spdlog::info("[Scheduler] 恢复待触发定时任务 {} 条（其中已过期 {} 条）", tasks.size(), overdue);
        if (!tasks.empty()) {
            m_cv.notify_all();
        }
    }

    void TaskScheduler::runLoop() {
        using Clock = std::chrono::system_clock;
        std::unique_lock lock(m_mutex);
        while (m_running.load()) {
            if (m_heap.empty()) {
                m_cv.wait(lock, [this] { return !m_heap.empty() || !m_running.load(); });
                continue;
            }
            m_cv.wait_until(lock, Clock::from_time_t(m_heap.top().fireTime));
            if (!m_running.load()) {
                break;
            }

            // 到期任务全部弹出再逐个触发；触发期间锁短暂放开，新创建的任务可同时入堆。
            // async_run 在本线程启动协程，首个真正的挂起点（HTTP 发送）之后续转到主循环执行，
            // 不会阻塞调度线程
            while (!m_heap.empty() && m_heap.top().fireTime <= Clock::to_time_t(Clock::now())) {
                TaskStore::ScheduledTask task = std::move(const_cast<Entry &>(m_heap.top()).task);
                m_heap.pop();
                if (m_cancelledIds.erase(task.id) > 0) {
                    continue;
                }
                lock.unlock();
                drogon::async_run([task = std::move(task)]() -> drogon::Task<> { co_await trigger(task); });
                lock.lock();
            }
        }
    }

    drogon::Task<> TaskScheduler::trigger(TaskStore::ScheduledTask task) {
        const uint64_t logSessionId =
          task.sessionType == "private" ? SessionId::fromPrivateUser(task.targetId) : task.targetId;

        const bool delayed = std::time(nullptr) > task.remindTime;
        Logger::session(logSessionId)
          .info("[Scheduler] 触发{}定时任务 #{} ({}{})", task.isDaily ? "每日" : "", task.id, delayed ? "延时，" : "",
            task.content.substr(0, 50));

        const auto body = buildSystemEvent(task, delayed);
        const auto resp =
          co_await HttpUtil::send("[Scheduler]", kSelfBaseUrl, "/", drogon::Post, body, "", 10.0, logSessionId);
        if (!resp || (*resp)->getStatusCode() != drogon::k200OK) {
            // HTTP 异常细节由 HttpUtil 记录；一次性任务无论成败都标记完成防止反复重发，每日任务次日自然重试
            spdlog::error("[Scheduler] 定时任务 #{} 注入失败", task.id);
        } else {
            Logger::session(logSessionId).info("[Scheduler] 定时任务 #{} 已注入消息接口", task.id);
        }

        if (!task.isDaily) {
            TaskStore::finishScheduledTask(task.id);
            co_return;
        }

        // 每日任务推进到下次触发重新入堆；更新以 pending 为条件，
        // 触发途中被取消（cancelledIds 已登记）则不再重排
        const std::time_t nextFire = nextDailyFire(task.remindTime);
        if (!TaskStore::rescheduleDailyTask(task.id, nextFire)) {
            co_return;
        }
        Entry entry;
        entry.task = std::move(task);
        entry.task.remindTime = nextFire;
        entry.fireTime = nextFire - kFireLead.count();
        Logger::session(logSessionId)
          .info("[Scheduler] 每日任务 #{} 已重排至下次触发：{}", entry.task.id, formatUnixTime(nextFire));
        instance().pushEntry(std::move(entry));
    }

    std::optional<std::time_t> TaskScheduler::parseTimeString(const std::string &input) {
        // 规整输入：去首尾空白、ISO 分隔符 T 视同空格
        const size_t begin = input.find_first_not_of(" \t\r\n");
        if (begin == std::string::npos) {
            return std::nullopt;
        }
        std::string text = input.substr(begin, input.find_last_not_of(" \t\r\n") - begin + 1);
        std::ranges::replace(text, 'T', ' ');

        static constexpr std::array formats{
          "%Y-%m-%d %H:%M:%S", "%Y/%m/%d %H:%M:%S", "%Y-%m-%d %H:%M", "%Y/%m/%d %H:%M"};
        for (const char *format: formats) {
            std::tm tm{};
            tm.tm_isdst = -1;
            std::istringstream stream(text);
            stream >> std::get_time(&tm, format);
            if (stream.fail()) {
                continue;
            }
            // get_time 对超范围数值不一定置错位，显式校验字段合法性
            if (tm.tm_mon < 0 || tm.tm_mon > 11 || tm.tm_mday < 1 || tm.tm_mday > 31 //
                || tm.tm_hour < 0 || tm.tm_hour > 23 || tm.tm_min < 0 || tm.tm_min > 59 || tm.tm_sec < 0 ||
                tm.tm_sec > 60) {
                return std::nullopt;
            }
            const std::time_t result = mktime(&tm);
            if (result == -1) {
                return std::nullopt;
            }
            return result;
        }
        return std::nullopt;
    }
} // namespace insoulforge
