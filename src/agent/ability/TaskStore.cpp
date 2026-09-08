/// @file TaskStore.cpp
/// @brief 定时任务存储 - 实现
/// @author donghao
/// @date 2026-08-30

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <agent/ability/TaskStore.hpp>

namespace insoulforge {
    namespace TaskStore {
        int64_t addScheduledTask(const ScheduledTask &task) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(),
              "INSERT INTO scheduled_tasks (session_type, target_id, remind_time, content, is_daily) "
              "VALUES (?, ?, ?, ?, ?)");
            stmt.bind(1, task.sessionType);
            stmt.bind(2, task.targetId);
            stmt.bind(3, task.remindTime);
            stmt.bind(4, task.content);
            stmt.bind(5, task.isDaily);
            stmt.exec();
            return Statement::lastInsertRowId(db.handle());
        }

        std::vector<ScheduledTask> getPendingScheduledTasks() {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::vector<ScheduledTask> tasks;
            const Statement stmt(db.handle(),
              "SELECT id, session_type, target_id, remind_time, content, is_daily FROM scheduled_tasks "
              "WHERE status = 'pending' ORDER BY remind_time ASC");
            while (stmt.step()) {
                ScheduledTask task;
                task.id = stmt.getInt64(0);
                task.sessionType = stmt.getText(1);
                task.targetId = stmt.getInt64(2);
                task.remindTime = stmt.getInt64(3);
                task.content = stmt.getText(4);
                task.isDaily = stmt.getInt(5) != 0;
                tasks.push_back(std::move(task));
            }
            return tasks;
        }

        std::vector<ScheduledTask> getPendingScheduledTasksByTarget(
          const std::string &sessionType, const uint64_t targetId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::vector<ScheduledTask> tasks;
            const Statement stmt(db.handle(),
              "SELECT id, session_type, target_id, remind_time, content, is_daily FROM scheduled_tasks "
              "WHERE status = 'pending' AND session_type = ? AND target_id = ? ORDER BY remind_time ASC");
            stmt.bind(1, sessionType);
            stmt.bind(2, targetId);
            while (stmt.step()) {
                ScheduledTask task;
                task.id = stmt.getInt64(0);
                task.sessionType = stmt.getText(1);
                task.targetId = stmt.getInt64(2);
                task.remindTime = stmt.getInt64(3);
                task.content = stmt.getText(4);
                task.isDaily = stmt.getInt(5) != 0;
                tasks.push_back(std::move(task));
            }
            return tasks;
        }

        bool cancelScheduledTask(const int64_t id) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(
              db.handle(), "UPDATE scheduled_tasks SET status='cancelled' WHERE id=? AND status='pending'");
            stmt.bind(1, id);
            stmt.exec();
            return Statement::changes(db.handle()) > 0;
        }

        bool rescheduleDailyTask(const int64_t id, const int64_t nextTime) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            // 条件限定 pending：触发途中被取消时不复活任务
            const Statement stmt(
              db.handle(), "UPDATE scheduled_tasks SET remind_time=? WHERE id=? AND status='pending'");
            stmt.bind(1, nextTime);
            stmt.bind(2, id);
            stmt.exec();
            return Statement::changes(db.handle()) > 0;
        }

        void finishScheduledTask(const int64_t id) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            // 条件限定 pending：触发途中被取消时不覆盖 cancelled 状态
            const Statement stmt(
              db.handle(), "UPDATE scheduled_tasks SET status='done' WHERE id=? AND status='pending'");
            stmt.bind(1, id);
            stmt.exec();
        }
    } // namespace TaskStore
} // namespace insoulforge
