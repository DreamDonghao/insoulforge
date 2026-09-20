/// @file QQNameDirectory.cpp
/// @brief 运行时 QQ 昵称目录实现

#include <conversation/session/QQNameDirectory.hpp>

#include <shared_mutex>

namespace insoulforge::QQNameDirectory {
    namespace {
        std::shared_mutex nameMutex;
        std::unordered_map<uint64_t, std::string> observedNames;
        std::unordered_map<uint64_t, std::string> customNames;
    } // namespace

    void recordName(const uint64_t qqNumber, std::string name) {
        if (qqNumber == 0 || name.empty()) {
            return;
        }
        std::unique_lock lock(nameMutex);
        if (!customNames.contains(qqNumber)) {
            observedNames[qqNumber] = std::move(name);
        }
    }

    void setCustomName(const uint64_t qqNumber, std::string name) {
        if (qqNumber == 0 || name.empty()) {
            return;
        }
        std::unique_lock lock(nameMutex);
        customNames[qqNumber] = std::move(name);
    }

    std::string getName(const uint64_t qqNumber) {
        std::shared_lock lock(nameMutex);
        if (const auto custom = customNames.find(qqNumber); custom != customNames.end()) {
            return custom->second;
        }
        if (const auto observed = observedNames.find(qqNumber); observed != observedNames.end()) {
            return observed->second;
        }
        return "未知";
    }

    std::unordered_map<std::string, uint64_t> nameToQQMap() {
        std::shared_lock lock(nameMutex);
        std::unordered_map<std::string, uint64_t> names;
        names.reserve(observedNames.size() + customNames.size());
        for (const auto &[qqNumber, name]: observedNames) {
            names[name] = qqNumber;
        }
        for (const auto &[qqNumber, name]: customNames) {
            names[name] = qqNumber;
        }
        return names;
    }
} // namespace insoulforge::QQNameDirectory
