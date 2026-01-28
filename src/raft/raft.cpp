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

// [修改] RaftNode构造函数 - 实现完整的初始化流程
RaftNode::RaftNode(const std::string root_dir, const std::string &ip, const u_short port, const uint32_t max_follower_count, const std::string password)
    : TCPServer(ip, port, max_follower_count, 0, 0),
      password_(password),
      log_array_(root_dir + "/.raft"),
      root_dir_(root_dir),
      log_dir_(root_dir + "/.raft"),
      current_term_(0),
      commit_index_(0),
      last_applied_(0),
      role_(RaftRole::Follower),
      rng_(std::chrono::steady_clock::now().time_since_epoch().count())
{
    // 1. 打开元数据文件
    std::string meta_path = PathUtils::CombinePath(root_dir, ".raft", ".raft_meta");
    metadata_file_ = fopen(meta_path.c_str(), "ab+");
    if (metadata_file_ == nullptr)
    {
        LOG_ERROR("Failed to open metadata file: %s", meta_path.c_str());
        throw std::runtime_error("Failed to open metadata file: " + meta_path);
    }

    // 2. 初始化随机数生成器
    election_timeout_ = generate_election_timeout();

    // 3. 加载持久化状态
    load_persistent_state();

    // 4. 加载日志
    log_array_.load_from_disk();

    // 5. 根据场景选择初始化策略
    if (!leader_address_.empty()) {
        // 场景1: 已知leader，作为follower加入
        LOG_INFO("Found leader address: %s", leader_address_.c_str());
        init_as_follower();
    } else if (cluster_nodes_.empty()) {
        // 场景2: 首个节点，自举为leader
        LOG_INFO("No cluster nodes found, bootstrapping as leader");
        init_as_bootstrap_leader();
    } else {
        // 场景3: 探查集群
        LOG_INFO("Cluster nodes found but no leader, discovering cluster state");
        init_with_cluster_discovery();
    }

    // 6. 启动后台线程
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

    if (election_thread_.joinable()) {
        election_thread_.join();
    }
    if (heartbeat_thread_.joinable()) {
        heartbeat_thread_.join();
    }
    if (follower_client_thread_.joinable()) {
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

// [新增] 加载持久化状态
void RaftNode::load_persistent_state()
{
    std::string meta_path = PathUtils::CombinePath(root_dir_, ".raft", ".raft_meta");

    FILE* file = fopen(meta_path.c_str(), "rb");
    if (file == nullptr) {
        LOG_INFO("No persistent state found, starting with empty state");
        current_term_ = 0;
        voted_for_ = "";
        cluster_metadata_ = ClusterMetadata();
        persistent_state_.log_snapshot_index_ = 0;
        return;
    }

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    if (file_size > 0) {
        std::vector<char> buffer(file_size);
        fread(buffer.data(), 1, file_size, file);
        fclose(file);

        try {
            // 读取持久化状态 (简化实现，实际应该完整解析)
            // 这里假设数据格式为: term(4) + voted_for_len(4) + voted_for + ...
            size_t offset = 0;
            if (offset + 4 <= buffer.size()) {
                uint32_t net_term;
                memcpy(&net_term, buffer.data() + offset, 4);
                current_term_ = ntohl(net_term);
                offset += 4;
            }

            if (offset + 4 <= buffer.size()) {
                uint32_t voted_len;
                memcpy(&voted_len, buffer.data() + offset, 4);
                offset += 4;
                voted_len = ntohl(voted_len);
                if (offset + voted_len <= buffer.size()) {
                    voted_for_.assign(buffer.data() + offset, voted_len);
                    offset += voted_len;
                }
            }

            // 加载集群元数据
            if (offset < buffer.size()) {
                cluster_metadata_ = ClusterMetadata::deserialize(buffer.data(), offset);
            }

            LOG_INFO("Loaded persistent state: term=%u, voted_for=%s",
                     current_term_.load(), voted_for_.c_str());
        } catch (const std::exception& e) {
            LOG_ERROR("Failed to parse persistent state: %s", e.what());
        }
    } else {
        fclose(file);
    }
}

// [新增] 保存持久化状态
void RaftNode::save_persistent_state()
{
    std::string meta_path = PathUtils::CombinePath(root_dir_, ".raft", ".raft_meta");

    FILE* file = fopen(meta_path.c_str(), "wb");
    if (file == nullptr) {
        LOG_ERROR("Failed to open metadata file for writing: %s", meta_path.c_str());
        return;
    }

    try {
        // 序列化持久化状态
        uint32_t net_term = htonl(current_term_.load());
        uint32_t voted_len = htonl(static_cast<uint32_t>(voted_for_.size()));

        fwrite(&net_term, 4, 1, file);
        fwrite(&voted_len, 4, 1, file);
        if (!voted_for_.empty()) {
            fwrite(voted_for_.data(), 1, voted_for_.size(), file);
        }

        // 序列化集群元数据
        std::string cluster_data = cluster_metadata_.serialize();
        fwrite(cluster_data.data(), 1, cluster_data.size(), file);

        fflush(file);
        fsync(fileno(file));
        fclose(file);

        LOG_INFO("Saved persistent state: term=%u, voted_for=%s",
                 current_term_.load(), voted_for_.c_str());
    } catch (const std::exception& e) {
        LOG_ERROR("Failed to save persistent state: %s", e.what());
        fclose(file);
    }
}

// [新增] 场景1: 作为follower启动
void RaftNode::init_as_follower()
{
    std::lock_guard<std::mutex> lock(mutex_);

    role_ = RaftRole::Follower;
    reset_election_timeout();

    LOG_INFO("Initiating as follower to connect to leader: %s", leader_address_.c_str());

    // TODO: 连接leader并同步数据
    // 这将在后续实现
}

// [新增] 场景2: 自举为初始leader
void RaftNode::init_as_bootstrap_leader()
{
    std::lock_guard<std::mutex> lock(mutex_);

    role_ = RaftRole::Leader;
    current_term_ = 1;
    leader_address_ = ip_ + ":" + std::to_string(port_);

    // 初始化next_index和match_index
    uint32_t last_index = log_array_.get_last_index();
    next_index_.clear();
    match_index_.clear();
    for (const auto& node : cluster_nodes_) {
        next_index_[node] = last_index + 1;
        match_index_[node] = 0;
    }

    // 持久化状态
    save_persistent_state();

    // 添加自己到集群节点列表
    std::string my_addr = ip_ + ":" + std::to_string(port_);
    bool found = false;
    for (const auto& node : cluster_nodes_) {
        if (node == my_addr) {
            found = true;
            break;
        }
    }
    if (!found) {
        cluster_nodes_.push_back(my_addr);
        cluster_metadata_.cluster_nodes_ = cluster_nodes_;
        cluster_metadata_.current_leader_ = my_addr;
        save_persistent_state();
    }

    LOG_INFO("Bootstrapped as leader, term: %u, address: %s",
             current_term_.load(), leader_address_.c_str());
}

// [新增] 场景3: 探查集群状态
void RaftNode::init_with_cluster_discovery()
{
    std::lock_guard<std::mutex> lock(mutex_);

    role_ = RaftRole::Follower;

    if (cluster_nodes_.empty()) {
        LOG_WARN("No cluster nodes to discover, will wait for manual configuration");
        return;
    }

    // 随机选择一个节点探查
    int random_idx = rng_() % cluster_nodes_.size();
    std::string target_node = cluster_nodes_[random_idx];

    LOG_INFO("Probing cluster via node: %s", target_node.c_str());

    // TODO: 发送QueryLeader消息并等待响应
    // 这将在后续实现
}

// [新增] 启动后台线程
void RaftNode::start_background_threads()
{
    // 启动选举线程
    election_thread_running_ = true;
    election_thread_ = std::thread(&RaftNode::election_loop, this);

    // 如果是leader，启动心跳线程
    if (role_ == RaftRole::Leader) {
        heartbeat_thread_running_ = true;
        heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);
    }

    LOG_INFO("Background threads started");
}

// [新增] 同步集群配置
void RaftNode::sync_cluster_metadata()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // TODO: 实现集群配置同步
    LOG_INFO("Syncing cluster metadata from leader");
}

