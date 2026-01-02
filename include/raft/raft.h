#pragma once

#include <string>
#include "common/export.h"

namespace eyakv::raft {

class EYAKV_RAFT_API RaftNode {
public:
    RaftNode() = default;
    ~RaftNode() = default;

    void Start();
    void Stop();
};

} // namespace eyakv::raft
