#include "raft/raft.h"
#include "logger/logger.h"
#include <algorithm>
#include <sys/stat.h>
#include "storage/storage.h"
#include "common/types/operation_type.h"
#include "common/util/path_utils.h"
#include "common/util/ip_utils.h"
#include "common/util/file_utils.h"
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define mkdir _mkdir
#else
#include <unistd.h>
#endif

std::unique_ptr<RaftNode> RaftNode::instance_ = nullptr;
bool RaftNode::is_init_ = false;

class StringSerializeCommand : public SerializeCommand<std::string>
{
public:
    std::string serialize(const std::string &value)
    {
        return Serializer::serialize(value);
    }
    std::string deserialize(const char *data, size_t &offset)
    {
        return Serializer::deserializeString(data, offset);
    }
};

class ResponseSerializeCommand : public SerializeCommand<Response>
{
    std::string serialize(const Response &value)
    {
        return value.serialize();
    }
    Response deserialize(const char *data, size_t &offset)
    {
        Response response;
        response.deserialize(data, offset);
        return response;
    }
};

// RaftNode构造函数
RaftNode::RaftNode(const std::string root_dir,
                   const std::string &ip,
                   const u_short port,
                   const std::unordered_set<std::string> &trusted_nodes,
                   const uint32_t max_follower_count,
                   const RaftNodeConfig &config)
    : TCPServer(ip, port, max_follower_count, 0, 0),
      root_dir_(root_dir),
      role_(RaftRole::Leader),
      trusted_nodes_(trusted_nodes),
      election_timeout_min_(config.election_timeout_min_ms),
      election_timeout_max_(config.election_timeout_max_ms),
      heartbeat_interval_(config.heartbeat_interval_ms),
      raft_msg_timeout_(config.raft_rpc_timeout_ms),
      result_cache_(config.result_cache_capacity),
      thread_pool_(nullptr),
      config_(config),
      rng_(std::chrono::steady_clock::now().time_since_epoch().count())
{
    std::string node_id = ip + ":" + std::to_string(port);
    LOG_INFO("[Node=%s] Initializing RaftNode", node_id.c_str());
    log_array_ = std::make_unique<RaftLogArray>(
        PathUtils::combine_path(root_dir, ".raft"),
        config_.log_config);
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    // 2. 加载持久化状态
    load_persistent_state();
    LOG_INFO("[Node=%s] Loaded persistent state - Term=%u, CommitIndex=%u, LastApplied=%u, SnapshotIndex=%u",
             node_id.c_str(),
             persistent_state_.current_term_.load(),
             persistent_state_.commit_index_.load(),
             persistent_state_.last_applied_.load(),
             persistent_state_.log_snapshot_index_.load());

    // 3. 启动后台线程
    start_background_threads();

    // 4. 根据场景选择初始化策略
    if (!persistent_state_.cluster_metadata_.current_leader_.is_null())
    {
        // 场景1: 已知leader，作为follower加入
        LOG_INFO("[Node=%s] Found leader address: %s, initializing as follower",
                 node_id.c_str(),
                 persistent_state_.cluster_metadata_.current_leader_.to_string().c_str());
        init_as_follower();
    }
    else if (persistent_state_.cluster_metadata_.cluster_nodes_.empty())
    {
        // 场景2: 首个节点，自举为leader
        LOG_INFO("[Node=%s] No cluster nodes found, bootstrapping as leader in term %u",
                 node_id.c_str(),
                 persistent_state_.current_term_.load());
    }
    else
    {
        // 场景3: 探查集群
        LOG_INFO("[Node=%s] Cluster nodes found (%zu nodes) but no leader, discovering cluster state",
                 node_id.c_str(),
                 persistent_state_.cluster_metadata_.cluster_nodes_.size());
        init_with_cluster_discovery();
    }

    // 确保 CommitIndex 不超过日志最后一条索引
    uint32_t last_log_idx = log_array_->get_last_index();
    if (persistent_state_.commit_index_ > last_log_idx)
    {
        LOG_WARN("[Node=%s] Sanitizing CommitIndex: %u -> %u (LastLogIndex)",
                 node_id.c_str(), persistent_state_.commit_index_.load(), last_log_idx);
        persistent_state_.commit_index_.store(last_log_idx);
    }

    // 尝试应用已加载的日志
    apply_committed_entries();

    // 5. 为LRU设置键值序列化器
    std::shared_ptr<StringSerializeCommand> string_serialize_command = std::make_shared<StringSerializeCommand>();
    std::shared_ptr<ResponseSerializeCommand> response_serialize_command = std::make_shared<ResponseSerializeCommand>();
    result_cache_.set_key_serializer(string_serialize_command);
    result_cache_.set_value_serializer(response_serialize_command);
    // 6. 初始化线程池(从配置读取参数)
    ThreadPool::Config pool_config = config_.thread_pool_config;
    thread_pool_ = std::make_unique<ThreadPool>(pool_config);
    is_init_ = true;
    LOG_INFO("[Node=%s][Role=%s][Term=%u] RaftNode initialized successfully",
             node_id.c_str(),
             role_to_string(role_.load()),
             persistent_state_.current_term_.load());
}

RaftNode::~RaftNode()
{
    std::string node_id = get_node_id();
    LOG_INFO("[Node=%s][Role=%s][Term=%u] Destroying RaftNode",
             node_id.c_str(),
             role_to_string(role_.load()),
             persistent_state_.current_term_.load());

    // 停止后台线程
    election_thread_running_ = false;
    heartbeat_thread_running_ = false;
    follower_client_thread_running_ = false;

    LOG_INFO("[Node=%s] Stopping background threads", node_id.c_str());
    if (election_thread_.joinable())
    {
        election_thread_.join();
    }
    if (heartbeat_thread_.joinable())
    {
        heartbeat_thread_.join();
    }
    if (follower_client_thread_.joinable())
    {
        follower_client_thread_.join();
    }

    for (auto &[sock, snapshot_state] : snapshot_state_)
    {
        if (snapshot_state.fp != nullptr)
        {
            fclose(snapshot_state.fp);
            snapshot_state.fp = nullptr;
        }
    }
    LOG_INFO("[Node=%s] RaftNode destroyed successfully", node_id.c_str());
}

// 加载持久化状态
void RaftNode::load_persistent_state()
{
    std::string meta_path = PathUtils::combine_path(PathUtils::combine_path(root_dir_, ".raft"), ".raft_meta");
    FILE *file = fopen(meta_path.c_str(), "rb");
    if (file == nullptr)
    {
        LOG_INFO("No persistent state file found: %s", meta_path.c_str());
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size > 0)
    {
        std::string data(file_size, '\0');
        if (fread(&data[0], 1, file_size, file) != (size_t)file_size)
        {
            LOG_ERROR("Failed to read persistent state from file");
            fclose(file);
            return;
        }
        size_t offset = 0;
        persistent_state_ = PersistentState::deserialize(data.data(), offset);
    }
    fclose(file);
}

// 保存持久化状态
void RaftNode::save_persistent_state()
{
    std::string meta_path = PathUtils::combine_path(PathUtils::combine_path(root_dir_, ".raft"), ".raft_meta");
    FILE *file = fopen(meta_path.c_str(), "wb");
    if (file == nullptr)
    {
        LOG_ERROR("Failed to open metadata file for writing: %s", meta_path.c_str());
        return;
    }

    std::string serialized_data = persistent_state_.serialize();
    if (fwrite(serialized_data.data(), 1, serialized_data.size(), file) != serialized_data.size())
    {
        LOG_ERROR("Failed to write persistent state to file: %s", meta_path.c_str());
        fclose(file);
        return;
    }

    fflush(file);
    fclose(file);
}

// 客户端线程工作函数
void RaftNode::follower_client_loop()
{
    while (follower_client_thread_running_)
    {
        if (follower_client_ == nullptr || role_ == RaftRole::Leader)
        {
            std::unique_lock<std::mutex> lock(follower_client_cv_mutex_);
            follower_client_cv_.wait_for(lock, std::chrono::milliseconds(config_.follower_idle_wait_ms), [this]()
                                         { return !follower_client_thread_running_ || follower_client_ != nullptr && role_ != RaftRole::Leader; });
            continue;
        }
        ProtocolBody *msg = new_body();
        // 为了防止一直等待添加一个超时时间
        int ret = follower_client_->receive(*msg, raft_msg_timeout_);
        if (ret == -1)
        {
            LOG_ERROR("Follower client failed to receive message,because of timeout");
            delete msg;
        }
        else if (ret == -2)
        {
            LOG_ERROR("Follower client failed to receive message, connection closed");
            delete msg;
            become_follower(persistent_state_.cluster_metadata_.current_leader_, persistent_state_.current_term_, true, persistent_state_.commit_index_);
        }
        else if (ret > 0)
        {
            // 处理收到的消息
            RaftMessage *raft_msg = dynamic_cast<RaftMessage *>(msg);
            handle_leader_message(*raft_msg);
        }
    }
}

void RaftNode::handle_leader_message(const RaftMessage &msg)
{
    switch (msg.type)
    {
    case RaftMessageType::APPEND_ENTRIES:
        handle_append_entries(msg);
        break;
    case RaftMessageType::NEW_NODE:
        handle_new_node_join(msg);
        break;
    case RaftMessageType::LEAVE_NODE:
        handle_leave_node(msg);
        break;
    case RaftMessageType::QUERY_LEADER_RESPONSE:
        handle_query_leader_response(msg);
        break;
    case RaftMessageType::JOIN_CLUSTER_RESPONSE:
        handle_join_cluster_response(msg);
        break;
    case RaftMessageType::SNAPSHOT_INSTALL:
        handle_install_snapshot(msg);
        break;
    default:
        LOG_WARN("Unknown message type: %d", static_cast<int>(msg.type));
        break;
    }
}
void RaftNode::handle_join_cluster_response(const RaftMessage &msg)
{
    LOG_INFO("Received Join cluster response");
    if (!msg.join_cluster_response_data.has_value())
    {
        LOG_WARN("Join cluster response data is empty");
        return;
    }
    if (role_ != RaftRole::Follower)
    {
        LOG_WARN("Join cluster response received in non-follower role: %d", static_cast<int>(role_));
        return;
    }
    uint32_t term = msg.term;
    auto &data = *msg.join_cluster_response_data;
    if (data.success)
    {
        LOG_INFO("Join cluster success");
        // 获取自身的address
        Address self_addr;
        get_self_address(follower_client_->get_socket(), self_addr);
        self_addr.port = port_;
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Address leader_addr = persistent_state_.cluster_metadata_.current_leader_;
        persistent_state_.current_term_ = term;
        persistent_state_.cluster_metadata_ = data.cluster_metadata;
        persistent_state_.cluster_metadata_.cluster_nodes_.insert(leader_addr);
        persistent_state_.cluster_metadata_.cluster_nodes_.erase(self_addr);
        persistent_state_.cluster_metadata_.current_leader_ = leader_addr;
        save_persistent_state();
    }
    else
    {
        LOG_ERROR("Join cluster failed: %s", data.error_message.c_str());
        // 变为leader
        become_leader();
    }
}
void RaftNode::handle_query_leader_response(const RaftMessage &msg)
{
    LOG_INFO("Received Query leader response");
    auto &leader_addr = msg.query_leader_response_data->leader_address;
    if (!leader_addr.is_null())
    {
        // 找到leader，设置为follower
        // 获取本地地址(ip)
        Address local_addr;
        if (get_self_address(follower_client_->get_socket(), local_addr))
        {
            if (!(leader_addr.host == local_addr.host && leader_addr.port == this->port_))
            {
                become_follower(leader_addr);
            }
            else
            {
                LOG_INFO("This node is the new leader");
                become_leader();
            }
        }
        else
        {
            LOG_ERROR("Failed to get local address");
        }
    }
}

