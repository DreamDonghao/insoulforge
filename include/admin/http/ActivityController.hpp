/// @file ActivityController.hpp
/// @brief 实时活动配置和独立服务回调入口；回调不使用管理员会话凭据。
#pragma once
#include <drogon/HttpController.h>

namespace insoulforge {
    class ActivityController : public drogon::HttpController<ActivityController> {
    public:
        METHOD_LIST_BEGIN
        ADD_METHOD_TO(ActivityController::getConfig, "/admin/api/activities", drogon::Get);
        ADD_METHOD_TO(ActivityController::saveConfig, "/admin/api/activities", drogon::Post);
        ADD_METHOD_TO(ActivityController::event, "/activities/events", drogon::Post);
        METHOD_LIST_END
        void getConfig(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
        void saveConfig(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
        void event(
          const drogon::HttpRequestPtr &req, std::function<void(const drogon::HttpResponsePtr &)> &&callback) const;
    };
} // namespace insoulforge
