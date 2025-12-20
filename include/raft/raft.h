#pragma once

#include <string>

namespace tinykv::raft {

class RaftNode {
public:
    RaftNode() = default;
    ~RaftNode() = default;

    void Start();
    void Stop();
};

} // namespace tinykv::raft
