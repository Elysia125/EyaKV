# TinyKV 压力测试报告

生成时间：2026-02-06 15:14:02

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
- 可用内存: 16.1 GB
- Python 版本: 3.12.0

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 50000
- Batch 模式: False
- stress_test 路径: build/bin/stress_test.exe

---

## 三、测试选择

- 单连接多数据结构测试: 跳过
- 连接数上限测试: 运行（目标连接数: 1000）
- 多线程吞吐量测试: 运行（线程数: 10）

---

## 四、单连接多数据结构吞吐测试结果

_未能从输出中解析到单连接测试结果，请检查 stress_test 输出与解析逻辑。_

---

## 五、连接数上限测试结果

| 指标 | 数值 |
| --- | --- |
| 目标最大连接数 | 1000 |
| 实际成功建立连接数 | 1000 |


---

## 六、多连接（多线程）吞吐量测试结果

| 指标 | 数值 |
| --- | --- |
| 线程数 | 10 |
| 总操作数 | 500000 |
| 总耗时(s) | 6184.690 |
| 吞吐量(ops/s) | 80.84 |


---

## 七、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 50000 items per type.

Starting connection limit test (target: 1000 connections)...
Connection limit test finished: established 1000 / 1000 connections.

Starting multi-thread throughput test: 10 threads, 50000 ops per thread (String SET)...
Multi-thread throughput summary:
  Thread 0: 50000 ops, 8.09 ops/sec
  Thread 1: 50000 ops, 8.09 ops/sec
  Thread 2: 50000 ops, 8.09 ops/sec
  Thread 3: 50000 ops, 8.09 ops/sec
  Thread 4: 50000 ops, 8.09 ops/sec
  Thread 5: 50000 ops, 8.09 ops/sec
  Thread 6: 50000 ops, 8.09 ops/sec
  Thread 7: 50000 ops, 8.09 ops/sec
  Thread 8: 50000 ops, 8.09 ops/sec
  Thread 9: 50000 ops, 8.09 ops/sec
  Total: 500000 ops, 80.84 ops/sec (6184.69s total)

Stress Test Complete.
```

