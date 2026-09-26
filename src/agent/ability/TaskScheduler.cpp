/// @file TaskScheduler.cpp
/// @brief 定时任务调度器 - 实现

#include <conversation/message/SessionId.hpp>
#include <conversation/workflow/OneBotEventWorkflow.hpp>
#include <include/agent/ability/TaskScheduler.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/config/Config.hpp>
#include <infrastructure/logging/Logger.hpp>

namespace insoulforge {
    namespace {
        /// @brief 触发提前量：提前这么多秒注入消息，补偿 Router+Executor 的回复生成耗时。
        /// 不宜过大，否则提醒会比用户指定时刻明显提前
        constexpr std::chrono::seconds kFireLead{5};

        /// @brief 单次等待的最大时长。
        /// @details 定期重新读取系统时钟，避免宿主机或容器校时后仍按旧绝对时刻等待数小时。
        constexpr std::chrono::minutes kMaximumWait{1};

        /// @brief 合成系统消息的发送者昵称与正文前缀
        constexpr std::string_view kSystemTaskLabel = "系统定时任务";

        /// @brief 计算每日任务的下次触发时刻：自上次时刻起逐日推进到严格晚于当前
        /// （mktime 归一化跨月/跨年，tm_isdst=-1 交给系统处理夏令时偏移）
        auto nextDailyFire(const std::time_t lastTime) -> std::time_t {
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

        auto buildText(const TaskStore::ScheduledTask &task, const bool delayed) -> std::string {
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

        auto buildSystemEvent(const TaskStore::ScheduledTask &task, const bool delayed) -> json {
            const auto &config = Config::instance();
            const std::string text = buildText(task, delayed);

            // 合成消息 ID 用远离真实 ID 的固定区段，避免与 NapCat 分配的冲突
            static std::atomic<i64> s_syntheticMsgId{0};
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

    auto TaskScheduler::instance() -> TaskScheduler & {
        static TaskScheduler scheduler;
        return scheduler;
    }

    TaskScheduler::~TaskScheduler() { stop(); }

    void TaskScheduler::start() {
        if (bool expected = false; !m_running.compare_exchange_strong(expected, true)) {
            return;
        }

        restorePendingTasks();
        m_thread = std::jthread([this] -> void { runLoop(); });
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

    auto TaskScheduler::schedule(TaskStore::ScheduledTask task) -> i64 {
        const i64 id = TaskStore::addScheduledTask(task);

        Entry entry;
        entry.task = std::move(task);
        entry.task.id = id;
        entry.fireTime = entry.task.remindTime - kFireLead.count();

        Logger::info(0, "Scheduler",
          fmt::format("已创建{}定时任务 #{}: {}({}) 于 {}", entry.task.isDaily ? "每日" : "", id,
            entry.task.sessionType == "private" ? "私聊" : "群聊", entry.task.targetId,
            formatUnixTime(entry.task.remindTime)));

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

    auto TaskScheduler::cancel(const i64 id) -> bool {
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
        Logger::info(
          0, "Scheduler", fmt::format("恢复待触发定时任务 {} 条（其中已过期 {} 条）", tasks.size(), overdue));
        if (!tasks.empty()) {
            m_cv.notify_all();
        }
    }

    void TaskScheduler::runLoop() {
        using Clock = std::chrono::system_clock;
        std::unique_lock lock(m_mutex);
        while (m_running.load()) {
            if (m_heap.empty()) {
                m_cv.wait(lock, [this] -> bool { return !m_heap.empty() || !m_running.load(); });
                continue;
            }

            const auto now = Clock::now();
            const auto fireTime = Clock::from_time_t(m_heap.top().fireTime);
            if (fireTime > now) {
                // 不直接等待到数小时后的绝对时刻。系统时间被校正后，至多一分钟便会重新计算剩余时间。
                const auto maximumWait = std::chrono::duration_cast<Clock::duration>(kMaximumWait);
                m_cv.wait_for(lock, std::min(fireTime - now, maximumWait));
                continue;
            }
            if (!m_running.load()) {
                break;
            }

            // 到期任务全部弹出再逐个触发；触发期间锁短暂放开，新创建的任务可同时入堆。
            while (!m_heap.empty() && m_heap.top().fireTime <= Clock::to_time_t(Clock::now())) {
                TaskStore::ScheduledTask task = std::move(const_cast<Entry &>(m_heap.top()).task);
                m_heap.pop();
                if (m_cancelledIds.erase(task.id) > 0) {
                    continue;
                }
                lock.unlock();
                trigger(std::move(task));
                lock.lock();
            }
        }
    }

    void TaskScheduler::trigger(TaskStore::ScheduledTask task) {
        const u64 logSessionId =
          task.sessionType == "private" ? SessionId::fromPrivateUser(task.targetId) : task.targetId;

        const std::time_t now = std::time(nullptr);
        const bool delayed = now > task.remindTime;
        Logger::info(logSessionId, "Scheduler",
          fmt::format("触发{}定时任务 #{} ({}{} | 计划={} | 实际={})", task.isDaily ? "每日" : "", task.id,
            delayed ? "延时，" : "", task.content.substr(0, 50), formatUnixTime(task.remindTime), formatUnixTime(now)));

        try {
            OneBotEventWorkflow::instance().enqueueOneBotEvent(buildSystemEvent(task, delayed));
            Logger::info(logSessionId, "Scheduler", fmt::format("定时任务 #{} 已加入消息工作流", task.id));
        } catch (const std::exception &error) {
            Logger::error(
              logSessionId, "Scheduler", fmt::format("定时任务 #{} 加入消息工作流失败: {}", task.id, error.what()));
        } catch (...) {
            Logger::error(logSessionId, "Scheduler", fmt::format("定时任务 #{} 加入消息工作流失败: 未知错误", task.id));
        }

        if (!task.isDaily) {
            TaskStore::finishScheduledTask(task.id);
            return;
        }

        // 每日任务推进到下次触发重新入堆；更新以 pending 为条件，
        // 触发途中被取消（cancelledIds 已登记）则不再重排
        const std::time_t nextFire = nextDailyFire(task.remindTime);
        if (!TaskStore::rescheduleDailyTask(task.id, nextFire)) {
            return;
        }
        Entry entry;
        entry.task = std::move(task);
        entry.task.remindTime = nextFire;
        entry.fireTime = nextFire - kFireLead.count();
        Logger::info(logSessionId, "Scheduler",
          fmt::format("每日任务 #{} 已重排至下次触发：{}", entry.task.id, formatUnixTime(nextFire)));
        instance().pushEntry(std::move(entry));
    }

    auto TaskScheduler::parseTimeString(const std::string &input) -> std::optional<std::time_t> {
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
