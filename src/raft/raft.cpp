#include "raft/raft.h"
#include "logger/logger.h"
#include <algorithm>
#include <sys/stat.h>
#include "storage/storage.h"
#include "common/types/operation_type.h"
#include "common/util/path_utils.h"

#ifdef _WIN32
#include <io.h>
#include <direct.h>
#define mkdir _mkdir
#else
#include <unistd.h>
#endif

std::unique_ptr<RaftNode> RaftNode::instance_ = nullptr;
bool RaftNode::is_init_ = false;

// RaftNode构造函数 - 实现完整的初始化流程
RaftNode::RaftNode(const std::string root_dir, const std::string &ip, const u_short port, const uint32_t max_follower_count, const std::string password)
    : TCPServer(ip, port, max_follower_count, 0, 0),
      password_(password),
      log_array_(PathUtils::CombinePath(root_dir, ".raft")),
      root_dir_(root_dir),
      role_(RaftRole::Leader),
      rng_(std::chrono::steady_clock::now().time_since_epoch().count())
{
    // 1. 打开元数据文件
    std::string meta_path = PathUtils::CombinePath(PathUtils::CombinePath(root_dir, ".raft"), ".raft_meta");
    metadata_file_ = fopen(meta_path.c_str(), "ab+");
    if (metadata_file_ == nullptr)
    {
        LOG_ERROR("Failed to open metadata file: %s", meta_path.c_str());
        throw std::runtime_error("Failed to open metadata file: " + meta_path);
    }

    // 2. 加载持久化状态
    load_persistent_state();

    // 3. 根据场景选择初始化策略
    if (!persistent_state_.cluster_metadata_.current_leader_.is_null())
    {
        // 场景1: 已知leader，作为follower加入
        LOG_INFO("Found leader address: %s", persistent_state_.cluster_metadata_.current_leader_.to_string().c_str());
        init_as_follower();
    }
    else if (persistent_state_.cluster_metadata_.cluster_nodes_.empty())
    {
        // 场景2: 首个节点，自举为leader
        LOG_INFO("Bootstrapped as leader, term: %u", persistent_state_.current_term_.load());
    }
    else
    {
        // 场景3: 探查集群
        LOG_INFO("Cluster nodes found but no leader, discovering cluster state");
        init_with_cluster_discovery();
    }

    // 4. 启动后台线程
    start_background_threads();

    is_init_ = true;
    LOG_INFO("RaftNode initialized successfully, role: %d", static_cast<int>(role_.load()));
}

RaftNode::~RaftNode()
{
    // 停止后台线程
    election_thread_running_ = false;
    heartbeat_thread_running_ = false;
    follower_client_thread_running_ = false;

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

    // 关闭元数据文件
    if (metadata_file_ != nullptr)
    {
        fclose(metadata_file_);
        metadata_file_ = nullptr;
    }

    LOG_INFO("RaftNode destroyed");
}

// 加载持久化状态
void RaftNode::load_persistent_state()
{
    if (metadata_file_ == nullptr)
    {
        LOG_INFO("No persistent state found, starting with empty state");
        return;
    }

    fseek(metadata_file_, 0, SEEK_END);
    long file_size = ftell(metadata_file_);
    fseek(metadata_file_, 0, SEEK_SET);

    if (file_size > 0)
    {
        std::string data(file_size, '\0');
        if (fread(&data[0], 1, file_size, metadata_file_) != file_size)
        {
            LOG_ERROR("Failed to read persistent state from file");
            return;
        }
        size_t offset = 0;
        persistent_state_ = PersistentState::deserialize(data.data(), offset);
    }
}

// 保存持久化状态
void RaftNode::save_persistent_state()
{
    if (metadata_file_ == nullptr)
    {
        LOG_ERROR("Metadata file is not open");
        return;
    }

    std::string serialized_data = persistent_state_.serialize();
    if (fwrite(serialized_data.c_str(), 1, serialized_data.size(), metadata_file_) != serialized_data.size())
    {
        LOG_ERROR("Failed to write persistent state to file");
        return;
    }

    fflush(metadata_file_);
}

