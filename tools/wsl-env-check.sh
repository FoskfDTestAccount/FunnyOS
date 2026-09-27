#!/bin/bash
# FunnyOS WSL 环境检查
echo "=== 身份 ==="
whoami
echo
echo "=== 发行版 ==="
grep PRETTY_NAME /etc/os-release
echo
echo "=== 内核 ==="
uname -r
echo
echo "=== 工具检查 ==="
for t in gcc cc ld nasm make qemu-system-x86_64 xorriso mcopy mtools git curl wget gdb objcopy python3; do
    p=$(command -v "$t" 2>/dev/null)
    if [ -n "$p" ]; then
        printf '%-24s OK      %s\n' "$t" "$p"
    else
        printf '%-24s MISSING\n' "$t"
    fi
done
echo
echo "=== 网络（apt 源可达性）==="
if timeout 10 curl -sI http://archive.ubuntu.com/ubuntu/ >/dev/null 2>&1; then
    echo "archive.ubuntu.com 可达"
else
    echo "archive.ubuntu.com 不可达"
fi
if timeout 10 curl -sI https://github.com >/dev/null 2>&1; then
    echo "github.com 可达"
else
    echo "github.com 不可达"
fi
echo
echo "=== KVM 嵌套虚拟化 ==="
if [ -e /dev/kvm ]; then
    echo "/dev/kvm 存在（QEMU 可硬件加速）"
else
    echo "/dev/kvm 不存在（QEMU 将使用 TCG 软件模拟，对本项目足够）"
fi
echo
echo "=== /mnt/c 可写性 ==="
if touch /mnt/c/FunnyOS/.wsl-write-test 2>/dev/null; then
    rm -f /mnt/c/FunnyOS/.wsl-write-test
    echo "/mnt/c/FunnyOS 可写"
else
    echo "/mnt/c/FunnyOS 不可写"
fi
