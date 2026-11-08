/// @file ActivityController.hpp
/// @brief Activity 配置管理入口，由管理员会话保护；活动通信使用独立 WebSocket。
#pragma once
#include <drogon/HttpController.h>

namespace insoulforge {
    class ActivityController : public drogon::HttpController<ActivityController> {
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(ActivityController::getConfig, "/admin/api/activities", drogon::Get);
        ADD_METHOD_TO(ActivityController::saveConfig, "/admin/api/activities", drogon::Post);
        ADD_METHOD_TO(ActivityController::setEnabled, "/admin/api/activities", drogon::Patch);
        ADD_METHOD_TO(ActivityController::removeService, "/admin/api/activities", drogon::Delete);
        ADD_METHOD_TO(
          ActivityController::uploadFile, "/activities/files", drogon::Post, "insoulforge::ActivityAuthentication");
        METHOD_LIST_END
        void uploadFile(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
        void getConfig(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
        void saveConfig(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
        void setEnabled(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
        void removeService(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
    };
} // namespace insoulforge
