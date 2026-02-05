# TinyKV 压力测试报告

生成时间：2026-02-05 20:04:39

---

## 一、系统环境信息

- 操作系统: Windows 11
- 内核版本: 10.0.26200
- 架构: AMD64
- CPU 标识: Intel64 Family 6 Model 186 Stepping 2, GenuineIntel
- 物理核心数: 10
- 逻辑核心数: 16
- CPU 最高频率: 2400.0 MHz
- 内存总量: 31.8 GB
- 可用内存: 17.08 GB
- Python 版本: 3.12.0

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 50000
- Batch 模式: True
- stress_test 路径: build/bin/stress_test.exe

---

## 三、单连接多数据结构吞吐测试结果

| 测试项 | 总操作数 | 总耗时(s) | 吞吐量(ops/s) | Batch 模式 |
| --- | --- | --- | --- | --- |
| String SET | 50000 | 1073.170 | 46.59 | 是 |

---

## 四、连接数上限测试结果

| 指标 | 数值 |
| --- | --- |
| 目标最大连接数 | 2000 |
| 实际成功建立连接数 | 0 |


---

## 五、多连接（多线程）吞吐量测试结果

| 指标 | 数值 |
| --- | --- |
| 线程数 | 8 |
| 总操作数 | 0 |
| 总耗时(s) | 2.050 |
| 吞吐量(ops/s) | 0.00 |


---

## 六、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 50000 items per type.

Starting String SET Benchmark (50000 items)...
Processed 10000...
Processed 20000...
Processed 30000...
Processed 40000...
Finished String SET: 46.59 ops/sec (1073.17s total)

Starting List RPUSH (Batch) Batch Benchmark (50000 items, batch size 100)...
Processed 10000...
Processed 20000...
Processed 30000...
Processed 40000...
Finished List RPUSH (Batch) (Batch): 2443.50 items/sec (20.46s total)

Starting Set SADD (Batch) Batch Benchmark (50000 items, batch size 100)...
Processed 10000...
Processed 20000...
Processed 30000...
Processed 40000...
Finished Set SADD (Batch) (Batch): 2274.80 items/sec (21.98s total)

Starting ZSet ZADD (Batch) Batch Benchmark (50000 items, batch size 100)...
Processed 10000...
Processed 20000...
Processed 30000...
Processed 40000...
Finished ZSet ZADD (Batch) (Batch): 199.23 items/sec (250.97s total)

Starting Hash HSET (Batch) Batch Benchmark (50000 items, batch size 100)...
Processed 10000...
Finished Hash HSET (Batch) (Batch): 7845.44 items/sec (6.37s total)

Starting connection limit test (target: 2000 connections)...
Connection limit test finished: established 0 / 2000 connections.

Starting multi-thread throughput test: 8 threads, 50000 ops per thread (String SET)...
Multi-thread throughput summary:
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Thread 0: 0 ops, 0.00 ops/sec
  Total: 0 ops, 0.00 ops/sec (2.05s total)

Stress Test Complete.
```



## 七、stress_test 原始标准错误输出

```text
Exception at batch 103
Connect failed
Connection 0 failed, stop.
Connect failedConnect failedConnect failed
Thread 
Connect failedConnect failedThread 7: connection/auth failed, aborting thread.


Thread Thread 34: connection/auth failed, aborting thread.: connection/auth failed, aborting thread.

Connect failed
Thread 5: connection/auth failed, aborting thread.
Connect failed
Thread 0: connection/auth failed, aborting thread.
Connect failed
Thread 6: connection/auth failed, aborting thread.

Thread 1: connection/auth failed, aborting thread.
2: connection/auth failed, aborting thread.
```
