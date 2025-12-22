#pragma once

#include <string>

namespace eyakv::raft {

class RaftNode {
public:
    RaftNode() = default;
    ~RaftNode() = default;

    void Start();
    void Stop();
};

} // namespace eyakv::raft