// 客户端线程工作函数
void RaftNode::follower_client_loop()
{
    while (follower_client_thread_running_)
    {
        if (follower_client_ == nullptr || !follower_client_->is_connected())
        {
            break;
        }
        ProtocolBody *msg = new_body();
        int ret = follower_client_->receive(*msg);
        if (ret == -1)
        {
            LOG_ERROR("Follower client failed to receive message,because of timeout");
            delete msg;
            continue;
        }
        else if (ret == -2)
        {
            LOG_ERROR("Follower client failed to receive message, connection closed");
            delete msg;
            follower_client_ = nullptr;
            follower_client_thread_running_ = false;
            // TODO 或许可以直接触发选举
        }
        else if (ret > 0)
        {
            // TODO 处理收到的消息
        }
    }
}
// 场景1: 作为follower启动
void RaftNode::init_as_follower()
{
    LOG_INFO("Initiating as follower to connect to leader: %s", persistent_state_.cluster_metadata_.current_leader_.to_string().c_str());
    become_follower(persistent_state_.cluster_metadata_.current_leader_, persistent_state_.current_term_, true, persistent_state_.commit_index_, persistent_state_.leader_password_);
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
    std::lock_guard<std::mutex> lock(mutex_);

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
    // 如果是follower,启动选举线程
    if (role_ == RaftRole::Follower)
    {
        election_thread_running_ = true;
        election_thread_ = std::thread(&RaftNode::election_loop, this);
    }

    // 如果是leader，启动心跳线程
    if (role_ == RaftRole::Leader)
    {
        heartbeat_thread_running_ = true;
        heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);
    }

    LOG_INFO("Background threads started");
}

// 同步集群配置
void RaftNode::sync_cluster_metadata()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // TODO: 实现集群配置同步
    LOG_INFO("Syncing cluster metadata from leader");
}

// 同步缺失日志
void RaftNode::sync_missing_logs()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // TODO: 实现日志同步
    LOG_INFO("Syncing missing logs from leader");
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
bool RaftNode::become_follower(const Address &leader_addr, uint32_t term, bool is_reconnect, const uint32_t commit_index, const std::string &password)
{
    std::lock_guard<std::mutex> lock(mutex_);
    LOG_INFO("Becoming follower to connect to leader: %s", leader_addr.to_string().c_str());

    // 连接leader并同步数据
    if (connect_to_leader(leader_addr))
    {
        // 连接成功，发送消息
        RaftMessage init_message = RaftMessage::join_cluster(is_reconnect, commit_index, password);
        follower_client_->send(init_message);
        ProtocolBody *msg = new_body();
        int ret = follower_client_->receive(*msg, raft_msg_timeout_);
        if (ret < 0)
        {
            LOG_ERROR("Follower client failed to receive init message response");
            delete msg;
            // 设置follower状态
            follower_client_ = nullptr;
        }
        else
        {
            role_ = RaftRole::Follower;
            LOG_INFO("Follower client received init message response");
            // 更新任期
            persistent_state_.cluster_metadata_.current_leader_ = leader_addr;
            persistent_state_.commit_index_ = commit_index;
            if (term > persistent_state_.current_term_)
            {
                persistent_state_.current_term_ = term;
                persistent_state_.voted_for_ = ""; // 重置投票
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
                    close_socket(socket);
                }
                // 清空follower_sockets_和follower_address_map_
                follower_sockets_.clear();
                follower_address_map_.clear();
            }
            if (heartbeat_thread_running_)
            {
                heartbeat_thread_running_ = false;
                if (heartbeat_thread_.joinable())
                {
                    heartbeat_thread_.join();
                }
            }
            reset_election_timeout();
            // 启动接收线程
            follower_client_thread_running_ = true;
            std::thread receive_thread(&RaftNode::follower_client_loop, this);
            receive_thread.detach();
            // TODO 处理收到的集群同步消息
            return true;
        }
    }
    role_ = RaftRole::Leader;
    return false;
}

// 转换为Candidate
void RaftNode::become_candidate()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 增加任期
    persistent_state_.current_term_++;
    std::string my_addr = ip_ + ":" + std::to_string(port_);
    persistent_state_.voted_for_ = my_addr; // 投给自己
    save_persistent_state();

    // 更新角色
    role_ = RaftRole::Candidate;

    LOG_INFO("Became candidate, term: %u, starting election", persistent_state_.current_term_.load());

    // 发送RequestVote
    send_request_vote();
}

