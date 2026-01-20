#include "raft/raft.h"
#include "logger/logger.h"
#include <algorithm>
//  RaftNode 实现
RaftNode::RaftNode()
    : current_term_(0), log_buffer_(), commit_index_(0), last_applied_(0), role_(RaftRole::Follower), rng_(std::random_device{}()), election_timeout_(generate_election_timeout()), last_heartbeat_time_(std::chrono::steady_clock::now()), running_(false)
{
}

RaftNode::~RaftNode()
{
    stop();
}

void RaftNode::start()
{
    if (running_.load())
    {
        return;
    }

    running_ = true;
    reset_election_timeout();

    // 启动选举线程
    election_thread_ = std::thread(&RaftNode::election_loop, this);

    // 启动心跳线程
    heartbeat_thread_ = std::thread(&RaftNode::heartbeat_loop, this);
}

void RaftNode::stop()
{
    if (!running_.load())
    {
        return;
    }

    running_ = false;

    // 等待线程结束
    if (election_thread_.joinable())
    {
        election_thread_.join();
    }
    if (heartbeat_thread_.joinable())
    {
        heartbeat_thread_.join();
    }
}

void RaftNode::election_loop()
{
    while (running_.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

        if (!running_.load())
        {
            break;
        }

        // 只有Follower和Candidate才会检查选举超时
        RaftRole current_role = role_.load();
        if (current_role == RaftRole::Follower || current_role == RaftRole::Candidate)
        {
            if (is_election_timeout())
            {
                become_candidate();
            }
        }
    }
}

void RaftNode::heartbeat_loop()
{
    while (running_.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval_));

        if (!running_.load())
        {
            break;
        }

        // 只有Leader才会发送心跳
        if (role_.load() == RaftRole::Leader)
        {
            for (int follower_id : cluster_nodes_)
            {
                if (follower_id != node_id_)
                {
                    send_append_entries(follower_id, true);
                }
            }
        }
    }
}

void RaftNode::become_follower(uint32_t term)
{
    std::lock_guard<std::mutex> lock(mutex_);

    role_ = RaftRole::Follower;
    current_term_ = term;
    voted_for_ = -1;
    leader_id_ = -1;
    reset_election_timeout();

    LOG_INFO("Node %d: became follower in term %u", node_id_, term);
}

void RaftNode::become_candidate()
{
    std::lock_guard<std::mutex> lock(mutex_);

    role_ = RaftRole::Candidate;
    current_term_++;
    voted_for_ = node_id_;
    reset_election_timeout();

    LOG_INFO("Node %d: became candidate in term %u", node_id_, current_term_.load());

    // 释放锁后发送投票请求
    lock.~lock_guard();
    send_request_vote();
}

void RaftNode::become_leader()
{
    std::lock_guard<std::mutex> lock(mutex_);

    role_ = RaftRole::Leader;
    leader_id_ = node_id_;

    // 初始化Leader状态
    uint32_t last_index = log_buffer_.get_last_index();
    for (int follower_id : cluster_nodes_)
    {
        if (follower_id != node_id_)
        {
            next_index_[follower_id] = last_index + 1;
            match_index_[follower_id] = 0;
        }
    }

    LOG_INFO("Node %d: became leader in term %u", node_id_, current_term_.load());
}

void RaftNode::send_request_vote()
{
    RequestVoteRequest req;
    req.term = current_term_.load();
    req.candidate_id = node_id_;
    log_buffer_.get_last_info(req.last_log_index, req.last_log_term);

    std::string data = req.serialize();

    for (int node_id : cluster_nodes_)
    {
        if (node_id != node_id_)
        {
            send_rpc_(node_id, RaftMessageType::REQUEST_VOTE, data);
        }
    }
}

