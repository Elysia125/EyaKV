# EyaKV 压力测试结论与性能提升建议

基于四份压测报告与代码阅读，总结现象与可落地的优化方向。

---

## 一、压测数据小结

| 报告 | Pipeline | 线程数 | 总操作数 | 总耗时(s) | 吞吐量(ops/s) |
|------|----------|--------|----------|-----------|----------------|
| 110834 | OFF | 10 | 10000 | 52.29 | **191** |
| 111059 | OFF | 1  | 10000 | 57.76 | **173** |
| 110650 | ON (batch=100) | 1  | 10000 | 3.63  | **2758** |
| 110335 | ON (batch=100) | 10 | 10000 | 1.21  | **8295** |

结论简要：

- **关 pipeline、单条 COMMAND**：约 19 ops/s/线程，受 RTT 限制，加线程也难提升。
- **开 pipeline、BATCH_COMMAND**：单线程约 2758 ops/s，10 线程约 8295 ops/s，**pipeline 带来约 15–40 倍提升**。
- 当前瓶颈已从「网络往返」转为「服务端处理 + 少量客户端并发」。

---

## 二、建议的优化方向（按优先级）

### 1. 客户端 / 压测参数（立即可做）

- **增大 pipeline 批次**  
  - 当前 `--pipeline-batch 100`，可尝试 **200、500**。  
  - 在 RTT 不变的前提下，更大 batch 能进一步摊薄每次往返成本，吞吐有望再升。  
  - 需注意单次请求体不要过大（例如可先卡在 1MB 以内，与配置中的 `DEFAULT_BATCH_MAX_SIZE_BYTES` 一致）。

- **适度增加线程数**  
  - 你已有 10 核/16 逻辑核，可试 **16 或 20 线程**，观察总 QPS 是否继续线性上升。  
  - 若上升变缓，说明开始顶到服务端单机瓶颈（CPU/锁/磁盘）。

- **压测时固定使用 pipeline**  
  - 生产或高 QPS 场景建议始终带 `--pipeline` 和合适的 `--pipeline-batch`，避免退化成单条 COMMAND。

---

### 2. 服务端：批量写等待逻辑（代码小改、收益明确）

**位置**：`src/raft/raft.cpp` 中 `execute_batch_write_command`，在 `need_majority_confirm_ == true` 时的等待循环。

**现状**：对 batch 内每条 log 各有一个 `pending_req`，当前实现是**按顺序**对每个 `pending_req->future.wait_for(timeout_ms)`。batch 内条目往往一起 commit、一起 apply，因此会依次被 notify，逻辑上等价于「等最后一条 apply 完」，但多了 N 次 `wait_for` 与唤醒。

**建议**：只等待**最后一条**（最大 index）的 future，超时后再统一填 `responses` 并清理 `pending_requests_`：

- 仅对 `pending_reqs.back()` 调用一次 `wait_for(timeout_ms)`。
- 若未超时，再对 `i = 0..size-1` 用 `pending_reqs[i]->future.get()` 填 `responses[entries[i].request_id]`（此时通常已 ready），并统一 `pending_requests_.erase(entries[j].index)`。
- 若超时，与现在一样，对未完成的 index 填 timeout 错误并 erase。

这样减少 wait/唤醒次数，降低批量写路径上的延迟抖动，有利于高 batch 下的吞吐与 P99。

---

### 3. 服务端：批量超时与配置

- **batch_command_timeout_ms**（默认 500ms）  
  - 整批命令共用的超时。若 batch 较大或磁盘/复制较慢，可适当调大（如 1000–2000ms），避免正常负载下频繁 timeout。  
  - 配置方式：确认 `config` 中 `BATCH_TIMEOUT_KEY` / `DEFAULT_BATCH_TIMEOUT_MS` 的读取与默认值，必要时在配置文件中显式设置。

- **need_majority_confirm_**  
  - 单节点部署时保持 **false**（默认），走「直接 commit + result_cache」路径，避免无谓的多数派等待。  
  - 多节点时再设为 true，并配合上面的「只等最后一条」优化。

---

### 4. 服务端：WAL 与磁盘

- **当前**：`raft_log_array.cpp` 中 `write_batch_to_wal` 对每批只做 `fflush`，没有对 WAL 文件做 `fsync`；`sync_metadata()` 里会对已打开的 `wal_file_` 做 `fsync`（在别处调用）。  
- **建议**：  
  - 若可接受「单机宕机可能丢最后几秒未 fsync 的写」，可在配置中增加「WAL 每批只 fflush、每 N 批或每 T 秒 fsync」的策略，减少磁盘同步次数，提升写入吞吐。  
  - 若要求持久化更强，可保持「每批或每 N 条 fsync」，但用**单次 fsync 覆盖整批**，而不是每一条一次 fsync。

---

### 5. 服务端：请求处理与线程模型

- **BATCH_COMMAND 路径**：`tcp_server` 里已对 BATCH_COMMAND 正确调用 `submit_batch_command` 并 `send(serialize({request->id, batch_responses}), client_sock)`，逻辑正确。  
- **线程池**：若 `thread_pool_` 的 worker 数偏小，高并发多连接时可能排队。可适当增大线程池大小（例如 ≥ CPU 逻辑核数），并观察 CPU 使用率与 QPS。  
- **连接与 backlog**：确认 `connect_wait_queue_size_` 等不会在压测时成为瓶颈（报告里未体现连接被拒，可暂维持现状）。

---

### 6. Raft 与复制（多节点时）

- **append_entries_max_batch**：控制每次 append_entries 带上的日志条数。batch 写时一次会 append 很多条，若网络/ follower 处理能力足够，可适当调大该值，减少 RPC 次数，加快复制。  
- **单节点压测**：当前报告应为单节点；多节点时再重点调复制与 `need_majority_confirm_`。

---

### 7. 存储与 execute_command

- 批量写时，每条 log 在 apply 时仍会**逐条**调用 `execute_command(entry.cmd)`。若底层存储（如 LSM/引擎）支持 **batch write**，可在此处做「按批提交」的接口，减少锁/IO 次数，进一步提升 apply 阶段吞吐。  
- 若当前存储为单条 put，可先做上述 1–4 的优化，再视 CPU/IO 瓶颈决定是否改存储接口。

---

## 三、建议的下一步操作顺序

1. **不改代码**：用当前二进制做一组对比压测（例如 `--pipeline --pipeline-batch 200` 或 `500`，`--threads 16`），记录 QPS 与 P99，建立基线。  
2. **改一处代码**：实现「批量写只等最后一条 future」的优化，再压测同参数，看吞吐与延迟是否改善。  
3. **调配置**：根据部署形态（单机/集群）调整 `batch_command_timeout_ms`、`need_majority_confirm_`、线程池大小等，并复测。  
4. **视情况**：再做 WAL 的 fsync 策略或存储层 batch 写（若 profiling 显示磁盘或 apply 是瓶颈）。

按上述顺序，可以在不改动整体架构的前提下，明显提升吞吐并降低延迟抖动；后续再根据 profiling 针对 Raft、存储做更深优化。