// [新增] 同步缺失日志
void RaftNode::sync_missing_logs()
{
    std::lock_guard<std::mutex> lock(mutex_);
    // TODO: 实现日志同步
    LOG_INFO("Syncing missing logs from leader");
}

// [新增] 获取当前时间戳
uint32_t RaftNode::get_current_timestamp()
{
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(duration).count());
}

// [新增] 判断是否有指定索引的日志
bool RaftNode::has_log(uint32_t index) const
{
    LogEntry entry;
    return log_array_.get(index, entry);
}

// [新增] 转换为Follower
void RaftNode::become_follower(uint32_t term)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新任期
    if (term > current_term_) {
        current_term_ = term;
        voted_for_ = "";  // 重置投票
        save_persistent_state();
    }

    // 更新角色
    role_ = RaftRole::Follower;

    // 清空leader状态
    next_index_.clear();
    match_index_.clear();

    // 停止心跳线程
    if (heartbeat_thread_running_) {
        heartbeat_thread_running_ = false;
        if (heartbeat_thread_.joinable()) {
            heartbeat_thread_.join();
        }
    }

    // 重置选举超时
    reset_election_timeout();

    LOG_INFO("Became follower, term: %u", current_term_.load());
}

// [新增] 转换为Candidate
void RaftNode::become_candidate()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 增加任期
    current_term_++;
    std::string my_addr = ip_ + ":" + std::to_string(port_);
    voted_for_ = my_addr;  // 投给自己
    save_persistent_state();

    // 更新角色
    role_ = RaftRole::Candidate;

    LOG_INFO("Became candidate, term: %u, starting election", current_term_.load());

    // 发送RequestVote
    send_request_vote();
}