void RaftNode::handle_leave_node(const RaftMessage &msg)
{
    LOG_INFO("Node leaving the cluster");
    if (!msg.leave_node_data.has_value())
    {
        LOG_WARN("Leave node data is empty");
    }
    auto &data = *msg.leave_node_data;
    Address self_addr;
    get_self_address(follower_client_->get_socket(), self_addr);
    self_addr.port = port_;
    if (self_addr == data.node_address)
    {
        LOG_INFO("This node is leaving the cluster, shutting down");
        become_leader();
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        persistent_state_.cluster_metadata_.cluster_nodes_.erase(data.node_address);
        save_persistent_state();
    }
}
void RaftNode::handle_new_node_join(const RaftMessage &msg)
{
    LOG_INFO("New node joined the cluster");
    if (!msg.new_node_join_data.has_value())
    {
        LOG_WARN("New node join data is empty");
    }
    auto &data = *msg.new_node_join_data;
    Address self_addr;
    get_self_address(follower_client_->get_socket(), self_addr);
    self_addr.port = port_;
    if (data.node_address == self_addr)
    {
        return;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        persistent_state_.cluster_metadata_.cluster_nodes_.insert(data.node_address);
        save_persistent_state();
    }
}
void RaftNode::init_as_follower()
{
    LOG_INFO("Initiating as follower to connect to leader: %s", persistent_state_.cluster_metadata_.current_leader_.to_string().c_str());
    become_follower(persistent_state_.cluster_metadata_.current_leader_, persistent_state_.current_term_, true, persistent_state_.commit_index_);
}

bool RaftNode::connect_to_leader(const Address &leader_addr)
{
    LOG_INFO("attempt to connect to leader: %s", leader_addr.to_string().c_str());
    follower_client_ = std::make_unique<TCPClient>(leader_addr.host, leader_addr.port);
    try
    {
        follower_client_->connect();
        LOG_INFO("Connected to leader: %s", leader_addr.to_string().c_str());
        return true;
    }
    catch (const std::exception &e)
    {
        LOG_ERROR("Failed to connect to leader: %s,leader addr: %s", e.what(), leader_addr.to_string().c_str());
        follower_client_ = nullptr;
        return false;
    }
}

// 场景3: 探查集群状态
void RaftNode::init_with_cluster_discovery()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    std::unordered_set<Address> &cluster_nodes = persistent_state_.cluster_metadata_.cluster_nodes_;
    if (cluster_nodes.empty())
    {
        LOG_WARN("No cluster nodes to discover, will wait for manual configuration");
        return;
    }

    // 随机选择一个节点探查
    RaftMessage msg = RaftMessage::query_leader();

    for (auto &target_node : cluster_nodes)
    {
        LOG_INFO("Probing cluster via node: %s", target_node.to_string().c_str());

        // 发送QueryLeader消息并等待响应
        TCPClient client(target_node.host, target_node.port);
        try
        {
            client.connect();
            client.send(msg);
            ProtocolBody *body = new_body();
            int ret = client.receive(*body, raft_msg_timeout_);
            if (ret < 0)
            {
                LOG_ERROR("Failed to receive response from node: %s", target_node.to_string().c_str());
                delete body;
                continue;
            }
            RaftMessage *response_msg = dynamic_cast<RaftMessage *>(body);
            if (response_msg->type != RaftMessageType::QUERY_LEADER_RESPONSE)
            {
                delete body;
                continue;
            }
            Address &leader_addr = response_msg->query_leader_response_data->leader_address;
            if (!leader_addr.is_null())
            {
                // 找到leader，设置为follower
                // 获取本地地址(ip)
                Address local_addr;
                if (get_self_address(client.get_socket(), local_addr))
                {
                    if (!(leader_addr.host == local_addr.host && leader_addr.port == this->port_))
                    {
                        persistent_state_.cluster_metadata_.current_leader_ = leader_addr;
                        init_as_follower();
                    }
                }
                else
                {
                    LOG_ERROR("Failed to get local address");
                }
            }
            delete body;
            break;
        }
        catch (const std::exception &e)
        {
            LOG_ERROR("Exception caught while probing cluster: %s", e.what());
        }
    }
}
// 启动后台线程
void RaftNode::start_background_threads()
{
    election_thread_running_ = true;
    election_thread_ = std::thread(&RaftNode::election_loop, this);
    heartbeat_thread_running_ = true;
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);
    follower_client_thread_running_ = true;
    follower_client_thread_ = std::thread(&RaftNode::follower_client_loop, this);
    LOG_INFO("Background threads started");
}

// 获取当前时间戳
uint32_t RaftNode::get_current_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

// 转换为Follower
bool RaftNode::become_follower(const Address &leader_addr, uint32_t term, bool is_reconnect, const uint32_t commit_index)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    std::string node_id = get_node_id();
    RaftRole old_role = role_.load();

    LOG_INFO("[Node=%s][Role=%s][Term=%u] STATE TRANSITION -> Follower, Leader=%s, TargetTerm=%u, IsReconnect=%s, CommitIndex=%u",
             node_id.c_str(),
             role_to_string(old_role),
             persistent_state_.current_term_.load(),
             leader_addr.to_string().c_str(),
             term,
             is_reconnect ? "true" : "false",
             commit_index);

    // 连接leader并同步数据
    if (connect_to_leader(leader_addr))
    {
        // 连接成功，发送消息
        RaftMessage init_message = RaftMessage::join_cluster(is_reconnect, commit_index, port_);
        follower_client_->send(init_message);

        // 重试机制：最多重试 N 次，每次超时时间递增
        const int max_retries = config_.join_cluster_max_retries;
        const int base_timeout = raft_msg_timeout_;
        bool success = false;

        for (int retry = 0; retry < max_retries && !success; retry++)
        {
            int current_timeout = base_timeout * (retry + 1); // 超时时间递增：2秒, 4秒, 6秒
            LOG_INFO("[Node=%s] Follower: Attempting to receive init message response (retry %d/%d, timeout: %dms)",
                     get_node_id().c_str(), retry + 1, max_retries, current_timeout);

            ProtocolBody *msg = new_body();
            int ret = follower_client_->receive(*msg, current_timeout);
            if (ret >= 0)
            {
                RaftMessage *response_msg = dynamic_cast<RaftMessage *>(msg);
                if (response_msg->type != RaftMessageType::JOIN_CLUSTER_RESPONSE && response_msg->type != RaftMessageType::QUERY_LEADER_RESPONSE)
                {
                    LOG_ERROR("Follower client received invalid init message response");
                }
                else
                {
                    success = true;
                    reset_election_timeout();
                    to_follower();
                    LOG_INFO("[Node=%s][Role=%s][Term=%u] Successfully connected to leader %s, commit_index updated to %u",
                             node_id.c_str(),
                             role_to_string(role_.load()),
                             persistent_state_.current_term_.load(),
                             leader_addr.to_string().c_str(),
                             commit_index);
                    // 更新任期
                    persistent_state_.cluster_metadata_.current_leader_ = leader_addr;
                    persistent_state_.cluster_metadata_.cluster_nodes_.clear();
                    persistent_state_.commit_index_ = commit_index;
                    if (term > persistent_state_.current_term_)
                    {
                        uint32_t old_term = persistent_state_.current_term_.load();
                        persistent_state_.current_term_ = term;
                        persistent_state_.voted_for_ = Address(); // 重置投票
                        LOG_INFO("[Node=%s] Term updated: %u -> %u", node_id.c_str(), old_term, term);
                    }
                    save_persistent_state();

                    // 清空leader状态
                    next_index_.clear();
                    match_index_.clear();
                    {
                        std::lock_guard<std::shared_mutex> lock(follower_sockets_mutex_);
                        // 关闭所有follower_sockets_
                        for (auto &socket : follower_sockets_)
                        {
                            // 给当前的follower_sockets_发送QueryLeaderResponse消息,让他们也去连接leader
                            RaftMessage response = RaftMessage::query_leader_response(leader_addr);
                            send(response, socket);
                            close_socket(socket);
                        }
                        // 清空follower_sockets_和follower_address_map_
                        follower_sockets_.clear();
                        follower_address_map_.clear();
                    }
                    // 处理接收到的消息
                    handle_leader_message(*response_msg);
                    delete msg;
                    return role_ == RaftRole::Follower;
                }
            }
            else
            {
                LOG_WARN("[Node=%s] Follower: Failed to receive init message response (retry %d), will retry connection",
                         get_node_id().c_str(), retry + 1);
            }
            delete msg;
        }

        // 所有重试都失败
        if (!success)
        {
            LOG_ERROR("[Node=%s] Follower: All %d retries failed to receive init message response from leader, closing connection",
                      get_node_id().c_str(), max_retries);
            follower_client_->close();
        }
        else
        {
            LOG_ERROR("Failed to connect to leader %s", leader_addr.to_string().c_str());
        }
        return false;
    }
    else
    {
        LOG_ERROR("Failed to connect to leader %s", leader_addr.to_string().c_str());
        return false;
    }
}

