#!/bin/bash
# 连接数限制测试脚本
# 用于验证 Windows FD_SETSIZE 限制修复

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}=== EyaKV 连接数限制测试 ===${NC}"

# 默认参数
HOST="127.0.0.1"
PORT=5210
MAX_CONNECTIONS=1000
STEP=100

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--host)
            HOST="$2"
            shift 2
            ;;
        -p|--port)
            PORT="$2"
            shift 2
            ;;
        -n|--max-connections)
            MAX_CONNECTIONS="$2"
            shift 2
            ;;
        -s|--step)
            STEP="$2"
            shift 2
            ;;
        --help)
            echo "用法: $0 [选项]"
            echo "选项:"
            echo "  -h, --host HOST          服务器地址 (默认: 127.0.0.1)"
            echo "  -p, --port PORT          服务器端口 (默认: 5210)"
            echo "  -n, --max-connections NUM  最大连接数 (默认: 1000)"
            echo "  -s, --step NUM           每步增加的连接数 (默认: 100)"
            echo "  --help                   显示此帮助信息"
            exit 0
            ;;
        *)
            echo "未知选项: $1"
            echo "使用 --help 查看帮助信息"
            exit 1
            ;;
    esac
done

echo -e "${YELLOW}配置:${NC}"
echo "  服务器: $HOST:$PORT"
echo "  最大连接数: $MAX_CONNECTIONS"
echo "  每步增加: $STEP"

# 检查 Python 是否可用
if ! command -v python3 &> /dev/null; then
    echo -e "${RED}错误: 未找到 python3${NC}"
    exit 1
fi

# 检查 stress_test 是否存在
STRESS_BIN="build/bin/stress_test.exe"
if [ "$(uname)" = "Linux" ] || [ "$(uname)" = "Darwin" ]; then
    STRESS_BIN="build/bin/stress_test"
fi

if [ ! -f "$STRESS_BIN" ]; then
    echo -e "${RED}错误: 未找到 stress_test 可执行文件: $STRESS_BIN${NC}"
    echo "请先编译项目: cd build && cmake .. && cmake --build ."
    exit 1
fi

echo -e "${GREEN}找到 stress_test: $STRESS_BIN${NC}"
echo ""

# 开始测试
echo -e "${GREEN}开始测试...${NC}"
echo ""

STEP_COUNT=1
CURRENT_MAX=$STEP

while [ $CURRENT_MAX -le $MAX_CONNECTIONS ]; do
    echo -e "${YELLOW}[$STEP_COUNT] 测试最大连接数: $CURRENT_MAX${NC}"

    OUTPUT="conn_test_${CURRENT_MAX}.txt"

    python3 stress_runner.py \
        --stress-bin "$STRESS_BIN" \
        --host "$HOST" \
        --port "$PORT" \
        --only-conn-limit \
        --conn-limit "$CURRENT_MAX" \
        --output "conn_test_${CURRENT_MAX}.md" > "$OUTPUT" 2>&1

    if [ $? -eq 0 ]; then
        # 从输出中提取实际建立的连接数
        ESTABLISHED=$(grep "established" "$OUTPUT" | awk '{print $NF}')
        echo -e "${GREEN}  ✓ 成功: 建立了 $ESTABLISHED / $CURRENT_MAX 个连接${NC}"

        if [ "$ESTABLISHED" -lt "$CURRENT_MAX" ]; then
            if [ "$ESTABLISHED" -lt 64 ]; then
                echo -e "${RED}  ✗ 警告: 连接数低于 64，可能仍有 FD_SETSIZE 限制问题${NC}"
            elif [ "$ESTABLISHED" -lt $((FD_SETSIZE - 10)) ]; then
                echo -e "${YELLOW}  ⚠ 接近 FD_SETSIZE 限制 (FD_SETSIZE=1024)${NC}"
            fi
        fi
    else
        echo -e "${RED}  ✗ 失败: 测试返回错误${NC}"
        cat "$OUTPUT"
    fi

    STEP_COUNT=$((STEP_COUNT + 1))
    CURRENT_MAX=$((CURRENT_MAX + STEP))
done

echo ""
echo -e "${GREEN}=== 测试完成 ===${NC}"
echo ""
echo "报告文件:"
ls -1 conn_test_*.md 2>/dev/null | while read file; do
    echo "  - $file"
done

echo ""
echo -e "${GREEN}下一步:${NC}"
echo "1. 检查服务器日志中的警告信息"
echo "2. 查看 conn_test_*.md 报告文件"
echo "3. 如果连接数仍然受限，考虑进一步优化 I/O 模型"
