/// @file TaskStore.hpp
/// @brief 定时任务存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：scheduled_tasks（提醒类定时任务）

#pragma once
#include <cstdint>
#include <string>
#include <vector>

/// @brief 定时任务存储
namespace insoulforge::TaskStore {
    /// @brief 定时任务结构
    struct ScheduledTask {
        int64_t id = 0;
        std::string sessionType; ///< "group" | "private"
        uint64_t targetId = 0; ///< 群号或私聊用户QQ号
        int64_t remindTime = 0; ///< 提醒时间（unix 秒，用户请求的原始时间）
        std::string content; ///< 触发时的提醒内容
        bool isDaily = false; ///< 每日重复任务：触发后自动推进到次日同一时刻，取消前一直有效
    };

    /// @brief 新增定时任务
    /// @return 任务 ID
    int64_t addScheduledTask(const ScheduledTask &task);

    /// @brief 获取所有待触发的定时任务（按提醒时间升序）
    std::vector<ScheduledTask> getPendingScheduledTasks();

    /// @brief 获取指定会话待触发的定时任务（按提醒时间升序）
    std::vector<ScheduledTask> getPendingScheduledTasksByTarget(const std::string &sessionType, uint64_t targetId);

    /// @brief 取消待触发的定时任务
    /// @return true=取消成功；false=任务不存在或已触发/已取消
    bool cancelScheduledTask(int64_t id);

    /// @brief 推进每日任务的下次触发时刻
    /// @return true=成功；false=任务不存在或非 pending（如触发途中被取消）
    bool rescheduleDailyTask(int64_t id, int64_t nextTime);

    /// @brief 标记定时任务已完成触发
    void finishScheduledTask(int64_t id);
} // namespace insoulforge::TaskStore