// 转换为Candidate
void RaftNode::become_candidate()
{
    std::string node_id = get_node_id();
    RaftRole old_role;
    uint32_t old_term, new_term;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        old_role = role_.load();
        old_term = persistent_state_.current_term_.load();

        // 1. 增加任期
        persistent_state_.current_term_++;
        new_term = persistent_state_.current_term_.load();

        // 2. 给自己投票
        persistent_state_.voted_for_ = Address("127.0.0.1", port_);
        save_persistent_state();

        // 3. 初始化票数 (1票是自己)
        granted_votes_ = 1;

        // 4. 更新角色
        to_candidate();

        // 5. 重置选举计时器（防止在本次选举中立即再次超时）
        last_heartbeat_time_ = std::chrono::steady_clock::now();
        election_timeout_ = generate_election_timeout();

        LOG_INFO("[Node=%s] STATE TRANSITION: %s -> Candidate, Term: %u -> %u, Votes: 1, ElectionTimeout: %dms",
                 node_id.c_str(),
                 role_to_string(old_role),
                 old_term,
                 new_term,
                 election_timeout_);
    }
    // 发送RequestVote
    send_request_vote();
}

// 转换为Leader
void RaftNode::become_leader()
{
    std::string node_id = get_node_id();
    RaftRole old_role;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        old_role = role_.load();
        // 更新角色
        to_leader();

        // 暂不清空旧的leader记录，方便下次连接相同的leader
        // persistent_state_.cluster_metadata_.current_leader_ = Address();
        // 清空集群节点
        persistent_state_.cluster_metadata_.cluster_nodes_.clear();
        save_persistent_state();

        LOG_INFO("[Node=%s] STATE TRANSITION: %s -> Leader, Term: %u, Cluster size: %zu",
                 node_id.c_str(),
                 role_to_string(old_role),
                 persistent_state_.current_term_.load(),
                 persistent_state_.cluster_metadata_.cluster_nodes_.size());
    }
}

// 选举线程主循环
void RaftNode::election_loop()
{
    std::string node_id = get_node_id();
    LOG_INFO("[Node=%s][Role=%s] Election loop started, timeout range: %d-%dms",
             node_id.c_str(),
             role_to_string(role_.load()),
             election_timeout_min_,
             election_timeout_max_);

    while (election_thread_running_)
    {
        // 如果已经是Leader，等待直到变成Follower或Candidate
        if (role_ == RaftRole::Leader)
        {
            LOG_DEBUG("[Node=%s] Election thread: Already Leader, waiting for role change", node_id.c_str());
            std::unique_lock<std::mutex> lock(election_cv_mutex_);
            // 等待被唤醒（role变为非Leader或线程被停止）
            election_cv_.wait_for(lock, std::chrono::milliseconds(election_timeout_),
                                  [this]()
                                  {
                                      return !election_thread_running_ || role_ != RaftRole::Leader;
                                  });
            continue;
        }

        // 计算等待时间
        uint32_t sleep_time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(last_heartbeat_time_.time_since_epoch()).count()) + election_timeout_ - get_current_timestamp();

        if (sleep_time > 0)
        {
            LOG_DEBUG("[Node=%s] Election thread: Waiting %ums before next election check", node_id.c_str(), sleep_time);
            std::unique_lock<std::mutex> lock(election_cv_mutex_);
            // 等待指定时间，或被提前唤醒（role变化或线程停止）
            election_cv_.wait_for(lock, std::chrono::milliseconds(sleep_time),
                                  [this]()
                                  {
                                      return !election_thread_running_ || role_ == RaftRole::Leader;
                                  });
        }

        // 检查是否超时且仍不是Leader
        if (election_thread_running_ && role_ != RaftRole::Leader && is_election_timeout())
        {
            LOG_WARN("[Node=%s][Role=%s][Term=%u] Election timeout triggered (%dms), starting new election",
                     node_id.c_str(),
                     role_to_string(role_.load()),
                     persistent_state_.current_term_.load(),
                     election_timeout_);
            become_candidate();
        }
    }
    LOG_INFO("[Node=%s] Election loop stopped", node_id.c_str());
}

// 心跳线程主循环
void RaftNode::heartbeat_loop()
{
    std::string node_id = get_node_id();
    LOG_INFO("[Node=%s] Heartbeat loop started, interval: %dms", node_id.c_str(), heartbeat_interval_);

    while (heartbeat_thread_running_)
    {
        // 如果不是Leader，等待直到变成Leader
        if (role_ != RaftRole::Leader)
        {
            LOG_DEBUG("[Node=%s] Heartbeat thread: Not Leader, waiting for role change", node_id.c_str());
            std::unique_lock<std::mutex> lock(heartbeat_cv_mutex_);
            // 等待被唤醒（role变为Leader或线程被停止）
            heartbeat_cv_.wait_for(lock, std::chrono::milliseconds(heartbeat_interval_),
                                   [this]()
                                   {
                                       return !heartbeat_thread_running_ || role_ == RaftRole::Leader;
                                   });
            continue;
        }

        // 等待心跳间隔时间
        LOG_DEBUG("[Node=%s] Heartbeat thread: Waiting %dms before next heartbeat", node_id.c_str(), heartbeat_interval_);
        std::unique_lock<std::mutex> lock(heartbeat_cv_mutex_);
        // 等待指定时间，或被提前唤醒（role变化或线程停止）
        heartbeat_cv_.wait_for(lock, std::chrono::milliseconds(heartbeat_interval_),
                               [this]()
                               {
                                   return !heartbeat_thread_running_ || role_ != RaftRole::Leader;
                               });

        // 如果被唤醒后仍不是Leader，跳过本次心跳发送
        if (!heartbeat_thread_running_ || role_ != RaftRole::Leader)
        {
            continue;
        }

        if (log_array_)
        {
            uint32_t log_count = log_array_->size();
            LOG_DEBUG("[Node=%s][Role=Leader][Term=%u] Sending heartbeat to %zu followers, log_count: %u",
                      node_id.c_str(),
                      persistent_state_.current_term_.load(),
                      follower_sockets_.size(),
                      log_count);
            send_heartbeat_to_all();
        }
    }
    LOG_INFO("[Node=%s] Heartbeat loop stopped", node_id.c_str());
}

// 发送RequestVote
void RaftNode::send_request_vote()
{
    std::string node_id = get_node_id();
    // 获取当前状态（加锁读取后立即释放，避免网络阻塞锁）
    uint32_t current_term;
    uint32_t last_log_index;
    uint32_t last_log_term;
    std::vector<Address> nodes;

    log_array_->get_last_info(last_log_index, last_log_term);
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        current_term = persistent_state_.current_term_.load();
        // 复制一份节点列表，避免遍历时持锁
        for (const auto &node : persistent_state_.cluster_metadata_.cluster_nodes_)
        {
            nodes.push_back(node);
        }
    }

    // 构造消息
    RaftMessage msg = RaftMessage::request_vote(current_term, last_log_index, last_log_term);

    LOG_INFO("[Node=%s][Role=Candidate][Term=%u] ELECTION: Sending RequestVote to %zu nodes, LastLogIndex=%u, LastLogTerm=%u",
             node_id.c_str(),
             current_term,
             nodes.size(),
             last_log_index,
             last_log_term);

    // 遍历发送
    for (const auto &node : nodes)
    {
        bool is_submitted = thread_pool_->submit([node, msg, this, node_id]()
                                                 {
            // 创建临时的 TCPClient
            TCPClient client(node.host, node.port);

            try
            {
                // 连接设置较短的超时
                client.connect();

                // 发送请求
                int ret = client.send(msg);
                if(ret <= 0){
                    LOG_WARN("[Node=%s] Failed to send RequestVote to %s", node_id.c_str(), node.to_string().c_str());
                    return;
                }
                // 设置超时时间，例如 200ms。如果对方挂了，这里会超时返回，不会卡死。
                ProtocolBody *response_body = new_body();
                ret = client.receive(*response_body, config_.request_vote_recv_timeout_ms); // 假设 receive 支持超时 ms

                if (ret > 0)
                {
                    RaftMessage *resp = dynamic_cast<RaftMessage *>(response_body);
                    if (resp && resp->type == RaftMessageType::REQUEST_VOTE_RESPONSE)
                    {
                        handle_request_vote_response(*resp);
                    }
                }
                delete response_body;
            }
            catch (const std::exception &e)
            {
                LOG_WARN("[Node=%s] Failed to request vote from %s: %s", node_id.c_str(), node.to_string().c_str(), e.what());
            } });
        if (!is_submitted)
        {
            LOG_WARN("[Node=%s] Failed to submit RequestVote task to thread pool for node %s", node_id.c_str(), node.to_string().c_str());
        }
    }
}

// 发送心跳到所有节点
void RaftNode::send_heartbeat_to_all()
{
    std::string node_id = get_node_id();
    std::unordered_set<Address> &cluster_nodes_ = persistent_state_.cluster_metadata_.cluster_nodes_;
    if (cluster_nodes_.empty())
    {
        LOG_DEBUG("[Node=%s] No followers to send heartbeat", node_id.c_str());
        return;
    }

    std::shared_lock<std::shared_mutex> lock(follower_sockets_mutex_);
    size_t sent_count = 0;
    for (const auto &sock : follower_sockets_)
    {
        send_append_entries(sock);
        sent_count++;
    }
    LOG_DEBUG("[Node=%s] Heartbeat sent to %zu followers", node_id.c_str(), sent_count);
}

// 发送AppendEntries
void RaftNode::send_append_entries(const socket_t &sock)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    send_append_entries_nolock(sock);
}

