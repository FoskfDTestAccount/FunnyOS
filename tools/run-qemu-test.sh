#!/bin/bash
#
# FunnyOS 启动测试
#
# 无头启动 QEMU，把串口输出重定向到文件，然后对输出做断言。
# 这是整个项目的自动化回归测试基座——后续每个里程碑都在这里
# 增加新的断言。
#
# 用法：bash tools/run-qemu-test.sh [ISO 路径] [bios|uefi]
#
# 两种引导路径都要测，因为它们走的是完全不同的固件栈：
#   BIOS 路径：Limine 的 BIOS 阶段（stage1/stage2）经 El Torito 加载
#   UEFI 路径：Limine 的 EFI 可执行体经 El Torito 加载
# 现代机器基本都是纯 UEFI，所以 UEFI 路径才是最终交付要用的那条。
#
set -u

BUILD_DIR="${FUNYOS_BUILD_DIR:-/var/tmp/funyos-build}"
ISO="${1:-$BUILD_DIR/funyos.iso}"
MODE="${2:-bios}"
TIMEOUT_SECS="${QEMU_TIMEOUT:-20}"

if [ ! -f "$ISO" ]; then
    echo "错误：找不到 ISO：$ISO" >&2
    echo "      请先运行 make" >&2
    exit 1
fi

if [ "$MODE" != "bios" ] && [ "$MODE" != "uefi" ]; then
    echo "错误：引导模式必须是 bios 或 uefi，收到：$MODE" >&2
    exit 1
fi

LOG="$BUILD_DIR/serial-$MODE.log"
mkdir -p "$BUILD_DIR"
rm -f "$LOG"

# ---------------------------------------------------------------- 加速
if [ -e /dev/kvm ] && [ -w /dev/kvm ]; then
    ACCEL="-enable-kvm -cpu host"
    echo "  加速      ：KVM（硬件）"
else
    ACCEL="-cpu max"
    echo "  加速      ：TCG（软件模拟，较慢）"
fi

# ---------------------------------------------------------------- 固件
if [ "$MODE" = "uefi" ]; then
    OVMF_CODE="/usr/share/OVMF/OVMF_CODE_4M.fd"
    OVMF_VARS_SRC="/usr/share/OVMF/OVMF_VARS_4M.fd"
    OVMF_VARS="$BUILD_DIR/OVMF_VARS_4M.fd"

    if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS_SRC" ]; then
        echo "错误：未找到 OVMF 固件（$OVMF_CODE）" >&2
        echo "      请安装：apt-get install ovmf" >&2
        exit 1
    fi

    # UEFI 变量存储必须是可写的，而 /usr/share 下的原文件不可写，
    # 所以每次测试都从模板复制一份。
    cp "$OVMF_VARS_SRC" "$OVMF_VARS"

    FIRMWARE_ARGS="-M q35 \
        -drive if=pflash,format=raw,unit=0,readonly=on,file=$OVMF_CODE \
        -drive if=pflash,format=raw,unit=1,file=$OVMF_VARS"
    BOOT_ARGS=""
    echo "  固件      ：OVMF (UEFI)"
else
    FIRMWARE_ARGS=""
    # 传统 BIOS 下显式指定从光驱启动
    BOOT_ARGS="-boot d"
    echo "  固件      ：SeaBIOS (传统 BIOS)"
fi

echo "  引导模式  ：$MODE"
echo "  ISO       ：$ISO"
echo "  串口日志  ：$LOG"
echo "  超时上限  ：${TIMEOUT_SECS} 秒"
echo

# shellcheck disable=SC2086
timeout "$TIMEOUT_SECS" qemu-system-x86_64 \
    -m 512 \
    -cdrom "$ISO" \
    $BOOT_ARGS \
    -serial "file:$LOG" \
    -display none \
    -no-reboot \
    $FIRMWARE_ARGS \
    $ACCEL \
    >/dev/null 2>&1

QEMU_STATUS=$?
# 124 = timeout 命令杀掉了 QEMU。内核停机后本就该一直运行，
# 所以超时是预期结果，不是错误。
if [ "$QEMU_STATUS" -eq 124 ]; then
    echo "  QEMU 因超时被终止（预期：内核停机后不会自行退出）"
else
    echo "  QEMU 自行退出，状态码 $QEMU_STATUS（可能发生了三重故障）"
fi

echo
echo "=================== 串口输出 ==================="
if [ -s "$LOG" ]; then
    # UEFI 路径下 OVMF 会把它自己的控制台输出（含大量 ANSI 光标定位序列）
    # 一并写到串口。这里仅为显示整洁而剥离，断言仍然针对原始日志文件。
    sed -e 's/\x1b\[[0-9;]*[a-zA-Z]//g' "$LOG" | cat -s
else
    echo "(串口无任何输出)"
fi
echo "================================================"
echo

# ------------------------------------------------------------------ 断言
FAILED=0

expect_present() {
    if grep -q "$1" "$LOG" 2>/dev/null; then
        printf '  [通过] %s\n' "$2"
    else
        printf '  [失败] %s\n' "$2"
        FAILED=1
    fi
}

expect_absent() {
    if grep -q "$1" "$LOG" 2>/dev/null; then
        printf '  [失败] %s\n' "$2"
        FAILED=1
    else
        printf '  [通过] %s\n' "$2"
    fi
}

if [ "$MODE" = "uefi" ]; then
    FW_EXPECT="UEFI 64-bit"
else
    FW_EXPECT="x86 BIOS (传统)"
fi

echo "断言结果（$MODE 路径）："
expect_present "M0 引导闭环验证"                 "内核入口已执行，串口输出可用"
expect_present "引导器        : Limine"          "引导器信息请求已解析"
expect_present "$FW_EXPECT"                      "固件类型确认为 $MODE"
expect_present "协议基版本    : 3 (已确认支持)"  "引导协议基版本检查通过"
expect_present "HHDM 偏移"                       "HHDM 请求已解析"
expect_present "可用内存"                        "内存映射请求已解析并汇总"
expect_present "帧缓冲"                          "帧缓冲请求已解析"
expect_present "M0 引导闭环验证通过"             "完成标记出现"
expect_absent  "PANIC"                           "未发生内核 panic"

echo
if [ "$FAILED" -eq 0 ]; then
    echo "====> M0 测试通过（$MODE）"
    exit 0
else
    echo "====> M0 测试失败（$MODE）"
    exit 1
fi
