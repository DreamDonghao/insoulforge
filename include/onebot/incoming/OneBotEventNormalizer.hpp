/// @file OneBotEventNormalizer.hpp
/// @brief OneBot 上报事件到统一消息记录的转换

#pragma once

#include <optional>

#include <infrastructure/JsonUtil.hpp>

namespace insoulforge::OneBotEventNormalizer {
    /// @brief 将一个支持的 OneBot 上报转换为统一消息记录
    /// @param body 已通过 HTTP JSON 校验的 OneBot 上报对象
    /// @return 统一消息记录；不支持或缺少必要字段的上报返回空值
    /// @details 转换时会记录上报中的发送者昵称，但不请求 OneBot API，也不进行图片识别、
    ///          命令识别或路由判断。`segments` 是内容和顺序的唯一来源，图片传输来源仅写入
    ///          `assets.images`。结果还包含仅供工作流使用的 `session_id`，用于后续会话配置、记录
    ///          和并发控制。机器人自身的结构化身份统一为 self；self_id 缺失时使用已配置的机器人账号，
    ///          两者均不可用时保留原身份，不猜测替换。
    [[nodiscard]] auto normalize(const json& body) -> std::optional<json>;
} // namespace insoulforge::OneBotEventNormalizer
