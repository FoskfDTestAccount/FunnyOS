#!/bin/bash
# FunnyOS WSL environment check.
echo "=== Identity ==="
whoami
echo
echo "=== Distribution ==="
grep PRETTY_NAME /etc/os-release
echo
echo "=== Kernel ==="
uname -r
echo
echo "=== Toolchain ==="
for t in gcc cc ld nasm make qemu-system-x86_64 xorriso mcopy mtools git curl wget gdb objcopy python3; do
    p=$(command -v "$t" 2>/dev/null)
    if [ -n "$p" ]; then
        printf '%-24s OK      %s\n' "$t" "$p"
    else
        printf '%-24s MISSING\n' "$t"
    fi
done
echo
echo "=== UEFI firmware (needed by make test-uefi) ==="
if [ -f /usr/share/OVMF/OVMF_CODE_4M.fd ]; then
    echo "OVMF present"
else
    echo "OVMF missing (apt-get install ovmf)"
fi
echo
echo "=== Network ==="
if timeout 10 curl -sI http://archive.ubuntu.com/ubuntu/ >/dev/null 2>&1; then
    echo "archive.ubuntu.com reachable"
else
    echo "archive.ubuntu.com unreachable"
fi
if timeout 10 curl -sI https://github.com >/dev/null 2>&1; then
    echo "github.com reachable"
else
    echo "github.com unreachable"
fi
echo
echo "=== KVM nested virtualisation ==="
if [ -e /dev/kvm ]; then
    echo "/dev/kvm present (QEMU can use hardware acceleration)"
else
    echo "/dev/kvm absent (QEMU falls back to TCG; fine for this project)"
fi
echo
echo "=== Project directory writability ==="
if touch /mnt/c/FunnyOS/.wsl-write-test 2>/dev/null; then
    rm -f /mnt/c/FunnyOS/.wsl-write-test
    echo "/mnt/c/FunnyOS writable"
    echo "Note: build artifacts still belong in WSL-native storage. See README."
else
    echo "/mnt/c/FunnyOS NOT writable"
fi
