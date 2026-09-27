#!/bin/bash
#
# 行尾符诊断与修复
#
# 背景：本项目在 Windows 上编辑、在 WSL 里构建。Windows 侧的编辑器
# 可能写入 CRLF，而 shell 脚本里的 CRLF 会让 "var=value\r" 带上回车，
# 导致路径判断、命令参数全部错位。C 源码和 Makefile 同样受影响。
#
# 用法：
#   bash tools/fix-line-endings.sh          # 只诊断
#   bash tools/fix-line-endings.sh --fix    # 诊断并转换
#
set -u

PROJ="/mnt/c/FunnyOS"
cd "$PROJ"

DO_FIX=0
[ "${1:-}" = "--fix" ] && DO_FIX=1

# 需要检查的文本文件类型
PATTERNS="*.sh *.c *.h *.asm *.ld *.conf Makefile *.md"

count_crlf() {
    # 统计含 CR 的行数
    grep -c $'\r' "$1" 2>/dev/null || echo 0
}

FOUND=0
FIXED=0

echo "=== CRLF 检查（项目根：$PROJ）==="
echo

for pat in $PATTERNS; do
    # 用 find 递归，排除下载缓存与参考源码
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        n=$(count_crlf "$f")
        if [ "$n" -gt 0 ] 2>/dev/null; then
            FOUND=$((FOUND + 1))
            printf '  CRLF  %5s 行  %s\n' "$n" "$f"
            if [ "$DO_FIX" -eq 1 ]; then
                # 去掉行尾 CR
                sed -i 's/\r$//' "$f"
                FIXED=$((FIXED + 1))
            fi
        fi
    done < <(find . -name "$pat" \
                  -not -path './.cache/*' \
                  -not -path './reference-msdos/*' \
                  -not -path './build/*' \
                  -not -path './.git/*' 2>/dev/null)
done

echo
if [ "$FOUND" -eq 0 ]; then
    echo "  未发现 CRLF 文件。"
elif [ "$DO_FIX" -eq 1 ]; then
    echo "  发现 $FOUND 个文件含 CRLF，已转换 $FIXED 个为 LF。"
else
    echo "  发现 $FOUND 个文件含 CRLF。加 --fix 参数可自动转换。"
fi