void RaftNode::send_append_entries_nolock(const socket_t &sock)
{
    std::string node_id = get_node_id();
    // 假设调用者已持有 mutex_

    // 如果不是 Leader，不发送
    if (role_ != RaftRole::Leader)
    {
        LOG_WARN("[Node=%s] Attempted to send AppendEntries but not Leader", node_id.c_str());
        return;
    }

    // 获取该 Follower 需要的下一条日志索引
    uint32_t next_idx = next_index_[sock];
    uint32_t last_log_idx = log_array_->get_last_index();

    // 准备 RPC 参数
    uint32_t prev_log_index = (next_idx > 0) ? next_idx - 1 : 0;
    uint32_t prev_log_term = log_array_->get_term(prev_log_index);
    uint32_t leader_commit = persistent_state_.commit_index_.load();
    uint32_t term = persistent_state_.current_term_.load();

    RaftMessage msg;

    // 如果 Follower 落后于 Leader (next_idx <= last_log_idx)，则携带日志
    // 否则，发送空日志（纯心跳）
    if (next_idx <= last_log_idx)
    {
        // 限制单次发送数量，防止包过大阻塞网络（例如每次最多发 config_.append_entries_max_batch 条）
        std::vector<LogEntry> entries = log_array_->get_entries_from(next_idx, static_cast<int>(config_.append_entries_max_batch));

        std::set<LogEntry> entry_set(entries.begin(), entries.end());
        msg = RaftMessage::append_entries_with_data(term, prev_log_index, prev_log_term, entry_set, leader_commit);

        LOG_DEBUG("[Node=%s][Role=Leader][Term=%u] LOG REPLICATION: Socket=%d, PrevIndex=%u, PrevTerm=%u, Entries=%zu, LeaderCommit=%u",
                 node_id.c_str(),
                 term,
                 sock,
                 prev_log_index,
                 prev_log_term,
                 entries.size(),
                 leader_commit);
    }
    else
    {
        // 没有新日志，发送纯心跳
        msg = RaftMessage::append_entries(term, prev_log_index, prev_log_term, leader_commit);
        LOG_DEBUG("[Node=%s][Role=Leader][Term=%u] HEARTBEAT: Socket=%d, PrevIndex=%u, PrevTerm=%u, LeaderCommit=%u",
                  node_id.c_str(),
                  term,
                  sock,
                  prev_log_index,
                  prev_log_term,
                  leader_commit);
    }

    int ret = send(msg, sock);
    if (ret < 0)
    {
        LOG_ERROR("[Node=%s] Failed to send AppendEntries to socket %d: %s", node_id.c_str(), sock, strerror(errno));
    }
}
bool RaftNode::handle_append_entries(const RaftMessage &msg)
{
    std::string node_id = get_node_id();
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 1. 检查term
    if (msg.term < persistent_state_.current_term_.load())
    {
        LOG_WARN("[Node=%s][Role=%s][Term=%u] REJECTED AppendEntries from old term %u",
                 node_id.c_str(),
                 role_to_string(role_.load()),
                 persistent_state_.current_term_.load(),
                 msg.term);
        // 发送失败响应
        if (follower_client_)
        {
            uint32_t last_idx = log_array_->get_last_index();
            RaftMessage response = RaftMessage::append_entries_response(persistent_state_.current_term_.load(), false, last_idx);
            follower_client_->send(response);
        }
        return false;
    }
    to_follower();

    // 2. 发现更高term,更新term
    if (msg.term > persistent_state_.current_term_.load())
    {
        uint32_t old_term = persistent_state_.current_term_.load();
        persistent_state_.current_term_.store(msg.term);
        LOG_INFO("[Node=%s] TERM UPDATE: %u -> %u (discovered from leader)",
                 node_id.c_str(),
                 old_term,
                 msg.term);
    }

    // 重置选举超时
    reset_election_timeout();

    if (!msg.append_entries_data)
    {
        LOG_ERROR("[Node=%s] AppendEntries message missing data", node_id.c_str());
        // 发送失败响应
        if (follower_client_)
        {
            uint32_t last_idx = log_array_->get_last_index();
            RaftMessage response = RaftMessage::append_entries_response(persistent_state_.current_term_.load(), false, last_idx);
            follower_client_->send(response);
        }
        return false;
    }

    const auto &data = *msg.append_entries_data;

    // 3. 检查prev_log_index和term
    if (data.prev_log_index > 0)
    {
        LogEntry prev_entry;
        if (!log_array_->get(data.prev_log_index, prev_entry))
        {
            LOG_WARN("[Node=%s] Prev log index %u not found, rejecting AppendEntries",
                     node_id.c_str(),
                     data.prev_log_index);
            // 发送失败响应
            if (follower_client_)
            {
                uint32_t last_idx = log_array_->get_last_index();
                RaftMessage response = RaftMessage::append_entries_response(persistent_state_.current_term_.load(), false, last_idx);
                follower_client_->send(response);
            }
            return false;
        }

        if (prev_entry.term != data.prev_log_term)
        {
            LOG_WARN("[Node=%s] Prev log term mismatch at index %u: expected %u, got %u",
                     node_id.c_str(),
                     data.prev_log_index,
                     prev_entry.term,
                     data.prev_log_term);
            // 发送失败响应
            if (follower_client_)
            {
                uint32_t last_idx = log_array_->get_last_index();
                RaftMessage response = RaftMessage::append_entries_response(persistent_state_.current_term_.load(), false, last_idx);
                follower_client_->send(response);
            }
            return false;
        }
    }

    // 4. 追加新日志
    if (data.entries && !data.entries->empty())
    {
        size_t appended = 0;
        size_t conflicted = 0;
        for (const auto &entry : *data.entries)
        {
            // 检查该索引是否已存在且任期一致
            bool found = false;
            LogEntry existing_entry;
            if (log_array_->get(entry.index, existing_entry))
            {
                if (existing_entry.term != entry.term)
                {
                    // term不匹配，删除冲突日志及之后的所有日志
                    LOG_INFO("[Node=%s] LOG CONFLICT at index %u: local_term=%u, remote_term=%u, truncating",
                             node_id.c_str(),
                             entry.index,
                             existing_entry.term,
                             entry.term);
                    log_array_->truncate_from(entry.index);
                    conflicted++;
                }
                else
                {
                    // 已存在且任期一致，跳过此条目
                    found = true;
                }
            }

            if (!found)
            {
                // 追加新日志
                if (log_array_->append(const_cast<LogEntry &>(entry)))
                {
                    appended++;
                }
            }
        }
        if (appended > 0)
        {
            LOG_INFO("[Node=%s][Role=%s][Term=%u] APPENDED: %zu new entries, %zu conflicts, PrevIndex=%u",
                     node_id.c_str(),
                     role_to_string(role_.load()),
                     persistent_state_.current_term_.load(),
                     appended,
                     conflicted,
                     data.prev_log_index);
        }
    }

    // 5. 更新commit_index
    if (data.leader_commit > persistent_state_.commit_index_.load(std::memory_order_relaxed))
    {
        uint32_t last_index = log_array_->get_last_index(); // 获取本地最新日志
        // commit_index 取 leader_commit 和 本地最新日志索引 的较小值
        uint32_t old_commit = persistent_state_.commit_index_.load();
        uint32_t new_commit = std::min(data.leader_commit, last_index);

        if (new_commit > old_commit)
        {
            persistent_state_.commit_index_.store(new_commit, std::memory_order_relaxed);
            save_persistent_state();
            // 减少日志输出，只在批量更新时打印
            if (new_commit % 100 == 0)
            {
                LOG_DEBUG("[Node=%s] COMMIT INDEX updated: %u -> %u", node_id.c_str(), old_commit, new_commit);
            }
            apply_committed_entries_nolock(); // Follower 也要应用日志
        }
    }
    else
    {
        LOG_DEBUG("[Node=%s] LeaderCommit=%u <= LocalCommit=%u, no update needed",
                  node_id.c_str(),
                  data.leader_commit,
                  persistent_state_.commit_index_.load());
    }

    // 5. 发送成功响应
    if (follower_client_)
    {
        uint32_t last_idx = log_array_->get_last_index();
        RaftMessage response = RaftMessage::append_entries_response(persistent_state_.current_term_.load(), true, last_idx);
        follower_client_->send(response);
    }

    return true;
}

// 处理RequestVote请求
bool RaftNode::handle_request_vote(const RaftMessage &msg, const socket_t &client_sock)
{
    std::string node_id = get_node_id();
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!msg.request_vote_data)
    {
        LOG_ERROR("[Node=%s] RequestVote message missing data", node_id.c_str());
        return false;
    }

    const auto &data = *msg.request_vote_data;
    uint32_t candidate_term = msg.term;
    uint32_t my_term = persistent_state_.current_term_.load();

    // 获取Candidate地址
    Address opposite_addr;
    if (!get_opposite_address(client_sock, opposite_addr))
    {
        LOG_ERROR("[Node=%s] Failed to get candidate address", node_id.c_str());
        return false;
    }

    LOG_INFO("[Node=%s][Role=%s][Term=%u] Received RequestVote from %s, CandidateTerm=%u, LastLogIndex=%u, LastLogTerm=%u",
             node_id.c_str(),
             role_to_string(role_.load()),
             my_term,
             opposite_addr.to_string().c_str(),
             candidate_term,
             data.last_log_index,
             data.last_log_term);

    // 准备响应消息
    RaftMessage response;

    // --- 规则 1: 如果对方任期比我小，拒绝 ---
    if (candidate_term < my_term)
    {
        LOG_WARN("[Node=%s] VOTE REJECTED for %s (candidate term %u < my term %u)",
                 node_id.c_str(),
                 opposite_addr.to_string().c_str(),
                 candidate_term,
                 my_term);
        response = RaftMessage::request_vote_response(my_term, false);
        send(response, client_sock);
        return false;
    }

    // --- 规则 2: 如果对方任期比我大，我更新任期并转为 Follower ---

    if (candidate_term > my_term)
    {
        LOG_INFO("[Node=%s] TERM UPDATE: %u -> %u (from candidate %s)",
                 node_id.c_str(),
                 my_term,
                 candidate_term,
                 opposite_addr.to_string().c_str());
        persistent_state_.current_term_ = candidate_term;
        persistent_state_.voted_for_ = Address(); // 新任期重置投票
        save_persistent_state();

        to_follower();

        // 更新本地变量以便后续判断
        my_term = candidate_term;
    }

    // --- 规则 3: 检查日志新旧 (Log Freshness) ---
    // Raft 限制：Candidate 的日志必须至少和 Follower 一样新
    uint32_t my_last_index, my_last_term;
    log_array_->get_last_info(my_last_index, my_last_term);

    bool log_is_ok = false;
    if (data.last_log_term > my_last_term)
    {
        log_is_ok = true;
    }
    else if (data.last_log_term == my_last_term && data.last_log_index >= my_last_index)
    {
        log_is_ok = true;
    }

    LOG_DEBUG("[Node=%s] Log freshness check: CandidateLog=(index=%u, term=%u), MyLog=(index=%u, term=%u), Result=%s",
              node_id.c_str(),
              data.last_log_index,
              data.last_log_term,
              my_last_index,
              my_last_term,
              log_is_ok ? "OK" : "REJECTED");

    // --- 规则 4: 检查是否已经投过票 ---
    // voted_for 为空，或者已经投给了这个 Candidate（幂等性）
    bool can_vote = (persistent_state_.voted_for_.is_null() || persistent_state_.voted_for_ == opposite_addr);

    if (!can_vote)
    {
        LOG_DEBUG("[Node=%s] Already voted for %s in term %u",
                  node_id.c_str(),
                  persistent_state_.voted_for_.to_string().c_str(),
                  my_term);
    }

    if (can_vote && log_is_ok)
    {
        // 投票成功
        persistent_state_.voted_for_ = opposite_addr;
        // 重置选举超时时间，避免我立刻发起选举
        last_heartbeat_time_ = std::chrono::steady_clock::now();
        election_timeout_ = generate_election_timeout();
        save_persistent_state();
        LOG_INFO("[Node=%s] VOTE GRANTED for %s in term %u",
                 node_id.c_str(),
                 opposite_addr.to_string().c_str(),
                 my_term);
        response = RaftMessage::request_vote_response(my_term, true);
        send(response, client_sock);
        return true;
    }
    else
    {
        // 拒绝投票
        LOG_WARN("[Node=%s] VOTE REJECTED for %s in term %u - Reason: %s",
                 node_id.c_str(),
                 opposite_addr.to_string().c_str(),
                 my_term,
                 can_vote ? "Log not fresh" : "Already voted");
        response = RaftMessage::request_vote_response(my_term, false);
        send(response, client_sock);
        return false;
    }
}

