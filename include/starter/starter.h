#ifndef EYAKV_STARTER_H
#define EYAKV_STARTER_H

#include "common/base/export.h"
#include <atomic>
#include <thread>
#include <memory>

class Storage;
class EyaServer;

/**
 * @brief EyaKV 服务启动器
 * * 负责整个系统的初始化、模块按序启动以及监听系统信号进行优雅停机。
 * 采用全静态方法设计，管理全局核心组件的生命周期。
 */
class EYAKV_STARTER_API EyaKVStarter
{
public:
    /**
     * @brief 启动 EyaKV 服务
     * * 将依次初始化环境、日志、存储引擎、Raft 共识模块和网络服务器。
     * 若发生致命错误，将捕获异常并自动触发安全关闭。
     */
    static void start();

    /**
     * @brief 优雅关闭服务
     * * 停止服务器接收新请求、安全停用 Raft 节点、清理存储资源并退出进程。
     * 该方法是线程安全的，多次调用只会执行一次。
     */
    static void shutdown();

private:
    // 禁用构造与拷贝，纯静态工具类
    EyaKVStarter() = default;
    ~EyaKVStarter() = default;
    EyaKVStarter(const EyaKVStarter&) = delete;
    EyaKVStarter& operator=(const EyaKVStarter&) = delete;

    /** @brief 打印终端 ASCII 艺术字 Banner */
    static void print_banner();

    /** @brief 串联执行所有的初始化步骤 */
    static void initialize();

    /** @brief 初始化日志系统 */
    static void initialize_logger();

    /** @brief 初始化存储引擎 (Storage) */
    static void initialize_storage();

    /** @brief 初始化 Raft 共识模块 */
    static void initialize_raft();

    /** @brief 初始化并启动 TCP 网络服务器 */
    static void initialize_server();

    /** @brief 注册跨平台的系统信号 (SIGINT, SIGTERM) 处理器 */
    static void register_signal_handlers();

    /** @brief 后台守护线程，专门用于安全监控退出信号 */
    static void background_watcher_thread();

private:
    // 核心组件指针与状态标记 (统一加下划线后缀)
    static std::unique_ptr<EyaServer> server_;           ///< 网络服务器实例
    static std::unique_ptr<std::thread> raft_thread_;    ///< Raft 运行线程
    static std::atomic<bool> should_shutdown_;           ///< 标志是否收到关闭信号
    static std::atomic<bool> has_shutdown_;              ///< 标志是否已经执行过关闭流程
};

#endif // EYAKV_STARTER_H