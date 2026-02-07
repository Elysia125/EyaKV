# EyaKV 压力测试报告

生成时间：2026-02-07 15:45:25

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
- 可用内存: 1.13 GB
- Python 版本: 3.14.2

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 50000
- Batch 模式: False
- Pipeline 模式: False
- Pipeline 批次大小: (默认: 50)
- stress_test 路径: build/bin/stress_test

---

## 三、测试选择

- 单连接多数据结构测试: 跳过
- 连接数上限测试: 运行（目标连接数: 10000）
- 多线程吞吐量测试: 未启用（未指定 --threads 参数）

---

## 四、单连接多数据结构吞吐测试结果

_未能从输出中解析到单连接测试结果，请检查 stress_test 输出与解析逻辑。_

---

## 五、连接数上限测试结果

| 指标 | 数值 |
| --- | --- |
| 目标最大连接数 | 10000 |
| 实际成功建立连接数 | 9999 |


---

## 六、多连接（多线程）吞吐量测试结果

_未启用或未解析到多连接（多线程）吞吐量测试结果。_

---

## 七、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 50000 items per type.
Pipeline mode: OFF (using single COMMAND per request)

Starting connection limit test (target: 10000 connections)...
Connection limit test finished: established 9999 / 10000 connections.

Stress Test Complete.
```



## 八、stress_test 原始标准错误输出

```text
Auth exception: server closed connection
Connection 9999 failed, stop.
```