// 转换为Leader
void RaftNode::become_leader()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新角色
    role_ = RaftRole::Leader;

    // 初始化Leader专用状态
    uint32_t last_index = log_array_.get_last_index();
    next_index_.clear();
    match_index_.clear();
    std::unordered_set<Address> &cluster_nodes_ = persistent_state_.cluster_metadata_.cluster_nodes_;
    for (const auto &node : cluster_nodes_)
    {
    }

    // 持久化状态
    save_persistent_state();

    // 立即发送心跳
    send_heartbeat_to_all();

    // 启动心跳线程
    heartbeat_thread_running_ = true;
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);

    LOG_INFO("Became leader, term: %u", persistent_state_.current_term_.load());
}

// 选举线程主循环
void RaftNode::election_loop()
{
    while (election_thread_running_)
    {
        uint32_t sleep_time = static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(last_heartbeat_time_.time_since_epoch()).count()) + election_timeout_ - get_current_timestamp();
        if (sleep_time > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_time));
        }

        if (is_election_timeout())
        {
            LOG_INFO("Election timeout, starting election");
            become_candidate();
        }
    }
}

// 心跳线程主循环
void RaftNode::heartbeat_loop()
{
    while (heartbeat_thread_running_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval_));

        if (role_ == RaftRole::Leader)
        {
            send_heartbeat_to_all();
        }
    }
}

// 发送RequestVote
void RaftNode::send_request_vote()
{
    uint32_t last_index, last_term;
    log_array_.get_last_info(last_index, last_term);

    // 构造RequestVote消息
    RaftMessage msg = RaftMessage::request_vote(current_term_.load(), last_index);

    std::string my_addr = ip_ + ":" + std::to_string(port_);

    // 向所有集群节点发送投票请求
    for (const auto &node : cluster_nodes_)
    {
        if (node == my_addr)
            continue;

        LOG_INFO("Sending RequestVote to %s (term=%u, last_log_index=%u)",
                 node.c_str(), persistent_state_.current_term_.load(), last_index);

        // TODO: 实际发送消息 (需要实现TCPClient发送)
        // 这将在后续实现完整的消息处理逻辑
    }
}

// 发送心跳到所有节点
void RaftNode::send_heartbeat_to_all()
{
    std::unordered_set<Address> &cluster_nodes_ = persistent_state_.cluster_metadata_.cluster_nodes_;
    if (cluster_nodes_.empty())
        return;
    std::shared_lock<std::shared_mutex> lock(follower_sockets_mutex_);
    for (const auto &sock : follower_sockets_)
    {
        send_append_entries(sock, true); // true表示心跳
    }
}

