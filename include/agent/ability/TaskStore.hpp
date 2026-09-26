/// @file TaskStore.hpp
/// @brief 定时任务存储
/// @details 表：scheduled_tasks（提醒类定时任务）

#pragma once

#include <cstdint>
#include <infrastructure/NumericTypes.hpp>
#include <string>
#include <vector>

/// @brief 定时任务存储
namespace insoulforge::TaskStore {
    /// @brief 定时任务结构
    struct ScheduledTask {
        i64 id = 0;
        std::string sessionType; ///< "group" | "private"
        u64 targetId = 0; ///< 群号或私聊用户QQ号
        i64 remindTime = 0; ///< 提醒时间（unix 秒，用户请求的原始时间）
        std::string content; ///< 触发时的提醒内容
        bool isDaily = false; ///< 每日重复任务：触发后自动推进到次日同一时刻，取消前一直有效
    };

    /// @brief 新增定时任务
    /// @return 任务 ID
    auto addScheduledTask(const ScheduledTask &task) -> i64;

    /// @brief 获取所有待触发的定时任务（按提醒时间升序）
    auto getPendingScheduledTasks() -> std::vector<ScheduledTask>;

    /// @brief 获取指定会话待触发的定时任务（按提醒时间升序）
    auto getPendingScheduledTasksByTarget(const std::string &sessionType, u64 targetId) -> std::vector<ScheduledTask>;

    /// @brief 取消待触发的定时任务
    /// @return true=取消成功；false=任务不存在或已触发/已取消
    auto cancelScheduledTask(i64 id) -> bool;

    /// @brief 推进每日任务的下次触发时刻
    /// @return true=成功；false=任务不存在或非 pending（如触发途中被取消）
    auto rescheduleDailyTask(i64 id, i64 nextTime) -> bool;

    /// @brief 标记定时任务已完成触发
    void finishScheduledTask(i64 id);
} // namespace insoulforge::TaskStore
