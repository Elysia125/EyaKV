#include "raft/raft.h"
#include "logger/logger.h"
#include <algorithm>
#include "storage/storage.h"
#include "common/types/operation_type.h"
#include "common/util/path_utils.h"
#include "logger/logger.h"
std::unique_ptr<RaftNode> RaftNode::instance_ = nullptr;
bool RaftNode::is_init_ = false;

RaftNode::RaftNode(const std::string root_dir, const std::string &ip, const u_short port, const uint32_t max_follower_count = 3, const std::string password = "") : TCPServer(ip, port, max_follower_count, 0, 0), password_(password)
{
    std::string meta_path = PathUtils::CombinePath(PathUtils::CombinePath(root_dir, ".raft"), ".raft_meta");
    metadata_file_ = fopen(meta_path.c_str(), "ab+");
    if (metadata_file_ == nullptr)
    {
        LOG_ERROR("Failed to open metadata file: %s ", meta_path);
        throw std::runtime_error("Failed to open metadata file: " + meta_path);
    }
    // TODO 初始化参数和线程

    // TODO 读取文件元信息（比如leader的address）

    if (!leader_address_.load().is_null())
    {
        LOG_INFO("Found leader address: %s", leader_address_.load().to_string().c_str());
        // TODO 尝试去连接leader（此时应该是重连）
    }
    is_init_ = true;
}

RaftNode::~RaftNode()
{
    if (metadata_file_ != nullptr)
    {
        fclose(metadata_file_);
        metadata_file_ = nullptr;
    }
}
Response RaftNode::execute_command(const std::string &cmd)
{
    if (!Storage::is_init())
    {
        LOG_ERROR("Storage is not initialized");
        exit(1);
    }
    static Storage *storage_ = Storage::get_instance();
    // 解析命令并执行
    std::vector<std::string> command_parts = split_by_spacer(cmd);
    Response response;

    if (command_parts.empty())
    {
        response = Response::error("Invalid command");
    }
    else
    {
        bool is_exec = false;
        response = handle_raft_command(command_parts, is_exec);
        if (is_exec)
        {
            return response;
        }
        uint8_t operation = stringToOperationType(command_parts[0]);
        if (isWriteOperation(operation) && role_ != RaftRole::Leader)
        {
            response = Response::error("not the leader node,cannot write");
            return response;
        }
        command_parts.erase(command_parts.begin());
        response = storage_->execute(operation, command_parts);
    }
    return response;
}

Response RaftNode::handle_raft_command(const std::vector<std::string> &command_parts, bool &is_exec)
{
    Response response;
    is_exec = true;
    if (command_parts[0] == "get_master" && command_parts.size() == 1)
    {
        // 返回当前leader地址
        Address leader = leader_address_.load();
        if (leader.is_null())
        {
            response = Response::error("no leader elected");
        }
        else
        {
            response = Response::success(leader.to_string());
        }
    }
    else if (command_parts[0] == "set_master" && (command_parts.size() == 3 || command_parts.size() == 4))
    {
        // 设置当前leader地址（仅限leader节点调用）
        if (role_ != RaftRole::Leader)
        {
            response = Response::error("only leader can set master");
            return response;
        }
        std::string host = command_parts[1];
        int port = std::stoi(command_parts[2]);
        std::string password = "";
        if (command_parts.size() == 4)
        {
            password = command_parts[3];
        }
        Address new_leader(host, port);
        // TODO 尝试去连接leader
        leader_address_.store(new_leader);
        response = Response::success(true);
    }
    else if (command_parts[0] == "raft_password" && command_parts.size() == 1)
    {
        response = Response::success(password_);
    }
    else
    {
        is_exec = false;
    }
    return response;
}
