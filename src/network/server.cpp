#include "network/server.h"
#include "network/connection.h"
#include <iostream>

Server::Server(Storage *storage, unsigned short port)
        : io_context_(),
          acceptor_(io_context_, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), port)),
          storage_(storage)
    {
    }

    Server::~Server()
    {
        Stop();
    }

    void Server::Run()
    {
        std::cout << "Server starting on port " << acceptor_.local_endpoint().port() << "..." << std::endl;
        DoAccept();
        io_context_.run();
    }

    void Server::Stop()
    {
        io_context_.stop();
    }

    void Server::DoAccept()
    {
        acceptor_.async_accept(
            [this](std::error_code ec, asio::ip::tcp::socket socket)
            {
                if (!ec)
                {
                    std::make_shared<Connection>(std::move(socket), storage_)->Start();
                }
                else
                {
                    std::cerr << "Accept error: " << ec.message() << std::endl;
                }

                // Continue accepting
                DoAccept();
            });
    }