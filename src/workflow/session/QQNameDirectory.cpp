/// @file QQNameDirectory.cpp
/// @brief 运行时用户昵称目录实现

#include <shared_mutex>

#include <infrastructure/NumericTypes.hpp>
#include <infrastructure/UserId.hpp>
#include <workflow/session/QQNameDirectory.hpp>

namespace insoulforge::QQNameDirectory {
    namespace {
        std::shared_mutex nameMutex;
        std::unordered_map<UserId, std::string> observedNames;
        std::unordered_map<UserId, std::string> customNames;
    } // namespace

    void recordName(const UserId &userId, std::string name) {
        if (name.empty()) {
            return;
        }
        std::unique_lock lock(nameMutex);
        if (!customNames.contains(userId)) {
            observedNames[userId] = std::move(name);
        }
    }

    void setCustomName(const UserId &userId, std::string name) {
        if (name.empty()) {
            return;
        }
        std::unique_lock lock(nameMutex);
        customNames[userId] = std::move(name);
    }

    auto getName(const UserId &userId) -> std::string {
        std::shared_lock lock(nameMutex);
        if (const auto custom = customNames.find(userId); custom != customNames.end()) {
            return custom->second;
        }
        if (const auto observed = observedNames.find(userId); observed != observedNames.end()) {
            return observed->second;
        }
        return "未知";
    }

    auto nameToQQMap() -> std::unordered_map<std::string, UserId> {
        std::shared_lock lock(nameMutex);
        std::unordered_map<std::string, UserId> names;
        names.reserve(observedNames.size() + customNames.size());
        for (const auto &[userId, name]: observedNames) {
            names.insert_or_assign(name, userId);
        }
        for (const auto &[userId, name]: customNames) {
            names.insert_or_assign(name, userId);
        }
        return names;
    }
} // namespace insoulforge::QQNameDirectory
