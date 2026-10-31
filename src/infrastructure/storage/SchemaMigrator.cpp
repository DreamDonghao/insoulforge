/// @file SchemaMigrator.cpp
/// @brief 数据库 Schema 版本迁移 - 实现

#include <array>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <infrastructure/storage/migrations/V25.hpp>

namespace insoulforge {
    namespace {
        void execSQL(sqlite3 *db, std::string_view sql) {
            char *errMsg = nullptr;
            if (sqlite3_exec(db, std::string(sql).c_str(), nullptr, nullptr, &errMsg) != SQLITE_OK) {
                std::string err = errMsg ? errMsg : sqlite3_errmsg(db);
                sqlite3_free(errMsg);
                Logger::error(0, "Storage", fmt::format("迁移 SQL 执行失败: {} - {}", sql, err));
                throw DbError(err);
            }
        }

        template<size_t N>
        void execAll(sqlite3 *db, const std::array<const char *, N> &statements) {
            for (const auto *sql: statements)
                execSQL(db, sql);
        }

        // 新安装直接创建当前结构，不再执行历史版本迁移。
        constexpr std::array currentTables = {
          R"(CREATE TABLE IF NOT EXISTS group_config (
        group_id INTEGER PRIMARY KEY,
        all_mes_count INTEGER DEFAULT 0,
        send_group_card INTEGER NOT NULL DEFAULT 0,
        auto_reply INTEGER NOT NULL DEFAULT 1,
        command_enabled INTEGER NOT NULL DEFAULT 1,
        cross_session_sharing INTEGER NOT NULL DEFAULT 0,
        additional_prompt TEXT NOT NULL DEFAULT ''
    ))",
          R"(CREATE TABLE IF NOT EXISTS chat_records (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        group_id INTEGER NOT NULL,
        role TEXT NOT NULL CHECK(role IN ('user', 'assistant')),
        content TEXT NOT NULL,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS short_term_memory (
        group_id INTEGER PRIMARY KEY,
        memory_content TEXT,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS prompts (
        prompt_key TEXT PRIMARY KEY,
        prompt_content TEXT NOT NULL,
        description TEXT,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS enabled_groups (
        group_id INTEGER PRIMARY KEY,
        group_name TEXT,
        enabled INTEGER DEFAULT 1,
        added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS admins (
        user_id INTEGER PRIMARY KEY,
        added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS llm_usage (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        role TEXT NOT NULL DEFAULT '',
        model TEXT NOT NULL,
        prompt_tokens INTEGER DEFAULT 0,
        completion_tokens INTEGER DEFAULT 0,
        total_tokens INTEGER DEFAULT 0,
        cached_tokens INTEGER DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS scheduled_tasks (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_type TEXT NOT NULL CHECK(session_type IN ('group', 'private')),
        target_id INTEGER NOT NULL,
        remind_time INTEGER NOT NULL,
        content TEXT NOT NULL,
        status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending', 'done', 'cancelled')),
        is_daily INTEGER NOT NULL DEFAULT 0 CHECK(is_daily IN (0, 1)),
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS long_term_memory (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        group_id INTEGER NOT NULL,
        content TEXT NOT NULL,
        embedding BLOB,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS image_description_cache (
        content_hash TEXT NOT NULL,
        model TEXT NOT NULL,
        prompt_version INTEGER NOT NULL,
        media_type TEXT NOT NULL,
        status TEXT NOT NULL CHECK(status IN ('succeeded', 'failed')),
        description TEXT NOT NULL DEFAULT '',
        sampled_frame_count INTEGER NOT NULL DEFAULT 1,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        PRIMARY KEY (content_hash, model, prompt_version)
    ))",
          R"(CREATE TABLE IF NOT EXISTS blacklisted_users (
        user_id INTEGER PRIMARY KEY,
        added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS person_affinity (
        user_id INTEGER PRIMARY KEY,
        affinity INTEGER NOT NULL DEFAULT 0 CHECK(affinity BETWEEN -100 AND 100)
    ))",
          R"(CREATE TABLE IF NOT EXISTS person_memories (
        user_id INTEGER PRIMARY KEY,
        profile TEXT NOT NULL DEFAULT '[]',
        recent_interactions TEXT NOT NULL DEFAULT '[]',
        relationship TEXT NOT NULL DEFAULT '[]'
    ))",
          R"(CREATE TABLE IF NOT EXISTS person_memory_entries (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        user_id INTEGER NOT NULL,
        source_session INTEGER NOT NULL,
        content TEXT NOT NULL,
        created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS person_memory_daily_state (
        id INTEGER PRIMARY KEY CHECK(id = 1),
        completed_day TEXT NOT NULL DEFAULT '',
        running_day TEXT NOT NULL DEFAULT ''
    ))"};

        constexpr std::array indexes = {
          "CREATE INDEX IF NOT EXISTS idx_chat_records_group ON chat_records(group_id)",
          "CREATE INDEX IF NOT EXISTS idx_chat_records_time ON chat_records(group_id, created_at DESC)",
          "CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_status ON scheduled_tasks(status, remind_time)",
          "CREATE INDEX IF NOT EXISTS idx_long_term_memory_group ON long_term_memory(group_id)",
          "CREATE INDEX IF NOT EXISTS idx_person_memory_entries_user_id ON person_memory_entries(user_id, id)"};

        auto getUserVersion(sqlite3 *db) -> i32 {
            const Statement stmt(db, "PRAGMA user_version");
            return stmt.step() ? stmt.getInt(0) : 0;
        }

        void setUserVersion(sqlite3 *db, const i32 version) {
            execSQL(db, fmt::format("PRAGMA user_version = {}", version));
        }

        auto hasUserTables(sqlite3 *db) -> bool {
            const Statement stmt(
              db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1");
            return stmt.step();
        }

        // 建库和每次增量迁移都整体提交；失败不留下半完成的结构或数据。
        void transaction(sqlite3 *db, const auto &action) {
            execSQL(db, "BEGIN IMMEDIATE");
            try {
                action();
                execSQL(db, "COMMIT");
            } catch (...) {
                sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
                throw;
            }
        }
    } // namespace

    namespace SchemaMigrator {
        void migrate(sqlite3 *db) {
            const i32 version = getUserVersion(db);
            if (version == 0 && !hasUserTables(db)) {
                Logger::info(0, "Storage", fmt::format("检测到全新数据库，直接创建最新 Schema (v{})", kLatestVersion));
                transaction(db, [db] {
                    execAll(db, currentTables);
                    execAll(db, indexes);
                    setUserVersion(db, kLatestVersion);
                });
                return;
            }

            // 在执行任何写入前检查兼容范围，旧库必须先用 v1.7.x 完成中转升级。
            if (version < kMinimumSupportedVersion)
                throw DbError(fmt::format("数据库版本 v{} 过旧，最低支持 v{}（InSoulForge v1.7.0）。"
                                         "请先运行支持旧库迁移的 InSoulForge v1.7.x（例如 v1.7.2）完成数据库升级，再安装当前版本。",
                  version, kMinimumSupportedVersion));
            if (version > kLatestVersion)
                throw DbError(fmt::format("数据库版本 v{} 高于当前程序支持的 v{}，请使用匹配或更新的程序版本。",
                  version, kLatestVersion));
            if (version == kLatestVersion)
                return;

            // steps[i] 将 kMinimumSupportedVersion + i 升级到下一版本。
            constexpr std::array steps = {&migrateV24ToV25};
            static_assert(steps.size() == kLatestVersion - kMinimumSupportedVersion);
            Logger::info(0, "Storage", fmt::format("数据库版本 v{}，开始迁移到 v{}", version, kLatestVersion));
            for (i32 v = version; v < kLatestVersion; ++v) {
                try {
                    transaction(db, [db, v, &steps] {
                        steps[v - kMinimumSupportedVersion](db);
                        setUserVersion(db, v + 1);
                    });
                } catch (...) {
                    Logger::error(0, "Storage", fmt::format("数据库迁移 v{} -> v{} 失败，已回滚", v, v + 1));
                    throw;
                }
                Logger::info(0, "Storage", fmt::format("数据库迁移完成: v{} -> v{}", v, v + 1));
            }
        }
    } // namespace SchemaMigrator
} // namespace insoulforge