// [新增] 转换为Leader
void RaftNode::become_leader()
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 更新角色
    role_ = RaftRole::Leader;
    std::string my_addr = ip_ + ":" + std::to_string(port_);
    leader_address_ = my_addr;

    // 初始化Leader专用状态
    uint32_t last_index = log_array_.get_last_index();
    next_index_.clear();
    match_index_.clear();
    for (const auto& node : cluster_nodes_) {
        if (node != my_addr) {
            next_index_[node] = last_index + 1;
            match_index_[node] = 0;
        }
    }

    // 持久化状态
    save_persistent_state();

    // 立即发送心跳
    send_heartbeat_to_all();

    // 启动心跳线程
    heartbeat_thread_running_ = true;
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);

    LOG_INFO("Became leader, term: %u, address: %s", current_term_.load(), leader_address_.c_str());
}

// [新增] 选举线程主循环
void RaftNode::election_loop()
{
    while (election_thread_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (is_election_timeout()) {
            LOG_INFO("Election timeout, starting election");
            become_candidate();
        }
    }
}

// [新增] 心跳线程主循环
void RaftNode::heartbeat_loop()
{
    while (heartbeat_thread_running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval_));

        if (role_ == RaftRole::Leader) {
            send_heartbeat_to_all();
        }
    }
}

// [新增] 发送RequestVote
void RaftNode::send_request_vote()
{
    uint32_t last_index, last_term;
    log_array_.get_last_info(last_index, last_term);

    // 构造RequestVote消息
    RaftMessage msg = RaftMessage::request_vote(current_term_.load(), last_index);

    std::string my_addr = ip_ + ":" + std::to_string(port_);

    // 向所有集群节点发送投票请求
    for (const auto& node : cluster_nodes_) {
        if (node == my_addr) continue;

        LOG_INFO("Sending RequestVote to %s (term=%u, last_log_index=%u)",
                 node.c_str(), current_term_.load(), last_index);

        // TODO: 实际发送消息 (需要实现TCPClient发送)
        // 这将在后续实现完整的消息处理逻辑
    }
}

// [新增] 发送心跳到所有节点
void RaftNode::send_heartbeat_to_all()
{
    std::string my_addr = ip_ + ":" + std::to_string(port_);

    for (const auto& node : cluster_nodes_) {
        if (node == my_addr) continue;
        send_append_entries(node, true);  // true表示心跳
    }
}

// [新增] 发送AppendEntries
void RaftNode::send_append_entries(const std::string& follower, bool is_heartbeat)
{
    if (role_ != RaftRole::Leader) {
        return;
    }

    uint32_t next_idx = next_index_[follower];

    // 获取待发送的日志
    std::vector<LogEntry> entries;
    if (!is_heartbeat) {
        entries = log_array_.get_entries_from(next_idx);
    }

    // 构造AppendEntries消息
    uint32_t prev_idx = (next_idx > 0) ? next_idx - 1 : 0;
    uint32_t prev_term = log_array_.get_term(prev_idx);
    uint32_t commit = commit_index_.load();

    RaftMessage msg;
    if (is_heartbeat) {
        msg = RaftMessage::append_entries(current_term_.load(), prev_idx, prev_term, commit);
    } else {
        // 批量发送日志
        std::set<LogEntry> entry_set(entries.begin(), entries.end());
        msg = RaftMessage::append_entries_with_data(current_term_.load(), prev_idx, prev_term,
                                                      entry_set, commit);
    }

    LOG_DEBUG("Sending AppendEntries to %s (prev_idx=%u, prev_term=%u, entries=%zu, commit=%u)",
              follower.c_str(), prev_idx, prev_term, entries.size(), commit);

    // TODO: 实际发送消息
    // 这将在后续实现
}

