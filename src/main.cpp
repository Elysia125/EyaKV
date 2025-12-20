#include <iostream>
#include <thread>
#include "storage/storage.h"
#include "network/server.h"

int main(int argc, char** argv) {
    std::cout << "TinyKV 0.1.0 starting..." << std::endl;

    std::string data_dir = "data";
    unsigned short port = 8080;

    // Initialize Storage
    tinykv::storage::Storage storage(data_dir);

    // Initialize Server
    tinykv::network::Server server(&storage, port);

    // Run Server
    // In a real app, we might want to handle signals (Ctrl+C) to stop gracefully.
    server.Run();

    return 0;
}