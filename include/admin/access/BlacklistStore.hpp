/// @file BlacklistStore.hpp
/// @brief 全局 QQ 黑名单持久化接口

#pragma once

#include <vector>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge::BlacklistStore {
    /// @brief 判断 QQ 是否在全局黑名单中。
    [[nodiscard]] auto contains(u64 qqNumber) -> bool;

    /// @brief 将 QQ 加入全局黑名单；已存在时保持不变。
    void add(u64 qqNumber);

    /// @brief 将 QQ 移出全局黑名单；不存在时保持不变。
    void remove(u64 qqNumber);

    /// @brief 获取全部黑名单 QQ 号。
    [[nodiscard]] auto getAll() -> std::vector<u64>;
} // namespace insoulforge::BlacklistStore