// [新增] 处理AppendEntries请求
bool RaftNode::handle_append_entries(const RaftMessage& msg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    // 1. 检查term
    if (msg.term < current_term_.load()) {
        LOG_DEBUG("AppendEntries from old term %u (current: %u), rejecting",
                  msg.term, current_term_.load());
        return false;
    }

    // 2. 发现更高term，降级为follower
    if (msg.term > current_term_.load()) {
        LOG_INFO("Discovered higher term %u (current: %u), stepping down to follower",
                 msg.term, current_term_.load());
        become_follower(msg.term);
    }

    // 重置选举超时
    reset_election_timeout();

    if (!msg.append_entries_data) {
        LOG_ERROR("AppendEntries message missing data");
        return false;
    }

    const auto& data = *msg.append_entries_data;

    // 3. 检查prev_log_index和term
    if (data.prev_log_index > 0) {
        LogEntry prev_entry;
        if (!log_array_.get(data.prev_log_index, prev_entry)) {
            LOG_DEBUG("Prev log index %u not found", data.prev_log_index);
            return false;
        }

        if (prev_entry.term != data.prev_log_term) {
            LOG_DEBUG("Prev log term mismatch: expected %u, got %u",
                      prev_entry.term, data.prev_log_term);
            return false;
        }
    }

    // 4. 追加新日志
    if (data.entries && !data.entries->empty()) {
        for (const auto& entry : *data.entries) {
            LogEntry existing_entry;
            if (log_array_.get(entry.index, existing_entry)) {
                // 已存在该索引的日志，检查term是否匹配
                if (existing_entry.term != entry.term) {
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
    if (data.leader_commit > commit_index_.load()) {
        uint32_t new_commit = std::min(data.leader_commit, log_array_.get_last_index());
        commit_index_ = new_commit;
        LOG_INFO("Updated commit_index to %u", new_commit);
        apply_committed_entries();
    }

    return true;
}

// [新增] 处理RequestVote请求
bool RaftNode::handle_request_vote(const RaftMessage& msg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (!msg.request_vote_data) {
        LOG_ERROR("RequestVote message missing data");
        return false;
    }

    const auto& data = *msg.request_vote_data;

    // 1. 检查term
    if (msg.term < current_term_.load()) {
        LOG_DEBUG("RequestVote from old term %u (current: %u), rejecting",
                  msg.term, current_term_.load());
        return false;
    }

    // 2. 发现更高term，更新
    if (msg.term > current_term_.load()) {
        LOG_INFO("Discovered higher term %u (current: %u) in RequestVote",
                 msg.term, current_term_.load());
        current_term_ = msg.term;
        voted_for_ = "";
        save_persistent_state();
    }

    // 3. 检查是否已投票
    std::string candidate_addr = "";  // TODO: 从消息中获取候选人地址
    if (!voted_for_.empty() && voted_for_ != candidate_addr) {
        LOG_DEBUG("Already voted for %s in term %u", voted_for_.c_str(), current_term_.load());
        return false;
    }

    // 4. [优化] 优先投票给最新日志的候选人
    uint32_t last_index, last_term;
    log_array_.get_last_info(last_index, last_term);

    // 候选人的日志至少和自己一样新
    if (data.last_log_index > last_index ||
        (data.last_log_index == last_index && data.last_log_index >= last_term)) {
        voted_for_ = candidate_addr;
        save_persistent_state();
        LOG_INFO("Granted vote to %s in term %u", candidate_addr.c_str(), current_term_.load());
        return true;
    }

    LOG_DEBUG("Rejected vote to %s (candidate log less up-to-date)", candidate_addr.c_str());
    return false;
}

// [新增] 应用已提交的日志到状态机
void RaftNode::apply_committed_entries()
{
    while (last_applied_.load() < commit_index_.load()) {
        last_applied_++;

        // 获取日志条目
        LogEntry entry;
        if (log_array_.get(last_applied_.load(), entry)) {
            // 应用到状态机
            Response result = execute_command(entry.cmd);

            LOG_INFO("Applied log %u, result: %s", last_applied_.load(),
                     result.success ? "success" : "failed");
        }
    }
}

// [新增] 尝试提交日志
void RaftNode::try_commit_entries()
{
    // 找到大多数节点都已匹配的日志索引
    std::vector<uint32_t> matched_indices;
    for (const auto& [node, idx] : match_index_) {
        matched_indices.push_back(idx);
    }

    std::sort(matched_indices.begin(), matched_indices.end(), std::greater<uint32_t>());

    if (matched_indices.empty()) {
        return;
    }

    uint32_t majority_index = matched_indices[cluster_nodes_.size() / 2];

    // 只有当前term的日志才能提交
    if (majority_index > commit_index_.load() &&
        log_array_.get_term(majority_index) == current_term_.load()) {
        commit_entry(majority_index);
    }
}

// [新增] 提交指定索引的日志
void RaftNode::commit_entry(uint32_t index)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (index <= commit_index_.load()) {
        return;  // 已提交
    }

    // 更新commit_index
    commit_index_ = index;
    LOG_INFO("Committed log entries up to index %u", index);

    // 应用到状态机
    apply_committed_entries();
}

// [新增] 计算可达节点数
int RaftNode::count_reachable_nodes()
{
    int count = 0;
    // TODO: 实现实际的可达性检查
    return count;
}

// [新增] 停止接受写请求
void RaftNode::stop_accepting_writes()
{
    LOG_INFO("Stopping write requests");
    // TODO: 实现写请求拒绝逻辑
}

// [新增] 主动连接处理
void RaftNode::on_active_connect(const std::string& target_address)
{
    if (role_ == RaftRole::Leader) {
        LOG_WARN("Active connecting to another node, step down to follower");
        become_follower(current_term_.load());
    }

    // TODO: 尝试连接并发送探查消息
    LOG_INFO("Actively connecting to node: %s", target_address.c_str());
}

// [新增] 处理探查leader请求
void RaftNode::handle_query_leader(const RaftMessage& msg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    std::string leader_addr = (role_ == RaftRole::Leader) ?
        (ip_ + ":" + std::to_string(port_)) : leader_address_;
    uint32_t term = current_term_.load();

    // TODO: 发送响应
    LOG_INFO("Handling QueryLeader: has_leader=%d, leader=%s, term=%u",
             (role_ == RaftRole::Leader), leader_addr.c_str(), term);
}

// [新增] 处理新节点加入请求
void RaftNode::handle_join_cluster(const RaftMessage& msg)
{
    std::lock_guard<std::mutex> lock(mutex_);

    if (role_ != RaftRole::Leader) {
        LOG_WARN("Only leader can handle JoinCluster");
        return;
    }

    if (!msg.join_cluster_data) {
        LOG_ERROR("JoinCluster message missing data");
        return;
    }

    const auto& data = *msg.join_cluster_data;

    // 检查密码
    if (!password_.empty() && data.password != password_) {
        LOG_ERROR("Invalid password for JoinCluster");
        return;
    }

    // 添加新节点到集群
    bool found = false;
    for (const auto& node : cluster_nodes_) {
        if (node == data.new_node) {
            found = true;
            break;
        }
    }

    if (!found) {
        cluster_nodes_.push_back(data.new_node);
        cluster_metadata_.cluster_nodes_ = cluster_nodes_;
        save_persistent_state();

        // TODO: 发送JoinClusterResponse
        LOG_INFO("New node joined cluster: %s", data.new_node.c_str());
    } else {
        LOG_WARN("Node already in cluster: %s", data.new_node.c_str());
    }
}

// [新增] 处理AppendEntries响应
void RaftNode::handle_append_entries_response(const std::string& follower, const RaftMessage& msg)
{
    if (!msg.append_entries_response_data) {
        LOG_ERROR("AppendEntriesResponse message missing data");
        return;
    }

    const auto& response = *msg.append_entries_response_data;

    if (response.term > current_term_.load()) {
        // 发现更高term，降级
        become_follower(response.term);
        return;
    }

    if (response.success) {
        // 复制成功，更新match_index和next_index
        match_index_[follower] = log_array_.get_last_index();
        next_index_[follower] = match_index_[follower] + 1;

        // 检查是否可以提交
        try_commit_entries();
    } else {
        // 复制失败，回退next_index
        if (next_index_[follower] > 0) {
            next_index_[follower]--;
        }

        // 重试
        send_append_entries(follower, false);
    }
}

// [新增] 处理RequestVote响应
void RaftNode::handle_request_vote_response(const std::string& voter, const RaftMessage& msg)
{
    if (!msg.request_vote_response_data) {
        LOG_ERROR("RequestVoteResponse message missing data");
        return;
    }

    const auto& response = *msg.request_vote_response_data;

    if (response.term > current_term_.load()) {
        // 发现更高term，降级
        become_follower(response.term);
        return;
    }

    if (response.vote_granted) {
        LOG_INFO("Received vote from %s for term %u", voter.c_str(), current_term_.load());
        // TODO: 统计投票，如果获得多数则成为leader
    } else {
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
    entry.term = current_term_.load();
    entry.command = cmd;
    entry.timestamp = get_current_timestamp();
    entry.command_type = 0;  // 普通命令类型

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
    if (command_parts[0] == "get_master" && command_parts.size() == 1)
    {
        // 返回当前leader地址
        if (leader_address_.empty())
        {
            response = Response::error("no leader elected");
        }
        else
        {
            response = Response::success(leader_address_);
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
        std::string new_leader = host + ":" + std::to_string(port);

        // [修改] 检查密码
        if (!password_.empty() && password != password_) {
            response = Response::error("Invalid password");
            return response;
        }

        leader_address_ = new_leader;
        response = Response::success(true);
    }
    else if (command_parts[0] == "raft_password" && command_parts.size() == 1)
    {
        response = Response::success(password_);
    }
    // [新增] 添加集群管理命令
    else if (command_parts[0] == "add_node" && command_parts.size() == 2)
    {
        if (role_ != RaftRole::Leader) {
            response = Response::error("only leader can add node");
            return response;
        }
        std::string new_node = command_parts[1];

        // 添加节点到集群
        bool found = false;
        for (const auto& node : cluster_nodes_) {
            if (node == new_node) {
                found = true;
                break;
            }
        }

        if (!found) {
            cluster_nodes_.push_back(new_node);
            cluster_metadata_.cluster_nodes_ = cluster_nodes_;
            save_persistent_state();
            response = Response::success("Node added to cluster");
            LOG_INFO("Added node to cluster: %s", new_node.c_str());
        } else {
            response = Response::error("Node already in cluster");
        }
    }
    else if (command_parts[0] == "remove_node" && command_parts.size() == 2)
    {
        if (role_ != RaftRole::Leader) {
            response = Response::error("only leader can remove node");
            return response;
        }
        std::string node_to_remove = command_parts[1];

        // 从集群移除节点
        auto it = std::find(cluster_nodes_.begin(), cluster_nodes_.end(), node_to_remove);
        if (it != cluster_nodes_.end()) {
            cluster_nodes_.erase(it);
            cluster_metadata_.cluster_nodes_ = cluster_nodes_;
            save_persistent_state();
            next_index_.erase(node_to_remove);
            match_index_.erase(node_to_remove);
            response = Response::success("Node removed from cluster");
            LOG_INFO("Removed node from cluster: %s", node_to_remove.c_str());
        } else {
            response = Response::error("Node not found in cluster");
        }
    }
    else if (command_parts[0] == "list_nodes" && command_parts.size() == 1)
    {
        std::string nodes_str;
        for (const auto& node : cluster_nodes_) {
            if (!nodes_str.empty()) nodes_str += ",";
            nodes_str += node;
        }
        response = Response::success(nodes_str);
    }
    else if (command_parts[0] == "get_status" && command_parts.size() == 1)
    {
        std::string status = "role=" + std::to_string(static_cast<int>(role_.load())) +
                          ",term=" + std::to_string(current_term_.load()) +
                          ",leader=" + leader_address_ +
                          ",nodes=" + std::to_string(cluster_nodes_.size()) +
                          ",commit=" + std::to_string(commit_index_.load()) +
                          ",applied=" + std::to_string(last_applied_.load());
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
    TCPServer::add_new_connection(client_sock, client_addr);
    bool is_connected = sockets_.find(client_sock) != sockets_.end();
    if (is_connected)
    {
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        u_short client_port = ntohs(client_addr.sin_port);
        std::string client_addr_str = std::string(client_ip) + ":" + std::to_string(client_port);

        LOG_INFO("New connection from: %s", client_addr_str.c_str());

        // [实现] 检查角色，如果是leader节点，发送集群元数据给新节点
        if (role_ == RaftRole::Leader)
        {
            LOG_INFO("Leader detected new connection, sending cluster metadata");
            // TODO: 发送集群元数据
        }
        else if (role_ == RaftRole::Follower && !leader_address_.empty())
        {
            // Follower连接，检查是否是leader
            LOG_INFO("Follower connecting to potential leader: %s", client_addr_str.c_str());
            // TODO: 发送QueryLeader消息
        }
    }
}