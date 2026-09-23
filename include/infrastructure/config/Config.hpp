/// @file Config.hpp
/// @brief 全局配置管理
#pragma once

#include <infrastructure/NumericTypes.hpp>
#include <string>

namespace insoulforge {
    struct LLMApiConfig {
        std::string apiKey;
        std::string baseUrl;
        std::string path;
        std::string model;
        std::string reasoningEffort; // "none"/"medium"/"high"，空串表示不发送
    };

    struct LLMModelParams {
        i32 maxTokens = 1024;
        // 用 f64 保证 JSON 序列化输出 0.7 而非 0.699999988079071
        f64 temperature = 0.7;
        f64 topP = 0.9;
    };

    class Config {
    public:
        // Agent 配置
        LLMApiConfig router;
        LLMModelParams routerParams;
        LLMApiConfig executor;
        LLMModelParams executorParams;
        LLMApiConfig executorThinking; // Executor 思考模型配置
        LLMModelParams executorThinkingParams;
        LLMApiConfig image;
        LLMModelParams imageParams;
        LLMApiConfig embedding; // Embedding 模型配置（长期记忆向量化）

        // 记忆配置
        i32 contextWindowLimit = 100; // Router、Executor 实际可见的最近消息上限
        i32 memorySummaryTriggerCount = 100; // 达到该条数时创建一批记忆总结任务
        i32 memorySummaryBatchSize = 50; // 每批实际总结并在成功后删除的最旧消息数
        i32 memorySummaryContextCount = 10; // 仅供总结理解上下文、不参与提取的后续消息数
        i32 memoryExtractMaxTokens = 4000; // 记忆提取 LLM 调用的 maxTokens
        i32 routerWindowTriggerCount = 20; // Router 子窗口触发条数（批量滑动）
        i32 routerWindowKeepCount = 10; // Router 子窗口保留条数
        i32 shortTermMemoryMax = 15;
        f64 longTermRecallThreshold = 0.65; // 长期记忆召回合并的相似度阈值（独立于 recall_memory 的 0.3）
        f64 longTermInjectThreshold = 0.45; // 长期记忆被动注入提示词的相似度阈值（消息入库时逐条召回）

        // QQ Bot 配置
        std::string accessToken;
        u64 selfQQNumber = 0;
        std::string oneBotTransport = "http";
        std::string qqHttpHost;
        std::string qqWebSocketHost;
        std::string botName{"机器人"};

        static Config &instance();

        /// @brief 从全局配置文件加载运行时配置。
        void loadFromStorage();

    private:
        Config() = default;
    };
} // namespace insoulforge
