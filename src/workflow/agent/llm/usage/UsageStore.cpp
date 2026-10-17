/// @file UsageStore.cpp
/// @brief LLM 用量统计存储 - 实现

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge::UsageStore {
    void addUsageRecord(const std::string &role, const std::string &model, const i32 promptTokens,
      const i32 completionTokens, const i32 totalTokens, const i32 cachedTokens) {
        const auto &db = Database::instance();
        std::unique_lock lock(db.mutex());
        const Statement stmt(db.handle(),
          "INSERT INTO llm_usage (role, model, prompt_tokens, completion_tokens, total_tokens, cached_tokens) "
          "VALUES (?, ?, ?, ?, ?, ?)");
        stmt.bind(1, role);
        stmt.bind(2, model);
        stmt.bind(3, promptTokens);
        stmt.bind(4, completionTokens);
        stmt.bind(5, totalTokens);
        stmt.bind(6, cachedTokens);
        stmt.exec();
    }

    auto getUsageSummary(const i32 days) -> json {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        json result;
        const std::string sinceDate = fmt::format("-{} days", days);

        // 汇总
        {
            const Statement stmt(db.handle(),
              "SELECT COUNT(*), COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0), "
              "COALESCE(SUM(total_tokens),0), COALESCE(SUM(cached_tokens),0) "
              "FROM llm_usage WHERE created_at >= datetime('now', 'localtime', ?)");
            stmt.bind(1, sinceDate);
            if (stmt.step()) {
                result["total_calls"] = stmt.getInt(0);
                result["total_prompt"] = stmt.getInt64(1);
                result["total_completion"] = stmt.getInt64(2);
                result["total_tokens"] = stmt.getInt64(3);
                result["total_cached"] = stmt.getInt64(4);
            }
        }

        // 今日汇总（按本地日期）
        {
            const Statement stmt(db.handle(),
              "SELECT COUNT(*), COALESCE(SUM(prompt_tokens),0), COALESCE(SUM(completion_tokens),0), "
              "COALESCE(SUM(total_tokens),0), COALESCE(SUM(cached_tokens),0) "
              "FROM llm_usage WHERE date(created_at, 'localtime') = date('now', 'localtime')");
            if (stmt.step()) {
                result["today"]["calls"] = stmt.getInt(0);
                result["today"]["prompt"] = stmt.getInt64(1);
                result["today"]["completion"] = stmt.getInt64(2);
                result["today"]["total"] = stmt.getInt64(3);
                result["today"]["cached"] = stmt.getInt64(4);
            }
        }

        // 今日按角色
        {
            json todayByRole = json::array();
            const Statement stmt(db.handle(),
              "SELECT usage.role, (SELECT latest.model FROM llm_usage AS latest "
              "WHERE latest.role = usage.role AND date(latest.created_at, 'localtime') = date('now', 'localtime') "
              "ORDER BY latest.id DESC LIMIT 1), COUNT(*), COALESCE(SUM(prompt_tokens),0), "
              "COALESCE(SUM(completion_tokens),0), COALESCE(SUM(total_tokens),0), COALESCE(SUM(cached_tokens),0) "
              "FROM llm_usage AS usage WHERE date(usage.created_at, 'localtime') = date('now', 'localtime') "
              "GROUP BY usage.role ORDER BY SUM(usage.total_tokens) DESC");
            while (stmt.step()) {
                json item;
                item["role"] = stmt.getText(0);
                item["model"] = stmt.getText(1);
                item["calls"] = stmt.getInt(2);
                item["prompt"] = stmt.getInt64(3);
                item["completion"] = stmt.getInt64(4);
                item["total"] = stmt.getInt64(5);
                item["cached"] = stmt.getInt64(6);
                todayByRole.push_back(item);
            }
            result["today_by_role"] = todayByRole;
        }

        // 按角色
        {
            json byRole = json::array();
            const Statement stmt(db.handle(),
              "SELECT usage.role, (SELECT latest.model FROM llm_usage AS latest "
              "WHERE latest.role = usage.role AND latest.created_at >= datetime('now', 'localtime', ?) "
              "ORDER BY latest.id DESC LIMIT 1), COUNT(*), COALESCE(SUM(prompt_tokens),0), "
              "COALESCE(SUM(completion_tokens),0), COALESCE(SUM(total_tokens),0), COALESCE(SUM(cached_tokens),0) "
              "FROM llm_usage AS usage WHERE usage.created_at >= datetime('now', 'localtime', ?) "
              "GROUP BY usage.role ORDER BY SUM(usage.total_tokens) DESC");
            stmt.bind(1, sinceDate);
            stmt.bind(2, sinceDate);
            while (stmt.step()) {
                json item;
                item["role"] = stmt.getText(0);
                item["model"] = stmt.getText(1);
                item["calls"] = stmt.getInt(2);
                item["prompt"] = stmt.getInt64(3);
                item["completion"] = stmt.getInt64(4);
                item["total"] = stmt.getInt64(5);
                item["cached"] = stmt.getInt64(6);
                byRole.push_back(item);
            }
            result["by_role"] = byRole;
        }

        // 按天
        {
            json byDay = json::array();
            const Statement stmt(db.handle(),
              "SELECT date(created_at, 'localtime') AS day, COUNT(*), COALESCE(SUM(total_tokens),0) "
              "FROM llm_usage WHERE created_at >= datetime('now', 'localtime', ?) "
              "GROUP BY day ORDER BY day");
            stmt.bind(1, sinceDate);
            while (stmt.step()) {
                json item;
                item["day"] = stmt.getText(0);
                item["calls"] = stmt.getInt(1);
                item["total"] = stmt.getInt64(2);
                byDay.push_back(item);
            }
            result["by_day"] = byDay;
        }

        return result;
    }

    auto getRecentUsage(const i32 limit) -> json {
        const auto &db = Database::instance();
        std::shared_lock lock(db.mutex());
        json result = json::array();
        const Statement stmt(db.handle(), "SELECT datetime(created_at, 'localtime') AS time, role, model, "
                                          "prompt_tokens, completion_tokens, total_tokens, cached_tokens "
                                          "FROM llm_usage ORDER BY id DESC LIMIT ?");
        stmt.bind(1, limit);
        while (stmt.step()) {
            json item;
            item["time"] = stmt.getText(0);
            item["role"] = stmt.getText(1);
            item["model"] = stmt.getText(2);
            item["prompt"] = stmt.getInt(3);
            item["completion"] = stmt.getInt(4);
            item["total"] = stmt.getInt(5);
            item["cached"] = stmt.getInt(6);
            result.push_back(item);
        }
        return result;
    }
} // namespace insoulforge::UsageStore
