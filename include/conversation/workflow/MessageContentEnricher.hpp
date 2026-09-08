/// @file MessageContentEnricher.hpp
/// @brief 统一消息的媒体与长期记忆富化

#pragma once
#include <drogon/utils/coroutine.h>
#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::MessageContentEnricher {
    /// @brief 识别消息内的普通图片和 GIF 动图
    /// @param message 待补充的统一消息 JSON
    /// @param sessionId 所属会话 ID
    /// @details GIF 的抽帧、视觉模型调用与媒体哈希缓存由 ImageDescriptionService 负责。识别失败时只标记
    ///          对应图片资产，不影响消息其余内容。
    /// @return 完成图片识别状态写入后的消息 JSON
    [[nodiscard]] drogon::Task<json> enrichImages(json message, uint64_t sessionId);

    /// @brief 召回长期记忆并写入消息的 `memories` 字段
    /// @param message 已完成图片富化的统一消息 JSON
    /// @param sessionId 所属会话 ID
    /// @details 召回查询使用文本段和成功图片描述。命中结果作为当前消息的完整字段持久化，供后续快照直接使用。
    /// @return 完成记忆注入后的消息 JSON
    [[nodiscard]] drogon::Task<json> injectMemories(json message, uint64_t sessionId);
} // namespace insoulforge::MessageContentEnricher
