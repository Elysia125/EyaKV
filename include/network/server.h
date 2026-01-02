#ifndef SERVER_H
#define SERVER_H

#include <memory>
#include <thread>
#include <vector>
#include "storage/storage.h"
#include "common/export.h"

class Connection; // Forward declaration

/**
 * @brief Server 负责监听端口并接受连接
 */
class EYAKV_NETWORK_API Server
{
public:
    Server(Storage *storage, unsigned short port);
    ~Server();

    /**
     * @brief 启动服务器（阻塞）。
     */
    void Run();

    /**
     * @brief 停止服务器。
     */
    void Stop();

private:
    void DoAccept();

    Storage *storage_; // 引用，不拥有所有权
};

#endif // SERVER_H