// 应用已提交的日志到状态机
void RaftNode::apply_committed_entries()
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    apply_committed_entries_nolock();
}

void RaftNode::apply_committed_entries_nolock()
{
    std::string node_id = get_node_id();
    // 假设调用者已持有 mutex_
    uint32_t commit_index = persistent_state_.commit_index_.load();
    uint32_t last_applied = persistent_state_.last_applied_.load();

    if (last_applied >= commit_index)
    {
        LOG_INFO("[Node=%s] No logs to apply: LastApplied=%u, CommitIndex=%u",
                 node_id.c_str(),
                 last_applied,
                 commit_index);
        return;
    }

    uint32_t apply_count = commit_index - last_applied;
    LOG_INFO("[Node=%s][Role=%s] APPLYING LOGS: %u entries (Index %u -> %u)",
             node_id.c_str(),
             role_to_string(role_.load()),
             apply_count,
             last_applied + 1,
             commit_index);

    while (last_applied < commit_index)
    {
        last_applied++;

        LogEntry entry;
        Response result;

        persistent_state_.last_applied_.store(last_applied);

        if (log_array_->get(last_applied, entry))
        {
            // 执行实际的业务逻辑
            result = execute_command(entry.cmd);
            result.request_id_ = entry.request_id;
            // 只记录摘要信息，避免打印完整命令（特别是批量命令会非常长）
            size_t cmd_len = entry.cmd.length();
            std::string cmd_summary = cmd_len > 100 ? entry.cmd.substr(0, 100) + "..." : entry.cmd;
            LOG_DEBUG("[Node=%s] Applied log index %u (term %u), cmd_len=%zu, cmd: %s",
                      node_id.c_str(),
                      last_applied,
                      entry.term,
                      cmd_len,
                      cmd_summary.c_str());
            // 只在错误时打印详细信息
            if (result.code_ == 0)
            {
                LOG_ERROR("[Node=%s] Failed to apply log %u: %s", node_id.c_str(), last_applied, result.error_msg_.c_str());
            }
            // 如果是写操作且带有 request_id，将结果写入缓存
            if (!entry.request_id.empty())
            {
                // 插入缓存
                result_cache_.put(entry.request_id, result);
                LOG_DEBUG("[Node=%s] Cached result for request_id: %s",
                          node_id.c_str(),
                          entry.request_id.c_str());
            }
        }
        else
        {
            LOG_ERROR("[Node=%s] Failed to read log at apply index %u", node_id.c_str(), last_applied);
            result = Response::error("log read error");
        }

        // Leader应通知等待的客户端
        if (role_ == RaftRole::Leader)
        {
            notify_request_applied(last_applied, result);
        }
    }

    // 批量保存一次状态（优化）
    save_persistent_state();
    LOG_INFO("[Node=%s] Log application completed: LastApplied=%u", node_id.c_str(), last_applied);
}

// 尝试提交日志
void RaftNode::try_commit_entries()
{
    std::string node_id = get_node_id();
    // 需持有锁以读取 match_index_ 等状态
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    uint32_t commit_index = persistent_state_.commit_index_.load();
    uint32_t last_log_index = log_array_->get_last_index();

    // 如果没有新日志，不需要计算
    if (commit_index >= last_log_index)
        return;

    LOG_DEBUG("[Node=%s][Role=Leader][Term=%u] Checking commit eligibility: CommitIndex=%u, LastLogIndex=%u",
              node_id.c_str(),
              persistent_state_.current_term_.load(),
              commit_index,
              last_log_index);

    // 1. 收集所有节点的 match_index
    // 包括 Leader 自己 (match_index = last_log_index)
    std::vector<uint32_t> match_indexes;
    match_indexes.push_back(last_log_index);

    for (const auto &pair : match_index_)
    {
        match_indexes.push_back(pair.second);
    }

    // 2. 排序（降序）
    std::sort(match_indexes.begin(), match_indexes.end(), std::greater<uint32_t>());

    // 3. 找到中间位置的索引 (Majority Index)
    // 例如 3 个节点，indices[1] 是中位数; 5 个节点，indices[2] 是中位数
    // cluster_size 包括 leader 和 followers
    size_t cluster_size = match_indexes.size();
    uint32_t majority_index = match_indexes[cluster_size / 2];

    // 4. 检查条件并更新 CommitIndex
    bool advanced = false;
    if (majority_index > commit_index)
    {
        uint32_t log_term = log_array_->get_term(majority_index);
        uint32_t current_term = persistent_state_.current_term_.load();

        if (log_term == current_term)
        {
            // 减少日志输出，只记录关键信息
            if (commit_index % 100 == 0)
            {
                LOG_DEBUG("[Node=%s][Role=Leader][Term=%u] COMMIT ADVANCED: %u -> %u (Majority Index), LogTerm=%u",
                          node_id.c_str(),
                          current_term,
                          commit_index,
                          majority_index,
                          log_term);
            }

            persistent_state_.commit_index_.store(majority_index);
            save_persistent_state(); // 持久化
            advanced = true;
        }
        else
        {
            LOG_DEBUG("[Node=%s] Cannot commit index %u: log_term=%u != current_term=%u",
                      node_id.c_str(),
                      majority_index,
                      log_term,
                      current_term);
        }
    }

    // 只要有已提交但未应用的日志，就尝试应用
    if (advanced || persistent_state_.commit_index_ > persistent_state_.last_applied_)
    {
        apply_committed_entries_nolock();
    }
}

void RaftNode::notify_request_applied(uint32_t index, const Response &response)
{
    std::lock_guard<std::mutex> lock(pending_requests_mutex_);
    auto it = pending_requests_.find(index);
    if (it != pending_requests_.end())
    {
        // 设置 Promise 的值，唤醒 submit_command
        try
        {
            it->second->promise.set_value(response);
        }
        catch (...)
        {
            LOG_WARN("Failed to set promise value for index %u", index);
        }
        pending_requests_.erase(it);
    }
}
// 停止接受写请求
bool RaftNode::stop_accepting_writes()
{
    LOG_INFO("Stopping write requests");
    if (role_ != RaftRole::Leader)
    {
        LOG_ERROR("Not a leader, cannot stop accepting writes");
        return false;
    }
    writable_.store(false, std::memory_order_relaxed);
    return true;
}

// 开始允许写操作
bool RaftNode::start_accepting_writes()
{
    LOG_INFO("Stopping write requests");
    if (role_ != RaftRole::Leader)
    {
        LOG_ERROR("Not a leader, cannot stop accepting writes");
        return false;
    }
    writable_.store(true, std::memory_order_relaxed);
    return true;
}
// 处理探查leader请求
void RaftNode::handle_query_leader(const RaftMessage &msg, const socket_t &client_sock)
{
    Address leader_addr;
    RaftMessage response_msg;
    Address opposite_addr;
    if (get_opposite_address(client_sock, opposite_addr) && is_trust(opposite_addr))
    {
        if (role_ == RaftRole::Leader)
        {
            get_self_address(client_sock, leader_addr);
        }
        else
        {
            leader_addr = persistent_state_.cluster_metadata_.current_leader_;
        }
    }
    else
    {
        LOG_WARN("Untrusted node attempted to query leader");
    }
    response_msg = RaftMessage::query_leader_response(leader_addr);
    send(response_msg, client_sock);
    close_socket(client_sock);
}

