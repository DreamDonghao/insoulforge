/// @file SchemaMigrator.cpp
/// @brief 数据库 Schema 版本迁移 - 实现
/// @author donghao
/// @date 2026-08-30

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/logging/Logger.hpp>
#include <infrastructure/storage/SchemaMigrator.hpp>
#include <infrastructure/storage/Statement.hpp>

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

        bool tableExists(sqlite3 *db, std::string_view name) {
            const Statement stmt(db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?");
            stmt.bind(1, name);
            return stmt.step();
        }

        bool columnExists(sqlite3 *db, std::string_view table, std::string_view column) {
            const Statement stmt(db, fmt::format("PRAGMA table_info({})", table));
            while (stmt.step()) {
                if (stmt.getText(1) == column)
                    return true;
            }
            return false;
        }

        void ensureColumn(sqlite3 *db, std::string_view table, std::string_view column, std::string_view ddl) {
            if (columnExists(db, table, column))
                return;
            Logger::info(0, "Storage", fmt::format("数据库迁移: 新增 {}.{}", table, column));
            execSQL(db, fmt::format("ALTER TABLE {} ADD COLUMN {}", table, ddl));
        }

        void dropColumnIfExists(sqlite3 *db, std::string_view table, std::string_view column) {
            if (!columnExists(db, table, column))
                return;
            Logger::info(0, "Storage", fmt::format("数据库迁移: 移除废弃的 {}.{}", table, column));
            execSQL(db, fmt::format("ALTER TABLE {} DROP COLUMN {}", table, column));
        }

        void storeSetting(sqlite3 *db, std::string_view key, const json &value) {
            const std::string payload = dumpJson(value);

            const Statement stmt(db, "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)");
            stmt.bind(1, key);
            stmt.bind(2, payload);
            stmt.exec();
        }

        /// @brief v2 Schema 的全部建表语句（IF NOT EXISTS，可安全重复执行）
        constexpr std::array v2Tables = {
          R"(CREATE TABLE IF NOT EXISTS group_config (
        group_id INTEGER PRIMARY KEY,
        all_mes_count INTEGER DEFAULT 0
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
        qq_number INTEGER PRIMARY KEY,
        added_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          R"(CREATE TABLE IF NOT EXISTS llm_config (
        name TEXT PRIMARY KEY,
        api_key TEXT,
        base_url TEXT,
        path TEXT,
        model TEXT,
        max_tokens INTEGER DEFAULT 1024,
        temperature REAL DEFAULT 0.7,
        top_p REAL DEFAULT 0.9,
        reasoning_effort TEXT DEFAULT ''
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
          R"(CREATE TABLE IF NOT EXISTS custom_tools (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        name TEXT UNIQUE NOT NULL,
        description TEXT NOT NULL,
        parameters TEXT,
        executor_type TEXT NOT NULL CHECK(executor_type IN ('python', 'http')),
        executor_config TEXT,
        script_content TEXT,
        readme TEXT,
        enabled INTEGER DEFAULT 1,
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
          R"(CREATE TABLE IF NOT EXISTS settings (
        key TEXT PRIMARY KEY,
        value TEXT
    ))"};

        constexpr std::array indexes = {"CREATE INDEX IF NOT EXISTS idx_chat_records_group ON chat_records(group_id)",
          "CREATE INDEX IF NOT EXISTS idx_chat_records_time ON chat_records(group_id, created_at DESC)",
          "CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_status ON scheduled_tasks(status, remind_time)"};

        /// @brief 基线 v1 Schema 额外需要的三张单行配置表（v2 已并入 settings）
        constexpr std::array v1ExtraTables = {
          R"(CREATE TABLE IF NOT EXISTS kb_config (id INTEGER PRIMARY KEY CHECK (id = 1), enabled INTEGER DEFAULT 1, api_key TEXT, base_url TEXT, knowledge_dataset_id TEXT, memory_dataset_id TEXT, memory_document_id TEXT))",
          R"(CREATE TABLE IF NOT EXISTS memory_config (id INTEGER PRIMARY KEY CHECK (id = 1), window_trigger_count INTEGER DEFAULT 100, window_keep_count INTEGER DEFAULT 50, memory_extract_max_tokens INTEGER DEFAULT 4000, router_window_trigger_count INTEGER DEFAULT 20, router_window_keep_count INTEGER DEFAULT 10, short_term_memory_max INTEGER DEFAULT 15, memory_migrate_count INTEGER DEFAULT 5))",
          R"(CREATE TABLE IF NOT EXISTS qq_config (id INTEGER PRIMARY KEY CHECK (id = 1), access_token TEXT, self_qq_number INTEGER, qq_http_host TEXT, bot_name TEXT DEFAULT '小喵'))"};

        /// @brief v3 新增表：group_affinity（每个会话独立维护 QQ 号 → 好感度映射，[-100, 100]）
        constexpr std::array v3Tables = {
          R"(CREATE TABLE IF NOT EXISTS group_affinity (
        group_id INTEGER NOT NULL,
        qq_number INTEGER NOT NULL,
        affinity INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY (group_id, qq_number)
    ))"};

        /// @brief v6 新增表：long_term_memory（长期记忆，embedding 以 float 数组存 BLOB，检索为暴力余弦）
        constexpr std::array v6Tables = {
          R"(CREATE TABLE IF NOT EXISTS long_term_memory (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        group_id INTEGER NOT NULL,
        content TEXT NOT NULL,
        embedding BLOB,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          "CREATE INDEX IF NOT EXISTS idx_long_term_memory_group ON long_term_memory(group_id)"};

        /// @brief v7 新增表：按媒体字节哈希缓存视觉描述，避免相同图片重复调用视觉模型
        constexpr std::array v7Tables = {
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
    ))"};

        /// @brief v8 新增表：持久化记忆维护任务，避免重启或失败时丢失总结输入
        constexpr std::array v8Tables = {
          R"(CREATE TABLE IF NOT EXISTS memory_maintenance_jobs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id INTEGER NOT NULL,
        messages TEXT NOT NULL,
        context_messages TEXT NOT NULL DEFAULT '[]',
        remove_count INTEGER NOT NULL DEFAULT 0,
        status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending', 'completed')),
        attempt_count INTEGER NOT NULL DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          "CREATE INDEX IF NOT EXISTS idx_memory_maintenance_jobs_session ON memory_maintenance_jobs(session_id, id)"};

        /// @brief v10 新增表：独立持久化好感度维护任务
        constexpr std::array v10Tables = {
          R"(CREATE TABLE IF NOT EXISTS affinity_maintenance_jobs (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        session_id INTEGER NOT NULL,
        messages TEXT NOT NULL,
        attempt_count INTEGER NOT NULL DEFAULT 0,
        created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
        updated_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    ))",
          "CREATE INDEX IF NOT EXISTS idx_affinity_maintenance_jobs_session ON affinity_maintenance_jobs(session_id, "
          "id)"};
        int getUserVersion(sqlite3 *db) {
            const Statement stmt(db, "PRAGMA user_version");
            return stmt.step() ? stmt.getInt(0) : 0;
        }

        void setUserVersion(sqlite3 *db, const int version) {
            execSQL(db, fmt::format("PRAGMA user_version = {}", version));
        }

        bool hasUserTables(sqlite3 *db) {
            const Statement stmt(
              db, "SELECT 1 FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' LIMIT 1");
            return stmt.step();
        }

        void createFreshSchema(sqlite3 *db) {
            execAll(db, v2Tables);
            execAll(db, v3Tables);
            execAll(db, v6Tables);
            execAll(db, v7Tables);
            execAll(db, v8Tables);
            execAll(db, v10Tables);
            execAll(db, indexes);
            setUserVersion(db, SchemaMigrator::kLatestVersion);
        }

        void migrateV0ToV1(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行基线迁移: 历史遗留 Schema 调整"));

            // 兜底补齐所有表（正常情况下旧库已全部存在）
            execAll(db, v2Tables);
            execAll(db, v1ExtraTables);
            execAll(db, indexes);

            ensureColumn(db, "kb_config", "enabled", "enabled INTEGER DEFAULT 1");
            ensureColumn(db, "kb_config", "memory_document_id", "memory_document_id TEXT DEFAULT ''");
            ensureColumn(db, "llm_config", "reasoning_effort", "reasoning_effort TEXT DEFAULT ''");
            ensureColumn(db, "llm_usage", "role", "role TEXT NOT NULL DEFAULT ''");

            // long_term_memory → short_term_memory
            // 注：表可能刚被上面创建为空表，需先检查旧表是否有数据
            if (tableExists(db, "long_term_memory")) {
                int64_t oldCount = 0;
                if (const Statement stmt(db, "SELECT COUNT(*) FROM long_term_memory"); stmt.step()) {
                    oldCount = stmt.getInt64(0);
                }
                if (oldCount > 0) {
                    if (tableExists(db, "short_term_memory")) {
                        execSQL(db, "DROP TABLE short_term_memory");
                    }
                    Logger::info(0, "Storage",
                      fmt::format("数据库迁移: 重命名 long_term_memory → short_term_memory ({} 条数据)", oldCount));
                    execSQL(db, "ALTER TABLE long_term_memory RENAME TO short_term_memory");
                } else {
                    Logger::info(0, "Storage", fmt::format("数据库迁移: 删除空的 long_term_memory 表"));
                    execSQL(db, "DROP TABLE long_term_memory");
                }
            }

            // 上下文窗口配置（窗口触发条数/保留条数/提取 maxTokens）
            ensureColumn(db, "memory_config", "window_trigger_count", "window_trigger_count INTEGER DEFAULT 100");
            ensureColumn(db, "memory_config", "window_keep_count", "window_keep_count INTEGER DEFAULT 50");
            ensureColumn(
              db, "memory_config", "memory_extract_max_tokens", "memory_extract_max_tokens INTEGER DEFAULT 4000");
            ensureColumn(
              db, "memory_config", "router_window_trigger_count", "router_window_trigger_count INTEGER DEFAULT 20");
            ensureColumn(
              db, "memory_config", "router_window_keep_count", "router_window_keep_count INTEGER DEFAULT 10");
            ensureColumn(db, "memory_config", "short_term_memory_max", "short_term_memory_max INTEGER DEFAULT 15");
            ensureColumn(db, "memory_config", "memory_migrate_count", "memory_migrate_count INTEGER DEFAULT 5");
            ensureColumn(db, "short_term_memory", "watermark_id", "watermark_id INTEGER DEFAULT 0");
            dropColumnIfExists(db, "memory_config", "memory_trigger_count");
            dropColumnIfExists(db, "memory_config", "memory_chat_record_limit");
            dropColumnIfExists(db, "memory_config", "executor_context_limit");
            dropColumnIfExists(db, "memory_config", "short_term_memory_limit");

            // 老群已有短期记忆：水位线初始化为该群最新记录 id，
            // 避免升级后首次触发时把全部历史重新提取一遍（历史已浓缩在现有记忆中）
            execSQL(db,
              "UPDATE short_term_memory SET watermark_id = "
              "(SELECT COALESCE(MAX(id), 0) FROM chat_records c WHERE c.group_id = short_term_memory.group_id) "
              "WHERE watermark_id = 0 "
              "AND EXISTS (SELECT 1 FROM chat_records c WHERE c.group_id = short_term_memory.group_id)");

            // emojis 表已废弃（表情包改为直接使用 QQ 收藏表情，不再存库）
            if (tableExists(db, "emojis")) {
                Logger::info(0, "Storage", fmt::format("数据库迁移: 删除废弃的 emojis 表"));
                execSQL(db, "DROP TABLE emojis");
            }
        }

        void migrateV1ToV2(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: kb_config/memory_config/qq_config 并入 settings"));

            json kb;
            {
                const Statement stmt(db, "SELECT enabled, api_key, base_url, knowledge_dataset_id, memory_dataset_id, "
                                         "memory_document_id FROM kb_config WHERE id = 1");
                if (stmt.step()) {
                    kb["enabled"] = stmt.getInt(0) != 0;
                    kb["apiKey"] = stmt.getText(1);
                    kb["baseUrl"] = stmt.getText(2);
                    kb["knowledgeDatasetId"] = stmt.getText(3);
                    kb["memoryDatasetId"] = stmt.getText(4);
                    kb["memoryDocumentId"] = stmt.getText(5);
                } else {
                    kb["enabled"] = true;
                    kb["apiKey"] = "";
                    kb["baseUrl"] = "";
                    kb["knowledgeDatasetId"] = "";
                    kb["memoryDatasetId"] = "";
                    kb["memoryDocumentId"] = "";
                }
            }
            storeSetting(db, "kb_config", kb);

            json memory;
            {
                const Statement stmt(db,
                  "SELECT window_trigger_count, window_keep_count, memory_extract_max_tokens, "
                  "router_window_trigger_count, router_window_keep_count, short_term_memory_max, "
                  "memory_migrate_count FROM memory_config WHERE id = 1");
                if (stmt.step()) {
                    memory["windowTriggerCount"] = stmt.getInt(0);
                    memory["windowKeepCount"] = stmt.getInt(1);
                    memory["memoryExtractMaxTokens"] = stmt.getInt(2);
                    memory["routerWindowTriggerCount"] = stmt.getInt(3);
                    memory["routerWindowKeepCount"] = stmt.getInt(4);
                    memory["shortTermMemoryMax"] = stmt.getInt(5);
                    memory["memoryMigrateCount"] = stmt.getInt(6);
                } else {
                    memory["windowTriggerCount"] = 100;
                    memory["windowKeepCount"] = 50;
                    memory["memoryExtractMaxTokens"] = 4000;
                    memory["routerWindowTriggerCount"] = 20;
                    memory["routerWindowKeepCount"] = 10;
                    memory["shortTermMemoryMax"] = 15;
                    memory["memoryMigrateCount"] = 5;
                }
            }
            storeSetting(db, "memory_config", memory);

            json qq;
            {
                const Statement stmt(
                  db, "SELECT access_token, self_qq_number, qq_http_host, bot_name FROM qq_config WHERE id = 1");
                if (stmt.step()) {
                    qq["accessToken"] = stmt.getText(0);
                    qq["selfQQNumber"] = stmt.getInt64(1);
                    qq["qqHttpHost"] = stmt.getText(2);
                    qq["botName"] = stmt.getText(3);
                } else {
                    qq["accessToken"] = "";
                    qq["selfQQNumber"] = 0;
                    qq["qqHttpHost"] = "http://127.0.0.1:3000";
                    qq["botName"] = "小喵";
                }
            }
            storeSetting(db, "qq_config", qq);

            execSQL(db, "DROP TABLE kb_config");
            execSQL(db, "DROP TABLE memory_config");
            execSQL(db, "DROP TABLE qq_config");
        }

        void migrateV2ToV3(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: 新增 group_affinity 好感度表"));
            execAll(db, v3Tables);
        }

        void migrateV3ToV4(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: scheduled_tasks 状态扩展（新增 cancelled）"));
            // SQLite 无法直接修改 CHECK 约束，需重建表
            execSQL(db, R"(CREATE TABLE scheduled_tasks_new (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            session_type TEXT NOT NULL CHECK(session_type IN ('group', 'private')),
            target_id INTEGER NOT NULL,
            remind_time INTEGER NOT NULL,
            content TEXT NOT NULL,
            status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending', 'done', 'cancelled')),
            created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
        ))");
            execSQL(db,
              "INSERT INTO scheduled_tasks_new (id, session_type, target_id, remind_time, content, status, created_at) "
              "SELECT id, session_type, target_id, remind_time, content, status, created_at FROM scheduled_tasks");
            execSQL(db, "DROP TABLE scheduled_tasks");
            execSQL(db, "ALTER TABLE scheduled_tasks_new RENAME TO scheduled_tasks");
            execSQL(
              db, "CREATE INDEX IF NOT EXISTS idx_scheduled_tasks_status ON scheduled_tasks(status, remind_time)");
        }

        void migrateV4ToV5(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: scheduled_tasks 新增 is_daily（每日重复任务）"));
            // 只加一列，ALTER TABLE 即可，无需像 v4 那样重建表
            ensureColumn(
              db, "scheduled_tasks", "is_daily", "is_daily INTEGER NOT NULL DEFAULT 0 CHECK(is_daily IN (0, 1))");
        }

        void migrateV5ToV6(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: 新增 long_term_memory 长期记忆表"));
            execAll(db, v6Tables);
            // RAGFlow 已移除，settings 中的知识库配置一并清理
            execSQL(db, "DELETE FROM settings WHERE key = 'kb_config'");
        }

        void migrateV6ToV7(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: 新增图片描述缓存表"));
            execAll(db, v7Tables);
            ensureColumn(
              db, "image_description_cache", "sampled_frame_count", "sampled_frame_count INTEGER NOT NULL DEFAULT 1");
        }

        void migrateV7ToV8(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: 新增持久化记忆维护任务队列并移除旧记忆水位线"));
            execAll(db, v8Tables);
            dropColumnIfExists(db, "short_term_memory", "watermark_id");
        }

        void migrateV8ToV9(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: 记忆任务保留总结上下文与完成删除状态"));
            ensureColumn(
              db, "memory_maintenance_jobs", "context_messages", "context_messages TEXT NOT NULL DEFAULT '[]'");
            ensureColumn(db, "memory_maintenance_jobs", "remove_count", "remove_count INTEGER NOT NULL DEFAULT 0");
            ensureColumn(db, "memory_maintenance_jobs", "status",
              "status TEXT NOT NULL DEFAULT 'pending' CHECK(status IN ('pending', 'completed'))");
        }

        void migrateV9ToV10(sqlite3 *db) {
            Logger::info(0, "Storage", fmt::format("执行迁移: 新增独立的好感度维护任务队列"));
            execAll(db, v10Tables);
            execSQL(db, "INSERT INTO affinity_maintenance_jobs (session_id, messages) "
                        "SELECT session_id, messages FROM memory_maintenance_jobs WHERE status = 'pending'");
        }
    } // namespace

    namespace SchemaMigrator {
        void migrate(sqlite3 *db) {
            const int version = getUserVersion(db);

            if (version == 0 && !hasUserTables(db)) {
                Logger::info(0, "Storage", fmt::format("检测到全新数据库，直接创建最新 Schema (v{})", kLatestVersion));
                createFreshSchema(db);
                return;
            }

            if (version >= kLatestVersion)
                return;

            Logger::info(0, "Storage", fmt::format("数据库版本 v{}，开始迁移到 v{}", version, kLatestVersion));

            // 迁移步骤表：steps[i] 将版本 i 升级到版本 i+1
            constexpr std::array steps = {
              &migrateV0ToV1, // 基线：历史遗留 Schema 调整到统一基线
              &migrateV1ToV2, // kb_config/memory_config/qq_config 并入 settings
              &migrateV2ToV3, // 新增 group_affinity 好感度表
              &migrateV3ToV4, // scheduled_tasks 状态扩展（新增 cancelled）
              &migrateV4ToV5, // scheduled_tasks 新增 is_daily（每日重复任务）
              &migrateV5ToV6, // 新增 long_term_memory 长期记忆表
              &migrateV6ToV7, // 新增图片描述缓存表
              &migrateV7ToV8, // 新增持久化记忆维护任务队列
              &migrateV8ToV9, // 记忆任务完成后再删除消息列表前缀
              &migrateV9ToV10, // 好感度维护任务与记忆维护任务解耦
            };

            for (int v = version; v < kLatestVersion; ++v) {
                char *errMsg = nullptr;
                if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, &errMsg) != SQLITE_OK) {
                    std::string err = errMsg ? errMsg : "BEGIN failed";
                    sqlite3_free(errMsg);
                    throw DbError(err);
                }
                try {
                    steps[v](db);
                    setUserVersion(db, v + 1);
                    if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, &errMsg) != SQLITE_OK) {
                        const std::string err = errMsg ? errMsg : "COMMIT failed";
                        sqlite3_free(errMsg);
                        throw DbError(err);
                    }
                } catch (...) {
                    sqlite3_exec(db, "ROLLBACK", nullptr, nullptr, nullptr);
                    Logger::error(0, "Storage", fmt::format("数据库迁移 v{} -> v{} 失败，已回滚", v, v + 1));
                    throw;
                }
                Logger::info(0, "Storage", fmt::format("数据库迁移完成: v{} -> v{}", v, v + 1));
            }
        }

    } // namespace SchemaMigrator
} // namespace insoulforge
