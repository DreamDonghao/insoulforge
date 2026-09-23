/// @file LongTermMemoryStore.cpp
/// @brief 长期记忆存储 - 实现
/// @author donghao
/// @date 2026-09-01


#include <infrastructure/NumericTypes.hpp>

#include <agent/memory/LongTermMemoryStore.hpp>
#include <infrastructure/storage/Database.hpp>
#include <infrastructure/storage/Statement.hpp>

namespace insoulforge {
    namespace LongTermMemoryStore {
        namespace {
            f32 cosineSimilarity(const std::vector<f32> &a, const std::vector<f32> &b) {
                f32 dot = 0, normA = 0, normB = 0;
                for (size_t i = 0; i < a.size(); ++i) {
                    dot += a[i] * b[i];
                    normA += a[i] * a[i];
                    normB += b[i] * b[i];
                }
                if (normA == 0.0f || normB == 0.0f)
                    return 0.0f;
                return dot / (std::sqrt(normA) * std::sqrt(normB));
            }
        } // namespace

        std::vector<SimilarMemory> searchSimilar(const u64 groupId, const std::vector<f32> &query, const i32 topK) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT id, content, embedding FROM long_term_memory WHERE group_id = ?");
            stmt.bind(1, groupId);

            std::vector<SimilarMemory> scored;
            while (stmt.step()) {
                const auto bytes = stmt.getBlob(2);
                if (bytes.empty() || bytes.size() % sizeof(f32) != 0)
                    continue;

                std::vector<f32> embedding(bytes.size() / sizeof(f32));
                std::memcpy(embedding.data(), bytes.data(), bytes.size());
                // 维度不匹配说明换过 embedding 模型，旧向量不可比，跳过
                if (embedding.size() != query.size())
                    continue;
                scored.push_back({stmt.getInt64(0), stmt.getText(1), cosineSimilarity(query, embedding)});
            }

            const auto byScoreDesc = [](const SimilarMemory &a, const SimilarMemory &b) {
                return a.similarity > b.similarity;
            };
            if (static_cast<size_t>(topK) < scored.size()) {
                std::ranges::partial_sort(scored, scored.begin() + topK, byScoreDesc);
                scored.resize(topK);
            } else {
                std::ranges::sort(scored, byScoreDesc);
            }
            return scored;
        }

        std::vector<LongTermMemoryEntry> listMemories(const u64 sessionId, const i32 limit, const i32 offset) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(),
              "SELECT id, group_id, content, created_at FROM long_term_memory WHERE (? = 0 OR group_id = ?) "
              "ORDER BY id DESC LIMIT ? OFFSET ?");
            stmt.bind(1, sessionId);
            stmt.bind(2, sessionId);
            stmt.bind(3, limit);
            stmt.bind(4, offset);

            std::vector<LongTermMemoryEntry> entries;
            while (stmt.step()) {
                entries.push_back(
                  {stmt.getInt64(0), static_cast<u64>(stmt.getInt64(1)), stmt.getText(2), stmt.getText(3)});
            }
            return entries;
        }

        i64 countMemories(const u64 sessionId) {
            const auto &db = Database::instance();
            std::shared_lock lock(db.mutex());
            const Statement stmt(db.handle(), "SELECT COUNT(*) FROM long_term_memory WHERE (? = 0 OR group_id = ?)");
            stmt.bind(1, sessionId);
            stmt.bind(2, sessionId);
            return stmt.step() ? stmt.getInt64(0) : 0;
        }

        bool deleteMemory(const i64 id) {
            const auto &db = Database::instance();
            std::unique_lock lock(db.mutex());
            const Statement stmt(db.handle(), "DELETE FROM long_term_memory WHERE id = ?");
            stmt.bind(1, id);
            stmt.exec();
            return Statement::changes(db.handle()) > 0;
        }
    } // namespace LongTermMemoryStore
} // namespace insoulforge