// 发送AppendEntries
void RaftNode::send_append_entries(const socket_t &sock, bool is_heartbeat)
{
    if (role_ != RaftRole::Leader)
    {
        return;
    }

    uint32_t next_idx = next_index_[sock];

    // 获取待发送的日志
    std::vector<LogEntry> entries;
    if (!is_heartbeat)
    {
        entries = log_array_.get_entries_from(next_idx);
    }

    // 构造AppendEntries消息
    uint32_t prev_idx = (next_idx > 0) ? next_idx - 1 : 0;
    uint32_t prev_term = log_array_.get_term(prev_idx);
    uint32_t commit = persistent_state_.commit_index_.load();

    RaftMessage msg;
    if (is_heartbeat)
    {
        msg = RaftMessage::append_entries(persistent_state_.current_term_.load(), prev_idx, prev_term, commit);
    }
    else
    {
        // 批量发送日志
        std::set<LogEntry> entry_set(entries.begin(), entries.end());
        msg = RaftMessage::append_entries_with_data(persistent_state_.current_term_.load(), prev_idx, prev_term,
                                                    entry_set, commit);
    }

    LOG_DEBUG("Sending AppendEntries to %d (prev_idx=%u, prev_term=%u, entries=%zu, commit=%u)",
              sock, prev_idx, prev_term, entries.size(), commit);

    int ret = send(msg, sock);
    if (ret < 0)
    {
        LOG_ERROR("Failed to send AppendEntries to %d: %s", sock, strerror(errno));
    }
}
// 处理AppendEntries请求
bool RaftNode::handle_append_entries(const RaftMessage &msg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 检查term
    if (msg.term < persistent_state_.current_term_.load())
    {
        LOG_DEBUG("AppendEntries from old term %u (current: %u), rejecting",
                  msg.term, persistent_state_.current_term_.load());
        return false;
    }

    // 2. 发现更高term，降级为follower
    if (msg.term > persistent_state_.current_term_.load())
    {
        LOG_INFO("Discovered higher term %u (current: %u), stepping down to follower",
                 msg.term, persistent_state_.current_term_.load());
        become_follower(msg.term);
    }

    // 重置选举超时
    reset_election_timeout();

    if (!msg.append_entries_data)
    {
        LOG_ERROR("AppendEntries message missing data");
        return false;
    }

    const auto &data = *msg.append_entries_data;

    // 3. 检查prev_log_index和term
    if (data.prev_log_index > 0)
    {
        LogEntry prev_entry;
        if (!log_array_.get(data.prev_log_index, prev_entry))
        {
            LOG_DEBUG("Prev log index %u not found", data.prev_log_index);
            return false;
        }

        if (prev_entry.term != data.prev_log_term)
        {
            LOG_DEBUG("Prev log term mismatch: expected %u, got %u",
                      prev_entry.term, data.prev_log_term);
            return false;
        }
    }

    // 4. 追加新日志
    if (data.entries && !data.entries->empty())
    {
        for (const auto &entry : *data.entries)
        {
            LogEntry existing_entry;
            if (log_array_.get(entry.index, existing_entry))
            {
                // 已存在该索引的日志，检查term是否匹配
                if (existing_entry.term != entry.term)
                {
                    // term不匹配，删除冲突日志
                    LOG_INFO("Log conflict at index %u (old_term=%u, new_term=%u), truncating",
                             entry.index, existing_entry.term, entry.term);
                    log_array_.truncate_from(entry.index);
                }
            }
            // 追加新日志
            log_array_.append(entry);
        }
    }

    // 5. 更新commit_index
    if (data.leader_commit > commit_index_.load())
    {
        uint32_t new_commit = std::min(data.leader_commit, log_array_.get_last_index());
        commit_index_ = new_commit;
        LOG_INFO("Updated commit_index to %u", new_commit);
        apply_committed_entries();
    }

    return true;
}

// 处理RequestVote请求
bool RaftNode::handle_request_vote(const RaftMessage &msg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!msg.request_vote_data)
    {
        LOG_ERROR("RequestVote message missing data");
        return false;
    }

    const auto &data = *msg.request_vote_data;

    // 1. 检查term
    if (msg.term < persistent_state_.current_term_.load())
    {
        LOG_DEBUG("RequestVote from old term %u (current: %u), rejecting",
                  msg.term, persistent_state_.current_term_.load());
        return false;
    }

    // 2. 发现更高term，更新
    if (msg.term > persistent_state_.current_term_.load())
    {
        LOG_INFO("Discovered higher term %u (current: %u) in RequestVote",
                 msg.term, persistent_state_.current_term_.load());
        persistent_state_.current_term_ = msg.term;
        persistent_state_.voted_for_ = "";
        save_persistent_state();
    }

    // 3. 检查是否已投票
    std::string candidate_addr = ""; // TODO: 从消息中获取候选人地址
    if (!voted_for_.empty() && persistent_state_.voted_for_ != candidate_addr)
    {
        LOG_DEBUG("Already voted for %s in term %u", persistent_state_.voted_for_.c_str(), persistent_state_.current_term_.load());
        return false;
    }

    // 4. [优化] 优先投票给最新日志的候选人
    uint32_t last_index, last_term;
    log_array_.get_last_info(last_index, last_term);

    // 候选人的日志至少和自己一样新
    if (data.last_log_index > last_index ||
        (data.last_log_index == last_index && data.last_log_index >= last_term))
    {
        persistent_state_.voted_for_ = candidate_addr;
        save_persistent_state();
        LOG_INFO("Granted vote to %s in term %u", candidate_addr.c_str(), persistent_state_.current_term_.load());
        return true;
    }

    LOG_DEBUG("Rejected vote to %s (candidate log less up-to-date)", candidate_addr.c_str());
    return false;
}

