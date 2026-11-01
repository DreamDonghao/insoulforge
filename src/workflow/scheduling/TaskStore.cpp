/// @file TaskStore.cpp
/// @brief 定时任务存储 - 实现

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <workflow/scheduling/TaskStore.hpp>

namespace insoulforge::TaskStore {
    namespace {
        auto readTask(const Statement &row) -> ScheduledTask {
            return {.id = row.getInt64(0),
              .sessionId = SessionId::parse(row.getText(1)).value(),
              .remindTime = row.getInt64(2),
              .content = row.getText(3),
              .isDaily = row.getInt(4) != 0};
        }
    } // namespace
    auto addScheduledTask(const ScheduledTask &task) -> i64 {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(
          db.handle(), "INSERT INTO scheduled_tasks (session_id,remind_time,content,is_daily) VALUES (?,?,?,?)");
        stmt.bind(1, task.sessionId);
        stmt.bind(2, task.remindTime);
        stmt.bind(3, task.content);
        stmt.bind(4, task.isDaily);
        stmt.execOrThrow();
        return Statement::lastInsertRowId(db.handle());
    }
    auto getPendingScheduledTasks() -> std::vector<ScheduledTask> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT id,session_id,remind_time,content,is_daily FROM scheduled_tasks "
                                          "WHERE status='pending' ORDER BY remind_time");
        std::vector<ScheduledTask> result;
        while (stmt.step())
            result.push_back(readTask(stmt));
        return result;
    }
    auto getPendingScheduledTasksBySession(const SessionId &sessionId) -> std::vector<ScheduledTask> {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT id,session_id,remind_time,content,is_daily FROM scheduled_tasks "
                                          "WHERE status='pending' AND session_id=? ORDER BY remind_time");
        stmt.bind(1, sessionId);
        std::vector<ScheduledTask> result;
        while (stmt.step())
            result.push_back(readTask(stmt));
        return result;
    }

    auto cancelScheduledTask(const i64 id) -> bool {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(
          db.handle(), "UPDATE scheduled_tasks SET status='cancelled' WHERE id=? AND status='pending'");
        stmt.bind(1, id);
        stmt.exec();
        return Statement::changes(db.handle()) > 0;
    }

    auto rescheduleDailyTask(const i64 id, const i64 nextTime) -> bool {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        // 条件限定 pending：触发途中被取消时不复活任务
        const Statement stmt(db.handle(), "UPDATE scheduled_tasks SET remind_time=? WHERE id=? AND status='pending'");
        stmt.bind(1, nextTime);
        stmt.bind(2, id);
        stmt.exec();
        return Statement::changes(db.handle()) > 0;
    }

    void finishScheduledTask(const i64 id) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        // 条件限定 pending：触发途中被取消时不覆盖 cancelled 状态
        const Statement stmt(db.handle(), "UPDATE scheduled_tasks SET status='done' WHERE id=? AND status='pending'");
        stmt.bind(1, id);
        stmt.exec();
    }
} // namespace insoulforge::TaskStore
