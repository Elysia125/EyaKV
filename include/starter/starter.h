#ifndef STARTER_H
#define STARTER_H

#include "common/export.h"

class Storage;
/**
 * 启动器
 */
class EYAKV_STARTER_API EyaKVStarter
{
private:
    static Storage*storage;
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
};

#endif // STARTER_H