// 应用已提交的日志到状态机
void RaftNode::apply_committed_entries()
{
    while (last_applied_.load() < commit_index_.load())
    {
        last_applied_++;

        // 获取日志条目
        LogEntry entry;
        if (log_array_.get(last_applied_.load(), entry))
        {
            // 应用到状态机
            Response result = execute_command(entry.cmd);

            LOG_INFO("Applied log %u, result: %s", last_applied_.load(),
                     result.success ? "success" : "failed");
        }
    }
}

// 尝试提交日志
void RaftNode::try_commit_entries()
{
    // 找到大多数节点都已匹配的日志索引
    std::vector<uint32_t> matched_indices;
    for (const auto &[node, idx] : match_index_)
    {
        matched_indices.push_back(idx);
    }

    std::sort(matched_indices.begin(), matched_indices.end(), std::greater<uint32_t>());

    if (matched_indices.empty())
    {
        return;
    }

    uint32_t majority_index = matched_indices[cluster_nodes_.size() / 2];

    // 只有当前term的日志才能提交
    if (majority_index > commit_index_.load() &&
        log_array_.get_term(majority_index) == persistent_state_.current_term_.load())
    {
        commit_entry(majority_index);
    }
}

// 提交指定索引的日志
void RaftNode::commit_entry(uint32_t index)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (index <= commit_index_.load())
    {
        return; // 已提交
    }

    // 更新commit_index
    commit_index_ = index;
    LOG_INFO("Committed log entries up to index %u", index);

    // 应用到状态机
    apply_committed_entries();
}

// 计算可达节点数
int RaftNode::count_reachable_nodes()
{
    int count = 0;
    // TODO: 实现实际的可达性检查
    return count;
}

// 停止接受写请求
void RaftNode::stop_accepting_writes()
{
    LOG_INFO("Stopping write requests");
    // TODO: 实现写请求拒绝逻辑
}

// 主动连接处理
void RaftNode::on_active_connect(const std::string &target_address)
{
    if (role_ == RaftRole::Leader)
    {
        LOG_WARN("Active connecting to another node, step down to follower");
        become_follower(current_term_.load());
    }

    // TODO: 尝试连接并发送探查消息
    LOG_INFO("Actively connecting to node: %s", target_address.c_str());
}