// 处理新节点加入请求
void RaftNode::handle_join_cluster(const RaftMessage &msg, const socket_t &client_sock)
{
    if (role_ != RaftRole::Leader)
    {
        LOG_WARN("Only leader can handle JoinCluster");
        RaftMessage response_msg = RaftMessage::query_leader_response(persistent_state_.cluster_metadata_.current_leader_);
        send(response_msg, client_sock);
        close_socket(client_sock);
        return;
    }

    if (!msg.join_cluster_data.has_value())
    {
        LOG_ERROR("JoinCluster message missing data");
        close_socket(client_sock);
        return;
    }

    const auto &data = *msg.join_cluster_data;

    RaftMessage response_msg;
    response_msg.type = RaftMessageType::JOIN_CLUSTER_RESPONSE;
    response_msg.term = persistent_state_.current_term_.load();
    JoinClusterResponseData response;
    response.success = false;
    // response_msg.join_cluster_response_data will be set before sending
    // 添加新节点到集群
    Address new_node_addr;
    if (get_opposite_address(client_sock, new_node_addr))
    {
        // 判断节点是否受信任
        if (!is_trust(new_node_addr))
        {
            LOG_WARN("New node %s is not trusted, rejecting JoinCluster", new_node_addr.to_string().c_str());
            response.error_message = "Node is not trusted";
            response_msg.join_cluster_response_data = response;
            send(response_msg, client_sock);
            close_socket(client_sock);
            return;
        }
        new_node_addr.port = data.port;
        // 广播有新节点加入
        auto broadcast_msg = RaftMessage::new_node_join_broadcast(new_node_addr);
        broadcast_to_followers(broadcast_msg);
        // 发送集群必要信息
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            if (follower_address_map_.count(new_node_addr) > 0)
            {
                match_index_.erase(follower_address_map_[new_node_addr]);
                next_index_.erase(follower_address_map_[new_node_addr]);
            }
            // 根据 Follower 汇报的 last_index 设置 next_index
            // Follower 说它有到 index 100 的日志，那我们下一次尝试发 101
            next_index_[client_sock] = data.last_index + 1;
            match_index_[client_sock] = 0;
        }
        response.success = true;
        response.cluster_metadata = persistent_state_.cluster_metadata_; // 发送最新集群配置
        response.cluster_metadata.current_leader_ = Address();
        response.sync_from_index = next_index_[client_sock];
        response_msg.join_cluster_response_data = response;
        send(response_msg, client_sock);
        // 3. 触发一次同步检测
        // 异步触发，不要阻塞当前线程
        std::thread([this, client_sock]()
                    { this->trigger_log_sync(client_sock); })
            .detach();
        // 更新集群配置
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            persistent_state_.cluster_metadata_.cluster_nodes_.insert(new_node_addr);
            save_persistent_state();
        }
        // 添加到follower_sockets
        {
            std::unique_lock<std::shared_mutex> lock(follower_sockets_mutex_);
            follower_sockets_.insert(client_sock);
            follower_address_map_[new_node_addr] = client_sock;
        }
        // 添加到io多路复用
        add_socket_to_epoll(client_sock);
        LOG_INFO("New node joined: %s", new_node_addr.to_string().c_str());
    }
    else
    {
        LOG_ERROR("Failed to get opposite address for JoinCluster");
        response.error_message = "Failed to get opposite address";
        send(response_msg, client_sock);
        close_socket(client_sock);
        return;
    }
}
void RaftNode::trigger_log_sync(socket_t sock)
{
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    uint32_t next_idx = next_index_[sock];
    uint32_t base_idx = log_array_->get_base_index();

    if (next_idx < base_idx)
    {
        // 场景：Follower 落后太多，日志已被 Leader 截断
        // 必须通过快照同步

        // 记录该 Socket 的快照发送状态
        snapshot_state_[sock] = {0, false}; // offset = 0, is_sending = false
        send_snapshot_chunk(sock);
    }
    else
    {
        // 场景：日志存在，走正常的 AppendEntrie
        send_append_entries_nolock(sock);
    }
}
// 发送快照块
void RaftNode::send_snapshot_chunk(socket_t sock)
{
    static Storage *storage_ = Storage::get_instance();
    // 注意：这里需要更精细的锁控制，避免长时间持有 mutex_ 读取文件
    SnapshotTransferState *state = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        state = &snapshot_state_[sock];
    }

    if (!state->is_sending)
    {
        // 第一次发送，打开文件
        std::string snapshot_path;
        // 生成快照文件
        if (!storage_->create_checkpoint(snapshot_path, result_cache_.serialize()))
        {
            LOG_ERROR("Failed to create storage checkpoint");
            return;
        }
        if (snapshot_path.empty())
        {
            LOG_ERROR("Snapshot path is empty");
            return;
        }
        state->fp = fopen(snapshot_path.c_str(), "rb");
        if (!state->fp)
        {
            LOG_ERROR("Failed to open snapshot file");
            return;
        }
        // 捕获快照元数据
        // 因为快照是基于当前状态机生成的，所以它对应的 Log Index 就是 last_applied_
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            state->last_included_index = persistent_state_.last_applied_.load();
            state->last_included_term = log_array_->get_term(state->last_included_index);

            // 如果日志已经被截断，get_term 可能失败。
            // 但作为 Leader，生成当前快照时，通常意味着该 Index 的任期记录应该还存在，
            // 或者我们可以取 current_term (如果刚好是最新)，或者从 persistent_state 记录的 snapshot_index 取。
            // 这里为了健壮性，如果取不到，暂且取 persistent_state_.current_term_
            if (state->last_included_term == 0 && state->last_included_index > 0)
            {
                LOG_WARN("Could not get term for snapshot index %u, using current term", state->last_included_index);
                state->last_included_term = persistent_state_.current_term_.load();
            }
        }

        state->is_sending = true;
        state->offset = 0;
        state->snapshot_path = snapshot_path;

        LOG_INFO("Started snapshot transfer to %d. File: %s, Index: %u, Term: %u",
                 sock, snapshot_path.c_str(), state->last_included_index, state->last_included_term);
    }

    // 缓冲区大小
    const size_t CHUNK_SIZE = config_.snapshot_chunk_size_bytes;
    std::vector<char> buffer(CHUNK_SIZE);

    // 定位文件指针
    fseek(state->fp, state->offset, SEEK_SET);
    size_t bytes_read = fread(buffer.data(), 1, CHUNK_SIZE, state->fp);

    bool done = false;
    if (bytes_read < CHUNK_SIZE)
    {
        // 读到了文件末尾
        done = true;
        fclose(state->fp);
        state->fp = nullptr;
        state->is_sending = false; // 发送完毕
        // 删除快照文件
        storage_->remove_snapshot(state->snapshot_path);
    }

    // 构造 InstallSnapshot 消息
    std::string data_chunk(buffer.data(), bytes_read);

    RaftMessage msg = RaftMessage::snapshot_install(
        data_chunk,
        state->offset,
        done,
        state->last_included_index,
        state->last_included_term);

    // 更新 offset 用于下一块
    state->offset += bytes_read;

    send(msg, sock);
    LOG_INFO("Sent snapshot chunk to %d: offset=%lu, size=%zu, done=%d",
             sock, state->offset - bytes_read, bytes_read, done);
}

void RaftNode::handle_install_snapshot(const RaftMessage &msg)
{
    const auto &data = *msg.snapshot_install_data;

    // 1. 定义临时文件路径
    std::string temp_path = PathUtils::combine_path(root_dir_, "snapshot.tar.gz");

    // 2. 判断offset是否小于文件大小，满足则直接发送响应返回
    uint64_t file_size = get_file_size(temp_path);
    if (data.offset < file_size)
    {
        LOG_INFO("Offset %lu is less than file size %lu, skip writing and send response directly",
                 data.offset, file_size);
        RaftMessage response = RaftMessage::snapshot_install_response(true, data.offset);
        send(response, follower_client_->get_socket());
        return;
    }

    // 3. 写入临时文件（使用智能指针RAII管理文件句柄，避免泄漏/重复关闭）
    UniqueFilePtr fp;
    if (data.offset == 0)
    {
        // 第一块，覆盖写
        fp = UniqueFilePtr(fopen(temp_path.c_str(), "wb"), FileCloser());
    }
    else
    {
        // 后续块，追加写
        fp = UniqueFilePtr(fopen(temp_path.c_str(), "ab"), FileCloser());
    }

    if (!fp)
    {
        LOG_ERROR("Failed to open snapshot temp file for writing: %s", temp_path.c_str());
        return;
    }

    // 写入数据
    size_t written_size = fwrite(data.snapshot_data.data(), 1, data.snapshot_data.size(), fp.get());
    if (written_size != data.snapshot_data.size())
    {
        LOG_ERROR("Failed to write all data to temp file, expected %zu, written %zu",
                  data.snapshot_data.size(), written_size);
        return;
    }

    // 4. 发送响应（智能指针会自动关闭文件句柄，无需手动fclose）
    RaftMessage response = RaftMessage::snapshot_install_response(true, data.offset);
    send(response, follower_client_->get_socket());

    // 5. 如果 done == true，应用快照
    if (data.done)
    {
        static Storage *storage_ = Storage::get_instance();
        std::string result_cache;
        if (storage_->restore_from_checkpoint(temp_path, result_cache))
        {
            uint32_t snapshot_index = data.last_included_index;
            uint32_t snapshot_term = data.last_included_term;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);

                // 1. 更新持久化状态
                persistent_state_.log_snapshot_index_ = snapshot_index;
                persistent_state_.commit_index_ = snapshot_index;
                persistent_state_.last_applied_ = snapshot_index;
                save_persistent_state();

                // 2. 重置日志数组，准备接收 snapshot_index + 1 的日志
                log_array_->reset(snapshot_index + 1);
            }
            if (!result_cache.empty())
            {
                size_t offset = 0;
                result_cache_.deserialize(result_cache.data(), offset);
            }
            LOG_INFO("Restore from checkpoint success: %s", temp_path.c_str());
        }
        else
        {
            LOG_ERROR("Failed to restore from checkpoint: %s", temp_path.c_str());
        }
        // 无需手动fclose(fp)！
    }
}

void RaftNode::handle_snapshot_response(const RaftMessage &msg, socket_t sock)
{
    const auto &resp = *msg.snapshot_install_response_data;
    std::unique_lock<std::recursive_mutex> lock(mutex_);
    if (snapshot_state_.count(sock) == 0)
    {
        return;
    }
    auto &snapshot_state = snapshot_state_[sock];
    if (resp.success && resp.offset == snapshot_state.offset)
    {
        if (snapshot_state.is_sending)
        {
            // 收到上一块的成功确认，发送下一块
            // 释放锁后再发送，避免IO阻塞锁
            lock.unlock();
            send_snapshot_chunk(sock);
        }
        else
        {
            // 快照彻底发送完毕，更新 next_index
            match_index_[sock] = snapshot_state.last_included_index;
            next_index_[sock] = snapshot_state.last_included_index + 1;
            snapshot_state_.erase(sock);
            LOG_INFO("Snapshot sync completed for node %d", sock);
        }
    }
    else
    {
        LOG_ERROR("Snapshot chunk failed or offset mismatch for node %d", sock);
    }
}

// 处理AppendEntries响应
void RaftNode::handle_append_entries_response(const RaftMessage &msg, const socket_t &sock)
{
    std::string node_id = get_node_id();

    if (!msg.append_entries_response_data)
    {
        LOG_ERROR("[Node=%s] AppendEntriesResponse message missing data", node_id.c_str());
        return;
    }

    const auto &response = *msg.append_entries_response_data;

    if (response.success)
    {
        // 复制成功，更新match_index和next_index
        uint32_t old_match, new_match;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            old_match = match_index_[sock];
            match_index_[sock] = std::max(match_index_[sock], response.log_index);
            new_match = match_index_[sock];
            next_index_[sock] = match_index_[sock] + 1;
        }

        LOG_DEBUG("[Node=%s][Role=Leader][Term=%u] LOG REPLICATION SUCCESS: Socket=%d, MatchIndex: %u -> %u",
                  node_id.c_str(),
                  persistent_state_.current_term_.load(),
                  sock,
                  old_match,
                  new_match);

        // 检查是否可以提交
        try_commit_entries();
    }
    else
    {
        // 复制失败，回退next_index
        uint32_t old_next, new_next;
        {
            std::lock_guard<std::recursive_mutex> lock(mutex_);
            old_next = next_index_[sock];
            if (next_index_[sock] > 0)
            {
                next_index_[sock]--;
            }
            new_next = next_index_[sock];
        }

        LOG_WARN("[Node=%s][Role=Leader] LOG REPLICATION FAILED: Socket=%d, NextIndex: %u -> %u, Retrying",
                 node_id.c_str(),
                 sock,
                 old_next,
                 new_next);

        // 重试
        send_append_entries(sock);
    }
}

