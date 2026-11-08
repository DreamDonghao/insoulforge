/// @file ExecutorSystemPrompt.hpp
/// @brief Executor 系统提示词构建和公共内容缓存。
/// @details 群聊与私聊分别缓存公共内容：人设、固定规则、MCP 目录、活动目录和表情名称。
/// 群附加设定不进入公共缓存，每次构建时读取并追加到完整 system 的最后。
/// 最终 system 依次包含：
/// 1. 项目信息。
/// 2. 群聊或私聊人设。
/// 3. 聊天记录格式与工具调用规则，包括会话类型差异及可选好感度说明。
/// 4. MCP 与 Activity 目录及操作说明。
/// 5. 可用表情包名称，空数组也保留。
/// 6. 本群附加设定，仅群聊且非空时追加到最后。
/// 实际聊天记录、记忆、活动运行状态及每轮 execution_state 位于 user 上下文，不属于 system。
#pragma once

#include <array>
#include <map>
#include <mutex>

#include <expected>

#include <infrastructure/JsonUtil.hpp>
#include <infrastructure/SessionId.hpp>

/// @brief 组装单条 system 消息的文本，不负责 user 上下文或工具消息。
#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    class ExecutorSystemPrompt {
    public:
        enum class PartKind { Project, Persona, Structure, Services, Stickers, GroupSetting, Count };
        static constexpr auto kPartCount = static_cast<std::size_t>(PartKind::Count);
        static constexpr auto kPublicPartCount = static_cast<std::size_t>(PartKind::GroupSetting);
        /// @brief 六个部分按 system 拼接顺序排列；失败通过 expected 保留，不冒充零用量。
        struct Part {
            std::string text;
            std::expected<u32, std::string> tokens = std::unexpected("尚未计算");
        };
        struct Snapshot {
            std::string text;
            std::array<Part, kPartCount> parts;
        };
        /// @brief 稳定分项标识、名称和统计范围，按 system 拼接顺序排列。
        struct PartInfo {
            std::string_view id, name, description;
        };
        static constexpr std::array<PartInfo, kPartCount> partInfo{{{"project", "项目说明", "项目身份介绍及仓库地址。"},
          {"persona", "人设与行为设定", "后台配置的群聊或私聊提示词，包含角色、语气及行为要求。"},
          {"structure", "上下文结构与工具规则",
            "上下文 JSON 与聊天记录字段说明、工具使用规则及会话类型和好感度说明；不含实际聊天记录或工具调用历史。"},
          {"services", "MCP 与 Activity 服务说明",
            "MCP 服务与 Activity 的名称、描述及使用说明；不含完整工具定义和活动运行状态。"},
          {"stickers", "可用表情名称列表", "可用收藏表情的名称及说明文本，不含表情图片。"},
          {"group-setting", "群聊附加设定", "当前群单独配置的附加提示词及其说明文本。"}}};
        /// @brief 返回完整文本和六部分 Token 估算；仅变化的部分重新分词。
        /// @note 各部分之和可能与完整文本分词结果不同；不含 user 或工具定义开销。
        /// @param includeGroupSetting 为 false 时仅返回公共部分，不读取或更新群附加设定缓存。
        auto buildSnapshot(const SessionId &sessionId, bool includeGroupSetting = true) -> Snapshot;
        /// @brief 获取进程唯一实例，首次初始化支持并发。
        static auto instance() -> ExecutorSystemPrompt &;
        ExecutorSystemPrompt(const ExecutorSystemPrompt &) = delete;
        auto operator=(const ExecutorSystemPrompt &) -> ExecutorSystemPrompt & = delete;

        /// @brief 同时使群聊、私聊公共缓存失效，下次 build() 时按需重建。
        /// @details 用于提示词等不能由输入比较发现的内容更新；本函数不读取数据源或立即重建。
        /// @note 可并发调用；已经返回的字符串快照不受影响。
        void invalidate();

        /// @brief MCP 配置生效后更新目录片段，使两份公共缓存失效；不协调进行中的工具调用。
        void updateMcpDirectory(const json &directory);

        /// @brief 收藏表情成功刷新后更新名称数组；名称变化时使两份公共缓存失效。
        void updateStickerNames(json names);

        /// @brief 获取公共内容快照，最后追加当前群附加设定。
        /// @param sessionId 本次回复所属会话，决定使用群聊或私聊缓存，并用于查询群附加设定。
        /// @return 独立持有的完整 system 文本；私聊不追加群附加设定。
        /// @details 首次调用、缓存失效或相关输入变化时重新构建，否则复用公共内容。
        /// 当前每次调用读取活动目录，并比较机器人名称及好感度开关。
        /// 数据库中的人设提示词只在重建时读取，修改后需调用 invalidate()。
        /// @note 可并发调用。调用方应在一次回复开始时构建一次，并在后续工具轮次和兜底请求中
        /// 复用返回值；本模块不自动维护一次回复的生命周期，也不将数据源异常转换为错误返回值。
        auto build(const SessionId &sessionId) -> std::string;
        /// @brief 查看群聊或私聊公共部分，不构造虚拟会话，也不读取群设定。
        auto buildPublicSnapshot(bool isPrivateSession) -> Snapshot;

    private:
        ExecutorSystemPrompt() = default;
        auto assemble(bool isPrivateSession, const json &activities) -> std::array<std::string, kPublicPartCount>;
        auto makeSnapshot(bool isPrivateSession, const SessionId *groupSession, bool includeParts) -> Snapshot;
        struct CachedPart : Part {
            bool initialized = false;
        };
        static void updatePart(CachedPart &part, std::string text);
        struct Cache {
            std::size_t version = static_cast<std::size_t>(-1);
            std::size_t personaGeneration = static_cast<std::size_t>(-1);
            std::string persona;
            bool affinity{};
            std::string botName;
            json activities;
            std::array<CachedPart, kPublicPartCount> parts;
        };
        std::map<std::string, CachedPart> m_groupParts;
        std::mutex m_mutex;
        std::size_t m_generation = 0;
        std::size_t m_personaGeneration = 0;
        std::array<Cache, 2> m_caches;
        std::string m_mcpSection;
        json m_stickerNames = json::array();
    };
} // namespace insoulforge
