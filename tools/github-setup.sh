#!/bin/bash
#
# FunnyOS —— GitHub 仓库初始化
#
# 前置条件：gh 已安装且已认证。
#   若未认证：gh auth login --hostname github.com --git-protocol https --web
#
# 本脚本做四件事：
#   1. 从 GitHub API 读取你的账号身份，据此设置本仓库的 git 提交身份
#      （用 GitHub 提供的 noreply 邮箱，避免把真实邮箱写进提交历史）
#   2. 提交 M0 的成果
#   3. 在你的 GitHub 账号下创建仓库
#   4. 推送
#
# 幂等：仓库已存在时会直接使用，不会报错。
#
set -e

GH="/c/Program Files/GitHub CLI/gh.exe"
[ -x "$GH" ] || GH="$(command -v gh || true)"
if [ ! -x "$GH" ]; then
    echo "错误：找不到 gh。请先安装：winget install --id GitHub.cli" >&2
    exit 1
fi

PROJ="/c/FunnyOS"
cd "$PROJ"

REPO_NAME="FunnyOS"
# 可见性：private 是安全默认值——私有转公开只需一次点击，
# 而公开转私有无法撤回已被索引的内容。要改成 public 就改这里。
VISIBILITY="private"
DESCRIPTION="原生 x86-64 操作系统，内建 8086 虚拟机，用于原生执行 DOS 程序"

# ---------------------------------------------------------------- 1. 认证
if ! "$GH" auth status >/dev/null 2>&1; then
    echo "错误：gh 尚未认证。" >&2
    echo "请先运行：" >&2
    echo "  gh auth login --hostname github.com --git-protocol https --web" >&2
    exit 1
fi

echo "=== 读取 GitHub 账号身份 ==="
LOGIN=$("$GH" api user --jq .login)
USER_ID=$("$GH" api user --jq .id)
echo "  账号：$LOGIN (id=$USER_ID)"

# GitHub 的 noreply 邮箱格式：<用户ID>+<用户名>@users.noreply.github.com
# 用它提交可以隐藏真实邮箱，同时让提交正确关联到账号。
NOREPLY_EMAIL="${USER_ID}+${LOGIN}@users.noreply.github.com"

echo "=== 设置本仓库的提交身份 ==="
git config user.name  "$LOGIN"
git config user.email "$NOREPLY_EMAIL"
echo "  user.name  = $LOGIN"
echo "  user.email = $NOREPLY_EMAIL"

# ---------------------------------------------------------------- 2. 提交
echo
echo "=== 暂存并提交 ==="
git add -A

if git diff --cached --quiet; then
    echo "  没有需要提交的改动。"
else
    git commit -F - <<'COMMIT_MSG'
M0: 引导闭环打通（Limine + x86-64 长模式内核）

内核经 Limine 引导进入 64 位长模式，串口输出与引导协议解析全部正常，
BIOS 与 UEFI 两条固件路径均有自动化断言覆盖并通过。

主要构成：
- kernel/arch/x86_64/entry.asm  长模式入口，建立 64 KiB 内核栈
- kernel/boot/bootinfo.c        封装 Limine 引导请求，隔离引导器依赖
- kernel/console/serial.c       16550 UART 驱动，含回环自检
- kernel/console/kprintf.c      输出后端抽象，便于后续接入帧缓冲
- kernel/panic.c                致命错误处理
- libk/                         裸机所需基础库（memcpy/memset/printf）
- linker.ld                     高半区内核链接脚本
- Makefile                      构建产物输出到 WSL 原生目录
- tools/                        环境自检、Limine 获取、启动测试

设计要点（完整论证见 docs/DESIGN.md）：
DOS 程序将由纯软件 8086 解释器执行，运行在 Ring 3 用户态。原因是硬件
层面的：x64 长模式不提供 virtual 8086 模式，而硬件虚拟化需 Westmere
之后的 unrestricted guest + EPT 才能进入实模式 guest，复杂度不成比例。

构建产物刻意不放在 /mnt/c：WSL 在 /mnt/c 上写入的文件若遇 WSL 提前终止，
会留下 Win32 可见但无法 stat 也无法删除的幽灵文件。详见 README。

Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>
COMMIT_MSG
    echo "  已提交：$(git rev-parse --short HEAD)"
fi

# ---------------------------------------------------------------- 3. 创建仓库
echo
echo "=== 创建 GitHub 仓库 ==="
if "$GH" repo view "$LOGIN/$REPO_NAME" >/dev/null 2>&1; then
    echo "  仓库已存在：$LOGIN/$REPO_NAME，直接使用。"
else
    "$GH" repo create "$REPO_NAME" \
        --"$VISIBILITY" \
        --description "$DESCRIPTION" \
        --source=. \
        --remote=origin
    echo "  已创建：$LOGIN/$REPO_NAME（$VISIBILITY）"
fi

# 确保 origin 指向正确，处理已存在仓库但本地没有 remote 的情况
if ! git remote get-url origin >/dev/null 2>&1; then
    git remote add origin "https://github.com/$LOGIN/$REPO_NAME.git"
    echo "  已添加 origin"
fi

# ---------------------------------------------------------------- 4. 推送
echo
echo "=== 推送 ==="
git push -u origin main

echo
echo "完成：https://github.com/$LOGIN/$REPO_NAME"
