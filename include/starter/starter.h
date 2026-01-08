#ifndef STARTER_H
#define STARTER_H

#include "common/base/export.h"
#include <atomic>
class Storage;
class EyaServer;

/**
 * 启动器
 */
class EYAKV_STARTER_API EyaKVStarter
{
private:
    static Storage *storage;
    static EyaServer *server;
    static std::atomic<bool> should_shutdown;

    EyaKVStarter() = default;
    static void print_banner();

    static void initialize();

    static void initialize_logger();

    static Storage *initialize_storage();

    static void initialize_server();

    static void register_signal_handlers();

public:
    /**
     * 启动
     */
    static void start();

    /**
     * 优雅关闭
     *
     * 停止服务器、清理存储资源并退出
     */
    static void shutdown();
};

#endif // STARTER_H