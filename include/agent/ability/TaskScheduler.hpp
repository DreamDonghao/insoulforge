/// @file TaskScheduler.hpp
/// @brief 定时任务调度器 - 小根堆 + 独立等待线程
/// @author donghao
/// @date 2026-08-27
/// @details LLM 通过 create_scheduled_task 工具创建任务后的完整链路：
///          - 任务先落库（用户请求的原始时间为准），再入堆参与调度
///          - 到点前提前 kFireLead 触发（补偿一次回复的生成耗时），把任务包装成
///            OneBot 格式消息 POST 回消息接收接口，走正常 Router/Executor 管线生成回复
///          - 每日任务（isDaily）触发后不结束，自动推进到次日同一时刻重新入堆，直到被取消
///          - 重启时从数据库恢复全部 pending 任务；已过期的任务照常触发并标注延时
#pragma once
#include <atomic>
#include <condition_variable>
#include <drogon/utils/coroutine.h>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <agent/ability/TaskStore.hpp>
#include <thread>

namespace insoulforge {
    /// @brief 定时任务调度器
    class TaskScheduler {
    public:
        static TaskScheduler &instance();

        /// @brief 启动调度线程并恢复未完成任务（重复调用无副作用）
        void start();

        /// @brief 停止调度线程（析构自动调用）
        void stop();

        /// @brief 创建定时任务（先落库再入堆）
        /// @param task 任务内容（sessionType/targetId/remindTime/content/isDaily 需已填充）
        /// @return 任务 ID；数据库写入失败抛出异常
        int64_t schedule(TaskStore::ScheduledTask task);

        /// @brief 取消定时任务（写库标记 cancelled，堆内条目在弹出时惰性跳过）
        /// @return true=取消成功；false=任务不存在或已触发/已取消
        bool cancel(int64_t id);

        /// @brief 解析模型给出的时间字符串为本地时间 unix 秒。
        /// 兼容 YYYY-MM-DD / YYYY/MM/DD 与 HH:MM(:SS 可省)，分隔符 T 视同空格
        /// @return 解析结果；无法解析返回 nullopt
        [[nodiscard]] static std::optional<std::time_t> parseTimeString(const std::string &input);

    private:
        TaskScheduler() = default;

        ~TaskScheduler();

        struct Entry {
            TaskStore::ScheduledTask task;

            /// @brief 实际触发时刻（remindTime 减去提前量）
            std::time_t fireTime = 0;

            bool operator>(const Entry &other) const { return fireTime > other.fireTime; }
        };

        void runLoop();

        /// @brief 任务入堆并唤醒调度线程
        void pushEntry(Entry entry);

        /// @brief 加载数据库中全部 pending 任务入堆
        void restorePendingTasks();

        /// @brief 把任务合成 OneBot 消息注入接收接口；一次性任务标记完成，每日任务重排到次日（在 drogon 循环上执行）
        static drogon::Task<> trigger(TaskStore::ScheduledTask task);

        std::priority_queue<Entry, std::vector<Entry>, std::greater<>> m_heap;
        mutable std::mutex m_mutex;
        std::condition_variable m_cv;
        /// @brief 已取消任务的 ID 集合：priority_queue 不支持任意删除，弹出时过滤
        std::set<int64_t> m_cancelledIds;
        std::jthread m_thread;
        std::atomic_bool m_running{false};
    };
} // namespace insoulforge
