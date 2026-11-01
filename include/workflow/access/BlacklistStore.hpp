/// @file BlacklistStore.hpp
/// @brief 全局用户黑名单持久化接口

#pragma once

#include <vector>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>

namespace insoulforge::BlacklistStore {
    /// @brief 判断用户是否在全局黑名单中。
    [[nodiscard]] auto contains(const UserId &userId) -> bool;

    /// @brief 将用户加入全局黑名单；已存在时保持不变。
    void add(const UserId &userId);

    /// @brief 将用户移出全局黑名单；不存在时保持不变。
    void remove(const UserId &userId);

    /// @brief 获取全部黑名单 用户 ID。
    [[nodiscard]] auto getAll() -> std::vector<UserId>;
} // namespace insoulforge::BlacklistStore
