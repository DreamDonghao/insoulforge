/// @file UsageStore.hpp
/// @brief LLM 用量统计存储
/// @author donghao
/// @date 2026-08-30
/// @details 表：llm_usage（每次 LLM 调用的 token 用量明细）

#pragma once

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/NumericTypes.hpp>
#include <string>


/// @brief LLM 用量统计存储
namespace insoulforge::UsageStore {
    /// @brief 记录一次 LLM 调用用量
    void addUsageRecord(const std::string &role, const std::string &model, i32 promptTokens, i32 completionTokens,
      i32 totalTokens, i32 cachedTokens);

    /// @brief 获取最近 N 天用量汇总（按角色、按天聚合）
    [[nodiscard]] json getUsageSummary(i32 days);

    /// @brief 获取最近调用明细
    [[nodiscard]] json getRecentUsage(i32 limit);
} // namespace insoulforge::UsageStore
