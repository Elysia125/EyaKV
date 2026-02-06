# TinyKV 压力测试使用说明

## 概述

TinyKV 的压力测试系统现在支持选择性运行测试，你可以选择运行一个或多个测试。

## 支持的测试类型

### 1. 单连接多数据结构测试
测试 String、List、Set、ZSet、Hash 五种数据结构的性能。

### 2. 连接数上限测试
测试服务器能同时处理的最大连接数。

### 3. 多线程吞吐量测试
测试多线程并发场景下的吞吐量。

## 使用方法

### 基本用法（运行所有启用的测试）

```bash
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --count 50000 \
    --batch \
    --conn-limit 1000 \
    --threads 10 \
    --output report.md
```

### 选择性运行测试

#### 方法一：使用 --skip-* 参数跳过某些测试

```bash
# 跳过单连接多数据结构测试，只运行连接数测试和多线程测试
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --skip-single \
    --conn-limit 1000 \
    --threads 10

# 跳过连接数测试，只运行单连接和多线程测试
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --count 50000 \
    --skip-conn-limit \
    --threads 10

# 跳过多线程测试，只运行单连接和连接数测试
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --count 50000 \
    --skip-multi-thread \
    --conn-limit 1000
```

#### 方法二：使用 --only-* 参数只运行特定测试

```bash
# 只运行单连接多数据结构测试
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --count 50000 \
    --only-single

# 只运行连接数上限测试
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --conn-limit 1000 \
    --only-conn-limit

# 只运行多线程吞吐量测试
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --threads 10 \
    --only-multi-thread
```

### 直接使用 stress_test 可执行文件

如果你不想使用 Python 脚本，也可以直接调用 `stress_test` 可执行文件：

```bash
# 运行所有测试（需要指定 conn-limit 和 threads）
build/bin/stress_test.exe \
    -h 127.0.0.1 \
    -p 5210 \
    -n 50000 \
    --batch \
    --conn-limit 1000 \
    --threads 10

# 只运行单连接测试（不指定 conn-limit 和 threads）
build/bin/stress_test.exe \
    -h 127.0.0.1 \
    -p 5210 \
    -n 50000 \
    --batch

# 跳过单连接测试，只运行连接数测试
build/bin/stress_test.exe \
    -h 127.0.0.1 \
    -p 5210 \
    --skip-single \
    --conn-limit 1000

# 跳过单连接和连接数测试，只运行多线程测试
build/bin/stress_test.exe \
    -h 127.0.0.1 \
    -p 5210 \
    --skip-single \
    --skip-conn-limit \
    --threads 10
```

## 参数说明

### Python 脚本参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `--stress-bin` | stress_test 可执行文件路径 | `build/bin/stress_test[.exe]` |
| `--host` | TinyKV 服务器地址 | `127.0.0.1` |
| `--port` | TinyKV 服务器端口 | `5210` |
| `--password` | 认证密码 | 空 |
| `--count` | 每类数据结构的操作数量 | `50000` |
| `--batch` | 启用批量模式 | False |
| `--conn-limit` | 连接数上限测试的目标连接数 | None（不运行） |
| `--threads` | 多线程测试的线程数 | None（不运行） |
| `--skip-single` | 跳过单连接多数据结构测试 | False |
| `--skip-conn-limit` | 跳过连接数上限测试 | False |
| `--skip-multi-thread` | 跳过多线程吞吐量测试 | False |
| `--only-single` | 只运行单连接多数据结构测试 | False |
| `--only-conn-limit` | 只运行连接数上限测试 | False |
| `--only-multi-thread` | 只运行多线程吞吐量测试 | False |
| `--output` | 输出报告文件路径 | `tinykv_stress_report_时间戳.md` |

### stress_test 可执行文件参数

| 参数 | 说明 | 默认值 |
|------|------|--------|
| `-h` | 服务器地址 | `127.0.0.1` |
| `-p` | 服务器端口 | `5210` |
| `-a` | 认证密码 | 空 |
| `-n` | 每类数据结构的操作数量 | `50000` |
| `--batch` | 启用批量模式 | False |
| `--threads` | 多线程测试的线程数 | 0（不运行） |
| `--conn-limit` | 连接数上限测试的目标连接数 | 0（不运行） |
| `--skip-single` | 跳过单连接多数据结构测试 | False |
| `--skip-conn-limit` | 跳过连接数上限测试 | False |
| `--skip-multi-thread` | 跳过多线程吞吐量测试 | False |

## 示例场景

### 场景 1：快速测试 String 性能

```bash
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --only-single \
    --count 10000 \
    --output string_test.md
```

### 场景 2：测试最大连接数

```bash
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --only-conn-limit \
    --conn-limit 10000 \
    --output conn_test.md
```

### 场景 3：测试多线程吞吐量

```bash
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --only-multi-thread \
    --threads 20 \
    --output thread_test.md
```

### 场景 4：完整压力测试（所有测试）

```bash
python stress_runner.py \
    --stress-bin build/bin/stress_test.exe \
    --host 127.0.0.1 \
    --port 5210 \
    --count 50000 \
    --batch \
    --conn-limit 1000 \
    --threads 10 \
    --output full_test.md
```

## 输出报告

生成的 Markdown 报告包含以下章节：

1. 系统环境信息
2. 测试配置
3. 测试选择（显示哪些测试被跳过）
4. 单连接多数据结构吞吐测试结果
5. 连接数上限测试结果
6. 多连接（多线程）吞吐量测试结果
7. stress_test 原始标准输出
8. stress_test 原始标准错误输出（如果有）

## 注意事项

1. **--only-* 参数与 --skip-* 参数不能混用**
   - 使用 `--only-single` 会自动跳过其他测试
   - 使用 `--skip-*` 可以自由组合

2. **必须指定相应参数才能运行测试**
   - 运行连接数测试必须指定 `--conn-limit`
   - 运行多线程测试必须指定 `--threads`

3. **测试顺序**
   - 默认情况下，测试按以下顺序运行：
     1. 单连接多数据结构测试
     2. 连接数上限测试
     3. 多线程吞吐量测试

4. **性能考虑**
   - 建议先运行单个测试来评估性能
   - 根据服务器配置调整 `--count`、`--threads` 等参数
   - 生产环境测试时建议使用较小的测试规模

## 故障排查

### 问题：测试报告显示"未启用"

**原因**：没有指定相应的参数（如 `--conn-limit` 或 `--threads`）

**解决**：确保指定了运行测试所需的所有参数

### 问题：错误信息说"没有选择任何测试运行"

**原因**：使用了 `--skip-*` 参数跳过了所有测试

**解决**：
- 使用 `--only-*` 参数只运行特定测试
- 或减少 `--skip-*` 参数，保留至少一个测试

### 问题：stress_test 返回非零退出码

**可能原因**：
- 服务器未启动
- 连接参数错误
- 认证失败

**解决**：
1. 检查服务器是否正常运行
2. 确认主机和端口正确
3. 检查密码是否正确
4. 查看服务器日志了解详细错误信息
