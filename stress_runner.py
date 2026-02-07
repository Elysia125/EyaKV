#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
EyaKV 全面压力测试脚本

功能：
1. 采集当前操作系统、CPU、内存等信息
2. 调用 EyaKV 的 C++ 压测程序 stress_test（apps/client/stress_test.cpp 编译产物）
3. 解析 stress_test 输出中每一类测试结果
4. 生成一份可读性较高的 Markdown 压测报告

依赖：
- Python 3.7+
- 建议安装 psutil：pip install psutil

用法示例（在工程根目录或 build 目录下）：
    python stress_runner.py \
        --stress-bin build/bin/stress_test \
        --host 127.0.0.1 \
        --port 5210 \
        --password 123456 \
        --count 50000 \
        --batch \
        --output tinykv_stress_report.md
"""

import argparse
import datetime
import os
import platform
import re
import subprocess
import textwrap
from typing import Any, Dict, List, Optional


def collect_system_info() -> Dict[str, Any]:
    """采集操作系统 / CPU / 内存等信息。"""
    info: Dict[str, Any] = {}

    info["os_system"] = platform.system()
    info["os_release"] = platform.release()
    info["os_version"] = platform.version()
    info["machine"] = platform.machine()
    info["processor"] = platform.processor() or "unknown"
    info["python_version"] = platform.python_version()

    # CPU / 内存信息，优先使用 psutil
    try:
        import psutil  # type: ignore

        info["cpu_physical_cores"] = psutil.cpu_count(logical=False)
        info["cpu_logical_cores"] = psutil.cpu_count(logical=True)
        freq = psutil.cpu_freq()
        if freq:
            info["cpu_max_freq_mhz"] = round(freq.max, 2)
            info["cpu_min_freq_mhz"] = round(freq.min, 2)
            info["cpu_current_freq_mhz"] = round(freq.current, 2)

        vm = psutil.virtual_memory()
        info["memory_total_gb"] = round(vm.total / (1024 ** 3), 2)
        info["memory_available_gb"] = round(vm.available / (1024 ** 3), 2)
    except ImportError:
        # 无 psutil 时的降级方案
        info["cpu_logical_cores"] = os.cpu_count()
        info["cpu_physical_cores"] = None
        info["cpu_max_freq_mhz"] = None
        info["memory_total_gb"] = None
        info["memory_available_gb"] = None

    return info


def run_stress_test(
    stress_bin: str,
    host: str,
    port: int,
    password: str,
    count: int,
    batch: bool,
    pipeline: bool = False,
    pipeline_batch: Optional[int] = None,
    threads: Optional[int] = None,
    conn_limit: Optional[int] = None,
    skip_single: bool = False,
    skip_conn_limit: bool = False,
    skip_multi_thread: bool = False,
    extra_env: Optional[Dict[str, str]] = None,
) -> Dict[str, Any]:
    """
    调用 C++ 的 stress_test 可执行文件，并解析其输出。
    通过传入 threads / conn_limit，可以在一次执行中完成：
      1）单连接下多种数据结构吞吐测试
      2）连接数上限测试
      3）多连接（多线程）吞吐量测试

    通过 skip_single / skip_conn_limit / skip_multi_thread 控制跳过哪些测试。
    """
    if not os.path.isfile(stress_bin):
        raise FileNotFoundError(f"stress_test 可执行文件不存在: {stress_bin}")

    cmd = [stress_bin, "-h", host, "-p", str(port), "-n", str(count)]
    if password:
        cmd.extend(["-a", password])
    if batch:
        cmd.append("--batch")
    if pipeline:
        cmd.append("--pipeline")
        if pipeline_batch is not None and pipeline_batch > 0:
            cmd.extend(["--pipeline-batch", str(pipeline_batch)])
    if conn_limit is not None and conn_limit > 0:
        cmd.extend(["--conn-limit", str(conn_limit)])
    if threads is not None and threads > 0:
        cmd.extend(["--threads", str(threads)])
    if skip_single:
        cmd.append("--skip-single")
    if skip_conn_limit:
        cmd.append("--skip-conn-limit")
    if skip_multi_thread:
        cmd.append("--skip-multi-thread")

    env = os.environ.copy()
    if extra_env:
        env.update(extra_env)

    proc = subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        env=env,
        check=False,
    )

    stdout = proc.stdout
    stderr = proc.stderr
    returncode = proc.returncode

    # 解析单连接多数据结构吞吐结果行：
    #   Finished String SET: 12345.67 ops/sec (4.05s total)
    #   Finished String SET (Pipeline): 12345.67 items/sec (4.05s total)
    single_pattern = re.compile(
        r"Finished\s+(.+?):\s+([0-9.]+)\s+(?:ops|items)/sec\s+\(([0-9.]+)s\s+total\)",
        re.IGNORECASE,
    )

    single_results: List[Dict[str, Any]] = []
    for line in stdout.splitlines():
        m = single_pattern.search(line)
        if m:
            test_name = m.group(1).strip()
            ops_per_sec = float(m.group(2))
            duration_s = float(m.group(3))
            is_pipeline = "(Pipeline)" in test_name
            single_results.append(
                {
                    "test_name": test_name,
                    "total_ops": count,
                    "duration_seconds": duration_s,
                    "ops_per_sec": ops_per_sec,
                    "batch_mode": batch and not is_pipeline,
                    "pipeline_mode": is_pipeline,
                }
            )

    # 解析连接数上限测试结果：
    #   Connection limit test finished: established 123 / 1000 connections.
    conn_limit_result: Optional[Dict[str, Any]] = None
    if conn_limit is not None and conn_limit > 0:
        m = re.search(
            r"Connection limit test finished:\s+established\s+(\d+)\s*/\s*(\d+)\s+connections\.",
            stdout,
        )
        if m:
            conn_limit_result = {
                "established": int(m.group(1)),
                "target": int(m.group(2)),
            }

    # 解析多线程总吞吐结果：
    #   Total: 12345 ops, 6789.01 ops/sec (1.23s total)
    multi_thread_result: Optional[Dict[str, Any]] = None
    if threads is not None and threads > 0:
        m = re.search(
            r"Total:\s+(\d+)\s+ops,\s+([0-9.]+)\s+ops/sec\s+\(([0-9.]+)s total\)",
            stdout,
        )
        if m:
            total_ops = int(m.group(1))
            qps = float(m.group(2))
            duration_s = float(m.group(3))
            multi_thread_result = {
                "threads": threads,
                "total_ops": total_ops,
                "duration_seconds": duration_s,
                "ops_per_sec": qps,
            }

    return {
        "command": cmd,
        "returncode": returncode,
        "stdout": stdout,
        "stderr": stderr,
        "parsed_results": single_results,
        "conn_limit_result": conn_limit_result,
        "multi_thread_result": multi_thread_result,
    }


def format_markdown_report(
    system_info: Dict[str, Any],
    test_config: Dict[str, Any],
    stress_result: Dict[str, Any],
) -> str:
    """把系统信息 + 测试配置 + 压测结果拼成一份 Markdown 报告。"""
    now = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

    sys_lines = [
        f"- 操作系统: {system_info.get('os_system')} {system_info.get('os_release')}",
        f"- 内核版本: {system_info.get('os_version')}",
        f"- 架构: {system_info.get('machine')}",
        f"- CPU 标识: {system_info.get('processor')}",
        f"- 物理核心数: {system_info.get('cpu_physical_cores')}",
        f"- 逻辑核心数: {system_info.get('cpu_logical_cores')}",
        f"- CPU 最高频率: {system_info.get('cpu_max_freq_mhz')} MHz",
        f"- 内存总量: {system_info.get('memory_total_gb')} GB",
        f"- 可用内存: {system_info.get('memory_available_gb')} GB",
        f"- Python 版本: {system_info.get('python_version')}",
    ]

    cfg_lines = [
        f"- 目标主机: {test_config.get('host')}:{test_config.get('port')}",
        f"- 认证密码: {'(已设置)' if test_config.get('password') else '(未设置)'}",
        f"- 单次测试操作数: {test_config.get('count')}",
        f"- Batch 模式: {test_config.get('batch')}",
        f"- Pipeline 模式: {test_config.get('pipeline')}",
        f"- Pipeline 批次大小: {test_config.get('pipeline_batch') if test_config.get('pipeline_batch') else '(默认: 50)'}",
        f"- stress_test 路径: {test_config.get('stress_bin')}",
    ]

    # 添加测试选择信息
    test_selection_lines = []
    if not test_config.get('skip_single'):
        test_selection_lines.append(f"- 单连接多数据结构测试: 运行")
    else:
        test_selection_lines.append(f"- 单连接多数据结构测试: 跳过")

    if test_config.get('conn_limit') is not None and test_config.get('conn_limit') > 0:
        if not test_config.get('skip_conn_limit'):
            test_selection_lines.append(f"- 连接数上限测试: 运行（目标连接数: {test_config.get('conn_limit')}）")
        else:
            test_selection_lines.append(f"- 连接数上限测试: 跳过")
    else:
        test_selection_lines.append(f"- 连接数上限测试: 未启用（未指定 --conn-limit 参数）")

    if test_config.get('threads') is not None and test_config.get('threads') > 0:
        if not test_config.get('skip_multi_thread'):
            test_selection_lines.append(f"- 多线程吞吐量测试: 运行（线程数: {test_config.get('threads')}）")
        else:
            test_selection_lines.append(f"- 多线程吞吐量测试: 跳过")
    else:
        test_selection_lines.append(f"- 多线程吞吐量测试: 未启用（未指定 --threads 参数）")

    # 单连接多数据结构测试结果表
    parsed_results: List[Dict[str, Any]] = stress_result.get("parsed_results", [])
    if parsed_results:
        header_single = "| 测试项 | 总操作数 | 总耗时(s) | 吞吐量(ops/s) | Batch 模式 | Pipeline 模式 |\n"
        header_single += "| --- | --- | --- | --- | --- | --- |\n"
        rows_single = []
        for r in parsed_results:
            rows_single.append(
                "| {name} | {ops} | {dur:.3f} | {qps:.2f} | {batch} | {pipeline} |".format(
                    name=r["test_name"],
                    ops=r["total_ops"],
                    dur=r["duration_seconds"],
                    qps=r["ops_per_sec"],
                    batch="是" if r.get("batch_mode", False) else "否",
                    pipeline="是" if r.get("pipeline_mode", False) else "否",
                )
            )
        single_table = header_single + "\n".join(rows_single)
    else:
        single_table = "_未能从输出中解析到单连接测试结果，请检查 stress_test 输出与解析逻辑。_"

    # 连接数上限测试结果
    conn_limit_result = stress_result.get("conn_limit_result")
    if conn_limit_result:
        conn_table = (
            "| 指标 | 数值 |\n"
            "| --- | --- |\n"
            f"| 目标最大连接数 | {conn_limit_result['target']} |\n"
            f"| 实际成功建立连接数 | {conn_limit_result['established']} |\n"
        )
    else:
        conn_table = "_未启用或未解析到连接数上限测试结果。_"

    # 多连接（多线程）吞吐量测试结果
    mt_result = stress_result.get("multi_thread_result")
    if mt_result:
        mt_table = (
            "| 指标 | 数值 |\n"
            "| --- | --- |\n"
            f"| 线程数 | {mt_result['threads']} |\n"
            f"| 总操作数 | {mt_result['total_ops']} |\n"
            f"| 总耗时(s) | {mt_result['duration_seconds']:.3f} |\n"
            f"| 吞吐量(ops/s) | {mt_result['ops_per_sec']:.2f} |\n"
        )
    else:
        mt_table = "_未启用或未解析到多连接（多线程）吞吐量测试结果。_"

    raw_stdout = stress_result.get("stdout", "")
    raw_stderr = stress_result.get("stderr", "")

    md = f"""# EyaKV 压力测试报告

