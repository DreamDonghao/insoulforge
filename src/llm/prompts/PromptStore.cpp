/// @file PromptStore.cpp
/// @brief 提示词存储 - 实现

#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>
#include <llm/prompts/PromptStore.hpp>


namespace insoulforge::PromptStore {
    auto getPrompt(const Database &db, const std::string &key, const std::string &defaultValue) -> std::string {
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT prompt_content FROM prompts WHERE prompt_key = ?");
        stmt.bind(1, key);
        return stmt.step() ? stmt.getText(0) : defaultValue;
    }

    void setPrompt(
      const Database &db, const std::string &key, const std::string &content, const std::string &description) {
        std::unique_lock lock(db.mutex());
        const Statement stmt(
          db.handle(), "INSERT OR REPLACE INTO prompts (prompt_key, prompt_content, description) VALUES (?, ?, ?)");
        stmt.bind(1, key);
        stmt.bind(2, content);
        stmt.bind(3, description);
        stmt.exec();
    }

    auto hasPrompt(const Database &db, const std::string &key) -> bool {
        std::shared_lock lock(db.mutex());
        const Statement stmt(db.handle(), "SELECT 1 FROM prompts WHERE prompt_key = ?");
        stmt.bind(1, key);
        return stmt.step();
    }

    auto getPrompt(const std::string &key, const std::string &defaultValue) -> std::string {
        return getPrompt(Database::instance(), key, defaultValue);
    }

    void setPrompt(const std::string &key, const std::string &content, const std::string &description) {
        setPrompt(Database::instance(), key, content, description);
    }

    auto hasPrompt(const std::string &key) -> bool { return hasPrompt(Database::instance(), key); }

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
} // namespace insoulforge::PromptStore