// 处理探查leader请求
void RaftNode::handle_query_leader(const RaftMessage &msg, const socket_t &client_sock)
{
    Address leader_addr;
    RaftMessage response_msg;
    if (role_ == RaftRole::Leader)
    {
        get_self_address(client_sock, leader_addr);
    }
    else
    {
        leader_addr = persistent_state_.cluster_metadata_.current_leader_;
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
    JoinClusterResponseData response;
    response.success = false;

    // 检查密码
    if (!password_.empty() && data.password != password_)
    {
        LOG_ERROR("Invalid password for JoinCluster");
        response.error_message = "Invalid password";
        send(response_msg, client_sock);
        close_socket(client_sock);
        return;
    }

    // 添加新节点到集群
    Address new_node_addr;
    if (get_opposite_address(client_sock, new_node_addr))
    {
        // 广播有新节点加入
        auto broadcast_msg = RaftMessage::new_node_join_broadcast(new_node_addr);
        broadcast_to_followers(broadcast_msg);
        response.success = true;
        // TODO: 发送集群必要信息以及判断缺失日志（增量更新 or 全量更新）
        send(response_msg, client_sock);
        // 更新集群配置
        persistent_state_.cluster_metadata_.cluster_nodes_.insert(new_node_addr);
        save_persistent_state();
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

// 处理AppendEntries响应
void RaftNode::handle_append_entries_response(const Address &follower, const RaftMessage &msg)
{
    if (!msg.append_entries_response_data)
    {
        LOG_ERROR("AppendEntriesResponse message missing data");
        return;
    }

    const auto &response = *msg.append_entries_response_data;

    if (response.term > persistent_state_.current_term_.load())
    {
        // 发现更高term，降级
        become_follower(response.term);
        return;
    }

    if (response.success)
    {
        // 复制成功，更新match_index和next_index
        match_index_[follower] = log_array_.get_last_index();
        next_index_[follower] = match_index_[follower] + 1;

        // 检查是否可以提交
        try_commit_entries();
    }
    else
    {
        // 复制失败，回退next_index
        if (next_index_[follower] > 0)
        {
            next_index_[follower]--;
        }

        // 重试
        send_append_entries(follower, false);
    }
}

// 处理RequestVote响应
void RaftNode::handle_request_vote_response(const std::string &voter, const RaftMessage &msg)
{
    if (!msg.request_vote_response_data)
    {
        LOG_ERROR("RequestVoteResponse message missing data");
        return;
    }

    const auto &response = *msg.request_vote_response_data;

    if (response.term > persistent_state_.current_term_.load())
    {
        // 发现更高term，降级
        become_follower(response.term);
        return;
    }

    if (response.vote_granted)
    {
        LOG_INFO("Received vote from %s for term %u", voter.c_str(), persistent_state_.current_term_.load());
        // TODO: 统计投票，如果获得多数则成为leader
    }
    else
    {
        LOG_DEBUG("Vote rejected from %s", voter.c_str());
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

// [修改] 实现两阶段提交 - 提交命令
Response RaftNode::submit_command(const std::string &cmd)
{
    if (role_ != RaftRole::Leader)
    {
        LOG_WARN("Not the leader, cannot submit command");
        return Response::error("Not the leader");
    }

    // 创建日志条目
    LogEntry entry;
    uint32_t last_index = log_array_.get_last_index();
    entry.index = last_index + 1;
    entry.term = persistent_state_.current_term_.load();
    entry.command = cmd;
    entry.timestamp = get_current_timestamp();
    entry.command_type = 0; // 普通命令类型

    // [阶段1] 预写日志
    if (!log_array_.append(entry))
    {
        LOG_ERROR("Failed to append log entry");
        return Response::error("Failed to append log");
    }

    LOG_INFO("Pre-wrote command at index %u, term %u", entry.index, entry.term);

    // [阶段2] 触发日志复制（异步）
    // TODO: 立即向所有follower发送AppendEntries
    // 这将在后续完整实现中完成

    // 返回日志索引 (客户端可用此索引查询结果)
    return Response::success(entry.index);
}

Response RaftNode::handle_raft_command(const std::vector<std::string> &command_parts, bool &is_exec)
{
    Response response;
    is_exec = true;
    auto &leader_address_ = persistent_state_.cluster_metadata_.current_leader_;
    if (command_parts[0] == "get_master" && command_parts.size() == 1)
    {
        // 返回当前leader地址
        if (leader_address_.is_null())
        {
            response = Response::error("no leader elected");
        }
        else
        {
            response = Response::success(leader_address_.to_string());
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
        // 连接到主节点
        Address address(host, port);
        bool success = become_follower(address);
        response = Response::success(success);
    }
    else if (command_parts[0] == "raft_password" && command_parts.size() == 1)
    {
        response = Response::success(password_);
    }
    else if (command_parts[0] == "remove_node" && command_parts.size() == 3)
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
            response = Response::success("node removed");
        }
        else
        {
            response = Response::error("node not found");
        }
    }
    else if (command_parts[0] == "list_nodes" && command_parts.size() == 1)
    {
        auto &cluster_nodes_ = persistent_state_.cluster_metadata_.cluster_nodes_;
        std::string nodes_str;
        for (const auto &node : cluster_nodes_)
        {
            if (!nodes_str.empty())
                nodes_str += ",";
            nodes_str += node.to_string();
        }
        if (!nodes_str.empty() && nodes_str.back() == ',')
        {
            nodes_str.pop_back();
        }
        response = Response::success(nodes_str);
    }
    else if (command_parts[0] == "get_status" && command_parts.size() == 1)
    {
        std::string status = "role=" + std::to_string(static_cast<int>(role_.load())) +
                             ",term=" + std::to_string(persistent_state_.current_term_.load()) +
                             ",leader=" + leader_address_.to_string() +
                             ",nodes=" + std::to_string(persistent_state_.cluster_metadata_.cluster_nodes_.size()) +
                             ",commit=" + std::to_string(persistent_state_.commit_index_.load()) +
                             ",applied=" + std::to_string(persistent_state_.last_applied_.load());
        response = Response::success(status);
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
        {
            std::unique_lock<std::shared_mutex> lock(follower_sockets_mutex_);
            follower_sockets_.erase(sock);
            follower_address_map_.erase(node_to_remove);
        }
        RaftMessage node_remove_msg = RaftMessage::leave_node(node_to_remove);
        broadcast_to_followers(node_remove_msg);
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