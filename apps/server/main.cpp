#include <iostream>
#ifdef __linux__
#include <sys/resource.h>
#endif

#include "starter/starter.h"

int main(int argc, char **argv)
{
    std::cout << "EyaKV 0.1.0 starting..." << std::endl;
#ifdef __linux__
    struct rlimit rl;
    getrlimit(RLIMIT_NOFILE, &rl);
    std::cout << "当前进程的文件描述符软限制: " << rl.rlim_cur << ", 硬限制: " << rl.rlim_max << std::endl;
#endif
    EyaKVStarter::start();
    return 0;
}