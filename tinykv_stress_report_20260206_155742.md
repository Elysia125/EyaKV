# TinyKV 压力测试报告

生成时间：2026-02-06 15:57:42

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
- 可用内存: 17.34 GB
- Python 版本: 3.12.0

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 1000
- Batch 模式: False
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
| 总操作数 | 10000 |
| 总耗时(s) | 66.450 |
| 吞吐量(ops/s) | 150.49 |


---

## 七、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 1000 items per type.

Starting multi-thread throughput test: 10 threads, 1000 ops per thread (String SET)...
Multi-thread throughput summary:
  Thread 0: 1000 ops, 15.06 ops/sec
  Thread 1: 1000 ops, 15.06 ops/sec
  Thread 2: 1000 ops, 15.06 ops/sec
  Thread 3: 1000 ops, 15.06 ops/sec
  Thread 4: 1000 ops, 15.07 ops/sec
  Thread 5: 1000 ops, 15.08 ops/sec
  Thread 6: 1000 ops, 15.07 ops/sec
  Thread 7: 1000 ops, 15.06 ops/sec
  Thread 8: 1000 ops, 15.07 ops/sec
  Thread 9: 1000 ops, 15.08 ops/sec
  Total: 10000 ops, 150.49 ops/sec (66.45s total)

Stress Test Complete.
```