void RaftNode::send_append_entries(int follower_id, bool is_heartbeat)
{
    AppendEntriesRequest req;
    req.term = current_term_.load();
    req.leader_id = node_id_;
    req.leader_commit = commit_index_.load();

    if (is_heartbeat)
    {
        req.prev_log_index = 0;
        req.prev_log_term = 0;
        req.entries.clear();
    }
    else
    {
        uint32_t next_idx = next_index_[follower_id];
        req.prev_log_index = next_idx - 1;

        LogEntry prev_entry;
        if (log_buffer_.get(req.prev_log_index, prev_entry))
        {
            req.prev_log_term = prev_entry.term;
        }
        else
        {
            req.prev_log_term = 0;
        }

        req.entries = log_buffer_.get_entries_from(next_idx);
    }

    std::string data = req.serialize();
    send_rpc_(follower_id, RaftMessageType::APPEND_ENTRIES, data);
}

void RaftNode::apply_committed_entries()
{
    while (true)
    {
        uint32_t commit_idx = commit_index_.load();
        uint32_t last_applied = last_applied_.load();

        if (last_applied >= commit_idx)
        {
            break;
        }

        for (uint32_t i = last_applied + 1; i <= commit_idx; ++i)
        {
            LogEntry entry;
            if (log_buffer_.get(i, entry))
            {
                apply_to_state_machine_(entry);
            }
            last_applied_ = i;
        }
    }
}

RequestVoteResponse RaftNode::handle_request_vote(const RequestVoteRequest &req)
{
    std::lock_guard<std::mutex> lock(mutex_);

    RequestVoteResponse resp;
    resp.term = current_term_.load();
    resp.vote_granted = false;

    // 如果请求的任期更大，更新当前任期并转为Follower
    if (req.term > current_term_.load())
    {
        current_term_ = req.term;
        role_ = RaftRole::Follower;
        voted_for_ = -1;
        leader_id_ = -1;
    }

    // 如果任期相同且未投票，或者已经投给该候选人
    if (req.term == current_term_.load() &&
        (voted_for_.load() == -1 || voted_for_.load() == req.candidate_id))
    {
        // 检查日志是否至少一样新
        uint32_t last_log_index, last_log_term;
        log_buffer_.get_last_info(last_log_index, last_log_term);

        if (req.last_log_term > last_log_term ||
            (req.last_log_term == last_log_term && req.last_log_index >= last_log_index))
        {
            voted_for_ = req.candidate_id;
            resp.vote_granted = true;
            reset_election_timeout();

            LOG_INFO("Node %d: voted for node %d in term %u", node_id_, req.candidate_id, req.term);
        }
    }

    return resp;
}

AppendEntriesResponse RaftNode::handle_append_entries(const AppendEntriesRequest &req)
{
    std::lock_guard<std::mutex> lock(mutex_);

    AppendEntriesResponse resp;
    resp.term = current_term_.load();
    resp.success = false;

    // 如果请求的任期更大，更新当前任期并转为Follower
    if (req.term > current_term_.load())
    {
        current_term_ = req.term;
        role_ = RaftRole::Follower;
        voted_for_ = -1;
        leader_id_ = req.leader_id;
    }

    // 如果任期不同，拒绝
    if (req.term != current_term_.load())
    {
        return resp;
    }

    // 更新Leader
    leader_id_ = req.leader_id;
    reset_election_timeout();

    // 检查前一条日志是否匹配
    if (req.prev_log_index > 0)
    {
        uint32_t prev_term = log_buffer_.get_term(req.prev_log_index);
        if (prev_term == 0 || prev_term != req.prev_log_term)
        {
            return resp;
        }
    }

    // 追加新日志
    for (size_t i = 0; i < req.entries.size(); ++i)
    {
        uint32_t log_index = req.prev_log_index + 1 + i;
        uint32_t existing_term = log_buffer_.get_term(log_index);

        if (existing_term == 0)
        {
            // 日志不存在，追加
            LogEntry new_entry(req.entries[i].term, log_index, req.entries[i].cmd);
            log_buffer_.append(new_entry);
        }
        else if (existing_term != req.entries[i].term)
        {
            // 日志冲突，删除从该位置开始的所有日志
            log_buffer_.truncate_from(log_index);
            LogEntry new_entry(req.entries[i].term, log_index, req.entries[i].cmd);
            log_buffer_.append(new_entry);
        }
    }

    // 更新commit_index
    if (req.leader_commit > commit_index_.load())
    {
        uint32_t last_log_index = log_buffer_.get_last_index();
        commit_index_ = std::min(req.leader_commit, last_log_index);
    }

    resp.success = true;

    // 在锁外应用已提交的日志
    lock.~lock_guard();
    apply_committed_entries();

    return resp;
}

