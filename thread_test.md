# EyaKV 压力测试报告

生成时间：2026-03-29 18:15:20

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
- 可用内存: 17.22 GB
- Python 版本: 3.12.0

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 10000
- Batch 模式: False
- Pipeline 模式: True
- Pipeline 批次大小: 100
- stress_test 路径: build/bin/stress_test.exe

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
| 总操作数 | 100000 |
| 总耗时(s) | 6.990 |
| 吞吐量(ops/s) | 14314.56 |


---

## 七、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 10000 items per type.
Pipeline mode: ON, batch size = 100 (using BATCH_COMMAND)

Starting multi-thread throughput test: 10 threads, 10000 ops per thread (String SET), pipeline batch 100...
Multi-thread throughput summary:
  Thread 0: 10000 ops, 1432.13 ops/sec
  Thread 1: 10000 ops, 1432.35 ops/sec
  Thread 2: 10000 ops, 1432.45 ops/sec
  Thread 3: 10000 ops, 1433.04 ops/sec
  Thread 4: 10000 ops, 1432.16 ops/sec
  Thread 5: 10000 ops, 1432.27 ops/sec
  Thread 6: 10000 ops, 1432.74 ops/sec
  Thread 7: 10000 ops, 1432.79 ops/sec
  Thread 8: 10000 ops, 1433.05 ops/sec
  Thread 9: 10000 ops, 1438.96 ops/sec
  Total: 100000 ops, 14314.56 ops/sec (6.99s total)

Stress Test Complete.
```