// 处理RequestVote响应
void RaftNode::handle_request_vote_response(const RaftMessage &msg)
{
    std::string node_id = get_node_id();
    std::unique_lock<std::recursive_mutex> lock(mutex_);

    uint32_t my_term = persistent_state_.current_term_.load();

    // 1. 忽略旧任期的响应
    if (msg.term < my_term)
    {
        LOG_DEBUG("[Node=%s] Ignored RequestVoteResponse from old term %u (current: %u)",
                  node_id.c_str(),
                  msg.term,
                  my_term);
        return;
    }

    // 2. 如果对方任期比我大，我立刻退回 Follower
    if (msg.term > my_term)
    {
        LOG_WARN("[Node=%s][Role=%s] RECEIVED HIGHER TERM %u in vote response, stepping down to Follower",
                 node_id.c_str(),
                 role_to_string(role_.load()),
                 msg.term);
        persistent_state_.current_term_ = msg.term;
        persistent_state_.voted_for_ = Address();
        save_persistent_state();
        to_follower();
        return; // 退出
    }

    // 3. 只有 Candidate 才会统计选票
    if (role_ != RaftRole::Candidate)
    {
        LOG_DEBUG("[Node=%s] Not a Candidate, ignoring RequestVoteResponse", node_id.c_str());
        return;
    }

    if (!msg.request_vote_response_data)
        return;

    size_t cluster_size = persistent_state_.cluster_metadata_.cluster_nodes_.size() + 1; // +1 for self
    size_t majority = cluster_size / 2 + 1;

    if (msg.request_vote_response_data->vote_granted)
    {
        granted_votes_++;
        LOG_INFO("[Node=%s][Role=Candidate][Term=%u] VOTE GRANTED - Votes: %u/%zu (Need %zu for majority)",
                 node_id.c_str(),
                 my_term,
                 granted_votes_,
                 cluster_size,
                 majority);

        // 检查是否获得多数派
        if (granted_votes_ >= majority)
        {
            LOG_INFO("[Node=%s][Term=%u] ELECTION WON! Received %zu votes (majority: %zu), becoming Leader",
                     node_id.c_str(),
                     my_term,
                     granted_votes_,
                     majority);
            //  记录下当前的集群节点数组和 Term
            std::unordered_set<Address> cluster_nodes = persistent_state_.cluster_metadata_.cluster_nodes_;
            uint32_t leader_term = my_term; // 保存成为 Leader 时的 Term
            become_leader();
            // 重新赋值给持久化状态
            persistent_state_.cluster_metadata_.cluster_nodes_ = cluster_nodes;
            save_persistent_state();
            // 广播NEW_MASTER消息通知所有已知节点
            // 注意：在广播前检查 Term 和角色是否一致，避免竞态条件
            LOG_INFO("[Node=%s][Term=%u] Broadcasting NEW_MASTER message to cluster",
                     node_id.c_str(),
                     leader_term);
            std::thread([this, leader_term]()
                        { this->broadcast_new_master_with_check(leader_term); })
                .detach();
        }
    }
    else
    {
        LOG_INFO("[Node=%s][Role=Candidate][Term=%u] VOTE REJECTED - Current votes: %u/%zu (Need %zu)",
                 node_id.c_str(),
                 my_term,
                 granted_votes_,
                 cluster_size,
                 majority);
    }
}

// 执行命令
Response RaftNode::execute_command(const std::string &cmd)
{
    if (!Storage::is_init())
    {
        LOG_ERROR("Storage is not initialized");
        exit(1);
    }
    static Storage *storage_ = Storage::get_instance();
    std::vector<std::string> command_parts = split_by_spacer(cmd);
    if (command_parts.empty())
    {
        return Response::error("Empty command");
    }
    uint8_t operation = stringToOperationType(command_parts[0]);
    // 移除操作名，只保留参数
    command_parts.erase(command_parts.begin());
    return storage_->execute(operation, command_parts);
}

// 实现两阶段提交 - 提交命令
Response RaftNode::submit_command(const std::string &request_id, const std::string &cmd)
{
    // 0. 参数校验
    if (request_id.empty())
    {
        return Response::error("request_id cannot be empty");
    }
    Response response;
    // 1. 幂等性检查：如果缓存里有，直接返回
    if (result_cache_.get(request_id, response))
    {
        LOG_INFO("Duplicate request_id %s found, returning cached response", request_id.c_str());
        return response;
    }
    static Storage *storage_ = Storage::get_instance();
    // 解析命令并执行
    std::vector<std::string> command_parts = split_by_spacer(cmd);
    if (command_parts.empty())
    {
        response = Response::error(std::string("invalid command"));
        response.request_id_ = request_id;
    }
    else
    {
        bool is_exec = false;
        response = handle_raft_command(command_parts, is_exec);
        if (is_exec)
        {
            response.request_id_ = request_id;
            return response;
        }
        uint8_t operation = stringToOperationType(command_parts[0]);
        if (isReadOperation(operation))
        {
            // 读操作直接执行
            command_parts.erase(command_parts.begin());
            response = storage_->execute(operation, command_parts);
            response.request_id_ = request_id;
        }
        else if (role_ != RaftRole::Leader)
        {
            response = Response::redirect(persistent_state_.cluster_metadata_.current_leader_.to_string());
            response.request_id_ = request_id;
        }
        else
        {
            std::shared_ptr<PendingRequest> pending_req = std::make_shared<PendingRequest>();

            // 1. 本地写入日志 (Phase 1 Start)
            LogEntry entry;
            entry.term = persistent_state_.current_term_.load();
            entry.cmd = cmd;
            entry.timestamp = get_current_timestamp();
            entry.command_type = 0;
            entry.request_id = request_id;
            if (!log_array_->append(entry))
            {
                LOG_ERROR("Failed to append log entry");
                return Response::error("execute failed");
            }
            // 执行了写命令
            if (!persistent_state_.cluster_metadata_.current_leader_.is_null())
            {
                std::lock_guard<std::recursive_mutex> lock(mutex_);
                persistent_state_.cluster_metadata_.current_leader_ = Address();
                save_persistent_state();
            }
            uint32_t log_index = entry.index;
            // 减少日志输出频率，只在批量提交时打印
            if (log_index % 100 == 0 || log_index == log_array_->get_last_index())
            {
                LOG_DEBUG("Leader appended log index %u, term %u, waiting for commit...", log_index, entry.term);
            }

            // 2. 注册等待通知
            {
                std::lock_guard<std::mutex> pending_lock(pending_requests_mutex_);
                pending_requests_[log_index] = pending_req;
            }

            // 3. 广播给 Followers
            {
                std::shared_lock<std::shared_mutex> sock_lock(follower_sockets_mutex_);
                for (const auto &sock : follower_sockets_)
                {
                    send_append_entries(sock);
                }
            }
            // 4. 先调用一次try_commit_entries
            try_commit_entries();
            // 5. 阻塞等待结果
            std::future_status status = pending_req->future.wait_for(
                std::chrono::milliseconds(config_.submit_command_timeout_ms));
            if (status == std::future_status::ready)
            {
                return pending_req->future.get();
            }
            else
            {
                // 超时处理
                std::lock_guard<std::mutex> pending_lock(pending_requests_mutex_);
                pending_requests_.erase(log_index);
                response = Response::error("command timeout");
                response.request_id_ = request_id;
            }
        }
    }
    return response;
}

Response RaftNode::handle_raft_command(const std::vector<std::string> &command_parts, bool &is_exec)
{
    Response response;
    is_exec = true;
    auto &leader_address_ = persistent_state_.cluster_metadata_.current_leader_;
    if (command_parts[0] == "get_master")
    {
        if (command_parts.size() == 1)
        {
            // 返回当前leader地址
            if (leader_address_.is_null() || role_ == RaftRole::Leader)
            {
                response = Response::error("no leader elected");
            }
            else
            {
                response = Response::success(leader_address_.to_string());
            }
        }
        else
        {
            response = Response::error(std::string("invalid command"));
        }
    }
    else if (command_parts[0] == "set_master")
    {
        if (command_parts.size() == 3)
        {
            // 设置当前leader地址（仅限leader节点调用）
            if (role_ != RaftRole::Leader)
            {
                response = Response::error("only leader can set master");
                return response;
            }
            std::string host = command_parts[1];
            int port = std::stoi(command_parts[2]);
            // 连接到主节点
            Address address(host, port);
            if (!persistent_state_.cluster_metadata_.current_leader_.is_null() && address == persistent_state_.cluster_metadata_.current_leader_)
            {
                if (follower_client_ != nullptr)
                {
                    response = Response::success("already connected to master");
                }
                else
                {
                    response = Response::success(become_follower(address, persistent_state_.current_term_.load(), true, persistent_state_.commit_index_.load()));
                }
            }
            else
            {
                // 清空现有的数据，避免脏数据
                static Storage *storage_ = Storage::get_instance();
                // 设置log_array_为空
                log_array_ = nullptr;
                if (!storage_->clear_and_backup_data())
                {
                    log_array_ = std::make_unique<RaftLogArray>(
                        PathUtils::combine_path(root_dir_, ".raft"),
                        config_.log_config);
                    response = Response::error("clear data failed");
                    return response;
                }
                log_array_ = std::make_unique<RaftLogArray>(
                    PathUtils::combine_path(root_dir_, ".raft"),
                    config_.log_config);
                {
                    std::lock_guard<std::recursive_mutex> lock(mutex_);
                    persistent_state_.last_applied_.store(0);
                    persistent_state_.commit_index_.store(0);
                    persistent_state_.current_term_.store(0);
                    persistent_state_.log_snapshot_index_.store(0);
                    save_persistent_state();
                }
                response = Response::success(become_follower(address));
            }
        }
        else
        {
            response = Response::error(std::string("invalid command"));
        }
    }
    else if (command_parts[0] == "remove_node")
    {
        if (command_parts.size() == 3)
        {
            if (role_ != RaftRole::Leader)
            {
                response = Response::error("only leader can remove node");
                return response;
            }
            std::string ip = command_parts[1];
            uint16_t port = std::stoi(command_parts[2]);
            // 从集群移除节点
            Address node_to_remove(ip, port);
            if (remove_node(node_to_remove))
            {
                response = Response::success(std::string("node removed"));
            }
            else
            {
                response = Response::error(std::string("node not found"));
            }
        }
        else
        {
            response = Response::error(std::string("invalid command"));
        }
    }
    else if (command_parts[0] == "list_nodes")
    {
        if (command_parts.size() == 1)
        {
            auto &cluster_nodes_ = persistent_state_.cluster_metadata_.cluster_nodes_;
            std::string nodes_str;
            for (const auto &node : cluster_nodes_)
            {
                if (node.is_null())
                {
                    continue;
                }
                if (!nodes_str.empty())
                    nodes_str += ",";
                nodes_str += node.to_string();
            }
            if (!nodes_str.empty())
                nodes_str += ",";
            nodes_str += "current_node";
            response = Response::success(nodes_str);
        }
        else
        {
            response = Response::error(std::string("invalid command"));
        }
    }
    else if (command_parts[0] == "get_status")
    {
        if (command_parts.size() == 1)
        {
            std::string status = "role=" + std::to_string(static_cast<int>(role_.load())) +
                                 ",term=" + std::to_string(persistent_state_.current_term_.load()) +
                                 ",leader=" + leader_address_.to_string() +
                                 ",nodes=" + std::to_string(persistent_state_.cluster_metadata_.cluster_nodes_.size() + 1) +
                                 ",commit=" + std::to_string(persistent_state_.commit_index_.load()) +
                                 ",applied=" + std::to_string(persistent_state_.last_applied_.load()) +
                                 ",writable=" + std::to_string(writable_);
            response = Response::success(status);
        }
        else
        {
            response = Response::error(std::string("invalid command"));
        }
    }
    else
    {
        is_exec = false;
    }
    return response;
}

