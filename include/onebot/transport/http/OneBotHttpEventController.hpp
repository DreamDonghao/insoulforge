/// @file OneBotHttpEventController.hpp
/// @brief OneBot HTTP 事件入口控制器

#pragma once

#include <drogon/HttpController.h>
#include <drogon/utils/coroutine.h>

namespace insoulforge {
    /// @brief OneBot 事件的 HTTP 入口
    /// @details 只负责 OneBot 请求解析与确认，业务处理委托给 OneBotEventWorkflow。
    class OneBotHttpEventController : public drogon::HttpController<OneBotHttpEventController> {
    public:
        OneBotHttpEventController() = default;

        ~OneBotHttpEventController() override = default;

        METHOD_LIST_BEGIN
        ADD_METHOD_TO(OneBotHttpEventController::receiveOneBotEvent, "/", drogon::Post);
        METHOD_LIST_END

        /// @brief 接收并投递 OneBot 上报事件
        /// @param req HTTP 请求，包含 OneBot 协议的 JSON 事件
        /// @param callback HTTP 响应回调
        /// @details 有效请求会先返回成功响应，再将事件投入所属会话的顺序处理队列。
        static auto receiveOneBotEvent(
          drogon::HttpRequestPtr req, std::function<void(const drogon::HttpResponsePtr &)> callback) -> drogon::Task<>;
    };
} // namespace insoulforge
