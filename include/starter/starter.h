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
    void print_banner();

    void initialize();

    void initialize_logger();

    Storage *initialize_storage();

    void initialize_server();

    void register_signal_handlers();

public:
    /**
     * 启动
     */
    void start();

    /**
     * 优雅关闭
     *
     * 停止服务器、清理存储资源并退出
     */
    static void shutdown();

    /**
     * 获取单例实例
     */
    static EyaKVStarter &get_instance()
    {
        static EyaKVStarter instance;
        return instance;
    }
};

#endif // STARTER_H