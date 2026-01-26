#ifndef COMMON_SOCKET_CS_COMMON_H
#define COMMON_SOCKET_CS_COMMON_H

#include "common/socket/socket.h"
#include "common/socket/protocol/protocol.h"

class TCPBase
{
protected:
    std::string ip_;
    u_short port_;

    virtual std::string build_header(uint32_t body_length)
    {
        ProtocolHeader header(body_length);
        return header.serialize();
    }

    virtual ProtocolHeader build_header(const std::string &s)
    {
        ProtocolHeader header;
        size_t offset = 0;
        header.deserialize(s.c_str(), offset);
        return header;
    }

    TCPBase(const std::string &ip, u_short port)
        : ip_(ip), port_(port)

    {
    }

    virtual int send(const ProtocolBody &body, socket_t socket)
    {
        std::string sbody = body.serialize();
        std::string request = build_header(sbody.length()) + sbody;
        return send_data(socket, request);
    }

    virtual int receive(ProtocolBody &body, socket_t socket, uint32_t timeout = 0)
    {
        std::string sheader;
        int result = receive_data(socket, sheader, ProtocolHeader::PROTOCOL_HEADER_SIZE, timeout);
        if (result != ProtocolHeader::PROTOCOL_HEADER_SIZE)
        {
            return result;
        }
        auto header = build_header(sheader);
        std::string sbody;
        result = receive_data(socket, sbody, header.length, timeout);
        if (result != header.length)
        {
            return result;
        }
        size_t offset = 0;
        body.deserialize(sbody.c_str(), offset);
        return result;
    }
};

#endif