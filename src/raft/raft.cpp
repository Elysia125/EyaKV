#include "raft/raft.h"
#include <iostream>

namespace tinykv::raft {

void RaftNode::Start() {
    std::cout << "Raft node starting (stub)" << std::endl;
}

void RaftNode::Stop() {
    std::cout << "Raft node stopping (stub)" << std::endl;
}

} // namespace tinykv::raft