void RaftNode::receive_rpc(int from_node_id, RaftMessageType msg_type, const std::string &data)
{
    size_t offset = 0;

    switch (msg_type)
    {
    case RaftMessageType::REQUEST_VOTE:
    {
        RequestVoteRequest req = RequestVoteRequest::deserialize(data.c_str(), offset);
        RequestVoteResponse resp = handle_request_vote(req);
        std::string resp_data = resp.serialize();
        send_rpc_(from_node_id, RaftMessageType::REQUEST_VOTE_RESPONSE, resp_data);
        break;
    }
    case RaftMessageType::REQUEST_VOTE_RESPONSE:
    {
        RequestVoteResponse resp = RequestVoteResponse::deserialize(data.c_str(), offset);

        // 如果是Candidate状态
        if (role_.load() == RaftRole::Candidate)
        {
            if (resp.term > current_term_.load())
            {
                // 发现更高任期，转为Follower
                become_follower(resp.term);
            }
            else if (resp.vote_granted)
            {
                // 统计票数
                int votes = 1; // 自己投给自己
                votes++;       // 收到一票

                // 如果获得多数票，成为Leader
                if (votes > static_cast<int>(cluster_nodes_.size()) / 2)
                {
                    become_leader();
                }
            }
        }
        break;
    }
    case RaftMessageType::APPEND_ENTRIES:
    {
        AppendEntriesRequest req = AppendEntriesRequest::deserialize(data.c_str(), offset);
        AppendEntriesResponse resp = handle_append_entries(req);
        std::string resp_data = resp.serialize();
        send_rpc_(from_node_id, RaftMessageType::APPEND_ENTRIES_RESPONSE, resp_data);
        break;
    }
    case RaftMessageType::APPEND_ENTRIES_RESPONSE:
    {
        AppendEntriesResponse resp = AppendEntriesResponse::deserialize(data.c_str(), offset);

        // 如果是Leader状态
        if (role_.load() == RaftRole::Leader)
        {
            if (resp.term > current_term_.load())
            {
                // 发现更高任期，转为Follower
                become_follower(resp.term);
            }
            else if (resp.success)
            {
                // 更新Follower的匹配索引
                match_index_[from_node_id] = next_index_[from_node_id] - 1;
                next_index_[from_node_id]++;

                // 检查是否有新的日志可以提交
                std::vector<uint32_t> match_indices;
                match_indices.push_back(log_buffer_.get_last_index());
                for (const auto &pair : match_index_)
                {
                    match_indices.push_back(pair.second);
                }

                std::sort(match_indices.rbegin(), match_indices.rend());
                uint32_t majority_idx = match_indices[match_indices.size() / 2];

                if (majority_idx > commit_index_.load())
                {
                    LogEntry entry;
                    if (log_buffer_.get(majority_idx, entry) && entry.term == current_term_.load())
                    {
                        commit_index_ = majority_idx;
                        apply_committed_entries();
                    }
                }
            }
        }
        break;
    }
    default:
        LOG_WARN("Node %d: unknown message type %d", node_id_, msg_type);
        break;
    }
}

int RaftNode::submit_command(const std::string &cmd)
{
    if (role_.load() != RaftRole::Leader)
    {
        LOG_WARN("Node %d: not leader, cannot submit command", node_id_);
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t new_index = log_buffer_.get_last_index() + 1;
    LogEntry entry(current_term_.load(), new_index, cmd);

    if (log_buffer_.append(entry))
    {
        LOG_INFO("Node %d: appended command at index %u", node_id_, new_index);
        return static_cast<int>(new_index);
    }

    return -1;
}
