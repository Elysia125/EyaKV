#include "network/server.h"
#include <iostream>

Server::Server(Storage *storage, unsigned short port) : storage_(storage)
{
}

Server::~Server()
{
    Stop();
}

void Server::Run()
{
    DoAccept();
}

void Server::Stop()
{
}

void Server::DoAccept()
{
}