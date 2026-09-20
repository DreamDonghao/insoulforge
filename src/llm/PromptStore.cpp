/// @file PromptStore.cpp
/// @brief 提示词存储 - 实现
/// @author donghao
/// @date 2026-08-30

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <llm/PromptStore.hpp>

namespace insoulforge {
    namespace PromptStore {
        std::string getPrompt(const std::string &key, const std::string &defaultValue) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT prompt_content FROM prompts WHERE prompt_key = ?");
            stmt.bind(1, key);
            return stmt.step() ? stmt.getText(0) : defaultValue;
        }

        void setPrompt(const std::string &key, const std::string &content, const std::string &description) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(
              db.handle(), "INSERT OR REPLACE INTO prompts (prompt_key, prompt_content, description) VALUES (?, ?, ?)");
            stmt.bind(1, key);
            stmt.bind(2, content);
            stmt.bind(3, description);
            stmt.exec();
        }

        bool hasPrompt(const std::string &key) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT 1 FROM prompts WHERE prompt_key = ?");
            stmt.bind(1, key);
            return stmt.step();
        }

        std::unordered_map<std::string, std::string> getAllPrompts() {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            std::unordered_map<std::string, std::string> prompts;

            const Statement stmt(db.handle(), "SELECT prompt_key, prompt_content FROM prompts");
            while (stmt.step()) {
                prompts[stmt.getText(0)] = stmt.getText(1);
            }
            return prompts;
        }
    } // namespace PromptStore
} // namespace insoulforge
