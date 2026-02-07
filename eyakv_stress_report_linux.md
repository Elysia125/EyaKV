# EyaKV 压力测试报告

生成时间：2026-02-07 15:42:52

---

## 一、系统环境信息

- 操作系统: Linux 6.18.7-200.fc43.x86_64
- 内核版本: #1 SMP PREEMPT_DYNAMIC Fri Jan 23 16:42:34 UTC 2026
- 架构: x86_64
- CPU 标识: unknown
- 物理核心数: 8
- 逻辑核心数: 8
- CPU 最高频率: 0.0 MHz
- 内存总量: 3.77 GB
- 可用内存: 1.07 GB
- Python 版本: 3.14.2

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 50000
- Batch 模式: False
- Pipeline 模式: True
- Pipeline 批次大小: 100
- stress_test 路径: build/bin/stress_test

---

## 三、测试选择

- 单连接多数据结构测试: 跳过
- 连接数上限测试: 未启用（未指定 --conn-limit 参数）
- 多线程吞吐量测试: 运行（线程数: 10）

---

## 四、单连接多数据结构吞吐测试结果

_未能从输出中解析到单连接测试结果，请检查 stress_test 输出与解析逻辑。_

---

## 五、连接数上限测试结果

_未启用或未解析到连接数上限测试结果。_

---

## 六、多连接（多线程）吞吐量测试结果

| 指标 | 数值 |
| --- | --- |
| 线程数 | 10 |
| 总操作数 | 500000 |
| 总耗时(s) | 56.100 |
| 吞吐量(ops/s) | 8912.82 |


---

## 七、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 50000 items per type.
Pipeline mode: ON, batch size = 100 (using BATCH_COMMAND)

Starting multi-thread throughput test: 10 threads, 50000 ops per thread (String SET), pipeline batch 100...
Multi-thread throughput summary:
  Thread 0: 50000 ops, 892.32 ops/sec
  Thread 1: 50000 ops, 892.29 ops/sec
  Thread 2: 50000 ops, 891.32 ops/sec
  Thread 3: 50000 ops, 891.31 ops/sec
  Thread 4: 50000 ops, 891.31 ops/sec
  Thread 5: 50000 ops, 891.35 ops/sec
  Thread 6: 50000 ops, 891.34 ops/sec
  Thread 7: 50000 ops, 891.31 ops/sec
  Thread 8: 50000 ops, 891.31 ops/sec
  Thread 9: 50000 ops, 891.35 ops/sec
  Total: 500000 ops, 8912.82 ops/sec (56.10s total)

Stress Test Complete.
```
