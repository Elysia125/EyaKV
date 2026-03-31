# EyaKV 压力测试报告

生成时间：2026-03-31 19:26:59

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
- 可用内存: 17.53 GB
- Python 版本: 3.12.0

---

## 二、测试配置

- 目标主机: 127.0.0.1:5210
- 认证密码: (未设置)
- 单次测试操作数: 50000
- Batch 模式: False
- Pipeline 模式: False
- Pipeline 批次大小: (默认: 50)
- stress_test 路径: build/bin/stress_test.exe

---

## 三、测试选择

- 单连接多数据结构测试: 跳过
- 连接数上限测试: 运行（目标连接数: 2000）
- 多线程吞吐量测试: 未启用（未指定 --threads 参数）

---

## 四、单连接多数据结构吞吐测试结果

_未能从输出中解析到单连接测试结果，请检查 stress_test 输出与解析逻辑。_

---

## 五、连接数上限测试结果

| 指标 | 数值 |
| --- | --- |
| 目标最大连接数 | 2000 |
| 实际成功建立连接数 | 1996 |


---

## 六、多连接（多线程）吞吐量测试结果

_未启用或未解析到多连接（多线程）吞吐量测试结果。_

---

## 七、stress_test 原始标准输出

```text
Authenticated. Starting Stress Test with 50000 items per type.
Pipeline mode: OFF (using single COMMAND per request)

Starting connection limit test (target: 2000 connections)...
Connection limit test finished: established 1996 / 2000 connections.

Stress Test Complete.
```



## 八、stress_test 原始标准错误输出

```text
Auth exception: recv header failed: 10053
Connection 1996 failed, stop.
```