void RaftNode::add_new_connection(socket_t client_sock, const sockaddr_in &client_addr)
{
    // 接受连接,但不添加到io多路复用，因为后续消息需要超时等待响应
    if (current_connections_ < max_connections_)
    {
        // 连接数未满，直接接受
        set_non_blocking(client_sock);
        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, clientIp, INET_ADDRSTRLEN);
        Address client_address(clientIp, ntohs(client_addr.sin_port));
        if (!is_trust(client_address))
        {
            LOG_WARN("Connection from %s rejected: not in trust list", client_address.to_string().c_str());
            CLOSE_SOCKET(client_sock);
            return;
        }
        LOG_INFO("New connection accepted: %s:%d", clientIp, ntohs(client_addr.sin_port));

        current_connections_++;
        {
            std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
            sockets_.insert(client_sock);
        }
        // 无论是主从，被主动连接都需要在一定时间安内接收到响应
        ProtocolBody *msg = new_body();
        int ret = receive(*msg, client_sock, raft_msg_timeout_);
        if (ret < 0)
        {
            // 出现错误（可能是超时或连接关闭），关闭连接
            close_socket(client_sock);
            delete msg;
        }
        else
        {
            handle_request(msg, client_sock);
        }
    }
    else
    {
        // 等待队列已满，拒绝连接
        LOG_WARN("Connection rejected: active connections full");
        CLOSE_SOCKET(client_sock);
    }
}

void RaftNode::handle_request(ProtocolBody *body, socket_t client_sock)
{
    // 处理客户端请求
    RaftMessage *msg = dynamic_cast<RaftMessage *>(body);
    switch (msg->type)
    {
    case RaftMessageType::JOIN_CLUSTER:
        handle_join_cluster(*msg, client_sock);
        break;
    case RaftMessageType::QUERY_LEADER:
        handle_query_leader(*msg, client_sock);
        break;
    case RaftMessageType::APPEND_ENTRIES_RESPONSE:
        handle_append_entries_response(*msg, client_sock);
        break;
    case RaftMessageType::REQUEST_VOTE:
        handle_request_vote(*msg, client_sock);
        break;
    case RaftMessageType::SNAPSHOT_INSTALL_RESPONSE:
        handle_snapshot_response(*msg, client_sock);
        break;
    case RaftMessageType::NEW_MASTER:
        handle_new_master(*msg, client_sock);
        close_socket(client_sock);
        break;
    }
    delete body;
}

void RaftNode::broadcast_to_followers(const RaftMessage &msg)
{
    if (role_ != RaftRole::Leader)
    {
        LOG_WARN("Only leader can broadcast to followers");
        return;
    }
    std::shared_lock<std::shared_mutex> lock(follower_sockets_mutex_);
    for (const auto &sock : follower_sockets_)
    {
        int ret = send(msg, sock);
        if (ret < 0)
        {
            LOG_ERROR("Failed to send broadcast message to follower: %d", sock);
        }
    }
}

void RaftNode::close_socket(socket_t sock)
{
    int ret = shutdown(sock, SHUT_WR);
#ifdef _WIN32
    if (ret == SOCKET_ERROR)
    {
        LOG_ERROR("Shutdown error on fd %d: %s", sock, socket_error_to_string(errno));
    }
#else
    if (ret == -1)
    {
        LOG_ERROR("Shutdown error on fd %d: %s", sock, socket_error_to_string(errno));
    }
#endif
    CLOSE_SOCKET(sock);
#ifdef __APPLE__
#elif _WIN32
    FD_CLR(sock, &master_set_);
#elif __linux__
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, sock, NULL);
#endif

    if (current_connections_ > 0)
    {
        current_connections_--;
    }
    // 从sockets集合中移除
    {
        std::lock_guard<std::mutex> sockets_lock(sockets_mutex_);
        sockets_.erase(sock);
    }
    // 从follower_sockets集合中移除
    {
        std::lock_guard<std::shared_mutex> lock(follower_sockets_mutex_);
        follower_sockets_.erase(sock);
    }
}

bool RaftNode::remove_node(const Address &node_to_remove)
{
    auto &cluster_nodes_ = persistent_state_.cluster_metadata_.cluster_nodes_;
    auto it = std::find(cluster_nodes_.begin(), cluster_nodes_.end(), node_to_remove);
    if (it != cluster_nodes_.end())
    {
        socket_t sock = follower_address_map_[node_to_remove];
        next_index_.erase(sock);
        match_index_.erase(sock);
        RaftMessage node_remove_msg = RaftMessage::leave_node(node_to_remove);
        broadcast_to_followers(node_remove_msg);
        {
            std::unique_lock<std::shared_mutex> lock(follower_sockets_mutex_);
            follower_sockets_.erase(sock);
            follower_address_map_.erase(node_to_remove);
        }
        cluster_nodes_.erase(it);
        save_persistent_state();
        LOG_INFO("Removed node from cluster: %s", node_to_remove.to_string().c_str());
        return true;
    }
    else
    {
        LOG_WARN("Node not found in cluster: %s,cannot remove", node_to_remove.to_string().c_str());
        return false;
    }
}

bool RaftNode::is_trust(const Address &addr)
{
    return is_trusted_ip(addr.to_string(), trusted_nodes_);
}

// 广播新主节点消息到所有已知节点（带 Term 检查）
void RaftNode::broadcast_new_master_with_check(uint32_t expected_term)
{
    // 检查当前 Term 和角色是否与预期一致，避免竞态条件
    uint32_t current_term;
    RaftRole current_role;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        current_term = persistent_state_.current_term_.load();
        current_role = role_.load();
    }

    // 如果 Term 或角色不匹配，说明状态已改变，放弃广播
    if (current_term != expected_term)
    {
        LOG_WARN("[Node=%s] Term mismatch when broadcasting new master: expected %u, actual %u, skipping broadcast",
                 get_node_id().c_str(), expected_term, current_term);
        return;
    }

    if (current_role != RaftRole::Leader)
    {
        LOG_WARN("[Node=%s] Not leader when broadcasting new master (role: %s, term: %u), skipping broadcast",
                 get_node_id().c_str(), role_to_string(current_role), current_term);
        return;
    }

    // 检查通过，执行实际广播
    broadcast_new_master_impl();
}

// 广播新主节点消息到所有已知节点
void RaftNode::broadcast_new_master()
{
    if (role_ != RaftRole::Leader)
    {
        LOG_WARN("Only leader can broadcast new master message");
        return;
    }

    // 调用实际的广播实现
    broadcast_new_master_impl();
}

// 广播新主节点消息的实际实现
void RaftNode::broadcast_new_master_impl()
{
    // 获取当前状态，避免长时间持锁
    uint32_t current_term;
    std::vector<Address> nodes;
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        current_term = persistent_state_.current_term_.load();
        // 复制一份节点列表
        for (const auto &node : persistent_state_.cluster_metadata_.cluster_nodes_)
        {
            nodes.push_back(node);
        }
    }

    if (nodes.empty())
    {
        LOG_INFO("No cluster nodes to broadcast new master message");
        return;
    }

    LOG_INFO("Broadcasting NEW_MASTER message to %zu nodes, term: %u", nodes.size(), current_term);

    // 向所有已知节点发送NEW_MASTER消息
    for (const auto &node : nodes)
    {
        bool is_submitted = thread_pool_->submit([node, current_term, this]()
                                                 {
            TCPClient client(node.host, node.port);
            try
            {
                client.connect();

                // 获取本地地址作为leader地址
                Address leader_addr;
                if (!get_self_address(client.get_socket(), leader_addr))
                {
                    LOG_ERROR("Failed to get self address for NEW_MASTER broadcast");
                    return;
                }
                leader_addr.port = this->port_;

                RaftMessage msg = RaftMessage::new_master(current_term, leader_addr);
                int ret = client.send(msg);
                if (ret <= 0)
                {
                    LOG_WARN("Failed to send NEW_MASTER message to %s", node.to_string().c_str());
                }
                else
                {
                    LOG_INFO("Sent NEW_MASTER message to %s", node.to_string().c_str());
                }
            }
            catch (const std::exception &e)
            {
                LOG_WARN("Failed to broadcast NEW_MASTER to %s: %s", node.to_string().c_str(), e.what());
            } });
        if (!is_submitted)
        {
            LOG_WARN("Failed to submit NEW_MASTER broadcast task for node: %s", node.to_string().c_str());
        }
    }
}

// 处理新主节点选举成功消息
void RaftNode::handle_new_master(const RaftMessage &msg, const socket_t &client_sock)
{
    if (!msg.new_master_data.has_value())
    {
        LOG_WARN("NEW_MASTER message missing data");
        return;
    }

    const auto &data = *msg.new_master_data;
    uint32_t leader_term = msg.term;
    uint32_t my_term = persistent_state_.current_term_.load();

    LOG_INFO("Received NEW_MASTER message from %s, term: %u", data.leader_address.to_string().c_str(), leader_term);

    // 如果消息的任期比我小，忽略
    if (leader_term < my_term)
    {
        LOG_WARN("Ignoring NEW_MASTER from old term %u (my term: %u)", leader_term, my_term);
        return;
    }

    // 如果我已经是Leader且任期相同，忽略（避免脑裂）
    if (role_ == RaftRole::Leader && leader_term == my_term)
    {
        LOG_WARN("Ignoring NEW_MASTER, I am already leader in term %u", my_term);
        return;
    }

    // 获取本机地址，避免连接自己
    Address self_addr;
    // 获取自己的地址
    if (!get_self_address(client_sock, self_addr))
    {
        LOG_ERROR("Failed to get self address for NEW_MASTER");
        return;
    }
    if (self_addr == data.leader_address)
    {
        LOG_WARN("Ignoring NEW_MASTER, self address is the same as leader address");
        return;
    }
    // 更新任期并连接新Leader
    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (leader_term > my_term)
        {
            persistent_state_.current_term_ = leader_term;
            persistent_state_.voted_for_ = Address(); // 重置投票
        }
        persistent_state_.cluster_metadata_.current_leader_ = data.leader_address;
        save_persistent_state();
    }

    LOG_INFO("Connecting to new leader: %s", data.leader_address.to_string().c_str());

    // 转换为Follower并连接新Leader
    become_follower(data.leader_address, leader_term, true, persistent_state_.commit_index_.load());
}