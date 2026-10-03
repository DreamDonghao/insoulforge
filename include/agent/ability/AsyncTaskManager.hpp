/// @file AsyncTaskManager.hpp
/// @brief 按会话限制并交付后台耗时任务

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

#include <drogon/utils/coroutine.h>

#include <infrastructure/NumericTypes.hpp>

namespace insoulforge {
    /// @brief 管理进程内的耗时任务，每个会话同时只允许一个任务运行
    /// @details 工具调用只负责启动任务；任务结果由管理器发送，不占用当前 Executor 回复流程。
    class AsyncTaskManager {
    public:
        /// @brief 返回要直接发送给原会话的内容；抛异常或返回空字符串表示任务失败
        /// @note 处理器在后台协程中运行，不得捕获短生命周期对象的引用。
        using Handler = std::function<drogon::Task<std::string>()>;

        /// @brief 启动请求的结果；Busy 时 taskId 为当前会话正在执行的任务编号
        struct StartResult {
            enum class Status { Started, Busy, Stopping } status;
            std::string taskId;
        };

        /// @brief 获取进程内唯一的任务管理器
        static auto instance() -> AsyncTaskManager &;

        /// @brief 以会话为单位原子检查并启动任务；其他会话不受影响
        /// @param sessionId 统一会话 ID，群号或带私聊标志的用户 QQ 号
        /// @param description 写入上下文的任务描述，不会单独发送给用户
        /// @param handler 后台处理器；返回可由 MessageService 发送的文本或 CQ 码
        /// @return Started 返回新编号；Busy 返回现有编号；Stopping 拒绝启动且编号为空
        /// @throws std::invalid_argument 会话 ID 为 0 或处理器为空
        /// @note 线程安全。启动状态直接写入 MessageList，不经过 Router；结果由后台协程发送。
        [[nodiscard]] auto start(u64 sessionId, std::string description, Handler handler) -> StartResult;

        /// @brief 拒绝新任务，并把尚未完成的任务标记为因退出中断
        /// @note 不等待或恢复处理器；应在消息列表持久化之前调用，且不可与服务继续运行并发使用。
        void stop();

    private:
        AsyncTaskManager() = default;

        auto run(u64 sessionId, std::string taskId, Handler handler) -> drogon::Task<>;
        void release(u64 sessionId, const std::string &taskId);

        std::mutex m_mutex; ///< 保护会话占用和退出状态
        std::unordered_map<u64, std::string> m_active; ///< 会话 ID 到当前任务编号
        bool m_stopping = false;
    };
} // namespace insoulforge