生成时间：{now}

---

## 一、系统环境信息

{os.linesep.join(sys_lines)}

---

## 二、测试配置

{os.linesep.join(cfg_lines)}

---

## 三、测试选择

{os.linesep.join(test_selection_lines)}

---

## 四、单连接多数据结构吞吐测试结果

{single_table}

---

## 五、连接数上限测试结果

{conn_table}

---

## 六、多连接（多线程）吞吐量测试结果

{mt_table}

---

## 七、stress_test 原始标准输出

```text
{textwrap.dedent(raw_stdout).strip()}
```

"""

    if raw_stderr.strip():
        md += f"""

## 八、stress_test 原始标准错误输出

```text
{textwrap.dedent(raw_stderr).strip()}
```
"""

    return md


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="EyaKV 全面压力测试脚本（调用 C++ stress_test 并生成 Markdown 报告）"
    )
    parser.add_argument(
        "--stress-bin",
        default="build/bin/stress_test" if os.name != "nt" else "build/bin/stress_test.exe",
        help="stress_test 可执行文件路径（默认：build/bin/stress_test[.exe]）",
    )
    parser.add_argument("--host", default="127.0.0.1", help="EyaKV 服务器地址")
    parser.add_argument("--port", type=int, default=5210, help="EyaKV 服务器端口")
    parser.add_argument("--password", default="", help="认证密码")
    parser.add_argument(
        "--count", type=int, default=50000, help="每类数据结构的操作数量（传给 -n）"
    )
    parser.add_argument(
        "--batch",
        action="store_true",
        help="是否启用 stress_test 的 --batch 模式（批量命令）",
    )
    parser.add_argument(
        "--pipeline",
        action="store_true",
        help="是否启用 pipeline 模式（批量命令，使用 BATCH_COMMAND）",
    )
    parser.add_argument(
        "--pipeline-batch",
        type=int,
        default=None,
        help="pipeline 模式的批次大小（默认：50）",
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=None,
        help="多连接（多线程）吞吐量测试的线程数，对应 stress_test 的 --threads 参数",
    )
    parser.add_argument(
        "--conn-limit",
        type=int,
        default=None,
        help="连接数上限测试的目标连接数，对应 stress_test 的 --conn-limit 参数",
    )

    # 测试选择参数
    test_selection_group = parser.add_argument_group("测试选择（默认运行所有启用的测试）")
    test_selection_group.add_argument(
        "--skip-single",
        action="store_true",
        help="跳过单连接多数据结构测试（String/List/Set/ZSet/Hash）",
    )
    test_selection_group.add_argument(
        "--skip-conn-limit",
        action="store_true",
        help="跳过连接数上限测试",
    )
    test_selection_group.add_argument(
        "--skip-multi-thread",
        action="store_true",
        help="跳过多线程吞吐量测试",
    )

    # 便捷选项：只运行特定测试
    test_selection_group.add_argument(
        "--only-single",
        action="store_true",
        help="只运行单连接多数据结构测试",
    )
    test_selection_group.add_argument(
        "--only-conn-limit",
        action="store_true",
        help="只运行连接数上限测试（需要同时指定 --conn-limit）",
    )
    test_selection_group.add_argument(
        "--only-multi-thread",
        action="store_true",
        help="只运行多线程吞吐量测试（需要同时指定 --threads）",
    )

    parser.add_argument(
        "--output",
        default=None,
        help="输出报告文件路径（默认：tinykv_stress_report_时间戳.md）",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()

    # 处理便捷选项（--only-*）
    skip_single = args.skip_single
    skip_conn_limit = args.skip_conn_limit
    skip_multi_thread = args.skip_multi_thread

    if args.only_single:
        skip_conn_limit = True
        skip_multi_thread = True
    elif args.only_conn_limit:
        if args.conn_limit is None or args.conn_limit <= 0:
            print("错误：使用 --only-conn-limit 必须同时指定 --conn-limit 参数")
            return
        skip_single = True
        skip_multi_thread = True
    elif args.only_multi_thread:
        if args.threads is None or args.threads <= 0:
            print("错误：使用 --only-multi-thread 必须同时指定 --threads 参数")
            return
        skip_single = True
        skip_conn_limit = True

    system_info = collect_system_info()

    test_config = {
        "host": args.host,
        "port": args.port,
        "password": args.password,
        "count": args.count,
        "batch": args.batch,
        "pipeline": args.pipeline,
        "pipeline_batch": args.pipeline_batch,
        "stress_bin": args.stress_bin,
        "threads": args.threads,
        "conn_limit": args.conn_limit,
        "skip_single": skip_single,
        "skip_conn_limit": skip_conn_limit,
        "skip_multi_thread": skip_multi_thread,
    }

    # 显示将要运行的测试
    tests_to_run = []
    if not skip_single:
        tests_to_run.append("单连接多数据结构测试")
    if not skip_conn_limit and args.conn_limit is not None and args.conn_limit > 0:
        tests_to_run.append(f"连接数上限测试（目标：{args.conn_limit}）")
    if not skip_multi_thread and args.threads is not None and args.threads > 0:
        tests_to_run.append(f"多线程吞吐量测试（线程数：{args.threads}）")

    if not tests_to_run:
        print("错误：没有选择任何测试运行。请使用 --only-* 或相应的 --conn-limit/--threads 参数来选择测试。")
        return

    print(">>> 收集系统信息完成")
    print(">>> 准备运行 stress_test:", args.stress_bin)
    print(">>> 将要运行的测试:", "、".join(tests_to_run))

    stress_result = run_stress_test(
        stress_bin=args.stress_bin,
        host=args.host,
        port=args.port,
        password=args.password,
        count=args.count,
        batch=args.batch,
        pipeline=args.pipeline,
        pipeline_batch=args.pipeline_batch,
        threads=args.threads,
        conn_limit=args.conn_limit,
        skip_single=skip_single,
        skip_conn_limit=skip_conn_limit,
        skip_multi_thread=skip_multi_thread,
    )

    if stress_result["returncode"] != 0:
        print(
            f"警告：stress_test 返回码 {stress_result['returncode']}，"
            f"请检查服务器是否正常及参数是否正确。"
        )

    report_md = format_markdown_report(system_info, test_config, stress_result)

    if args.output:
        output_path = args.output
    else:
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        output_path = f"tinykv_stress_report_{ts}.md"

    with open(output_path, "w", encoding="utf-8") as f:
        f.write(report_md)

    print(f">>> 压测完成，报告已写入：{output_path}")


if __name__ == "__main__":
    main()

