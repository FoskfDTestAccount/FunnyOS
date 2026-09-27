#!/bin/bash
#
# Line ending diagnostics and repair.
#
# Background: this project is edited on Windows and built in WSL. A
# Windows editor may write CRLF, and CRLF inside a shell script turns
# "var=value" into a value with a trailing carriage return, which throws
# off path tests and command arguments. C sources and Makefiles are
# affected too, in subtler ways.
#
# Usage:
#   bash tools/fix-line-endings.sh          # diagnose only
#   bash tools/fix-line-endings.sh --fix    # diagnose and convert
#
set -u

PROJ="/mnt/c/FunnyOS"
cd "$PROJ"

DO_FIX=0
[ "${1:-}" = "--fix" ] && DO_FIX=1

# Text file patterns to inspect
PATTERNS="*.sh *.c *.h *.asm *.ld *.conf Makefile *.md"

count_crlf() {
    grep -c $'\r' "$1" 2>/dev/null || echo 0
}

FOUND=0
FIXED=0

echo "=== CRLF check (project root: $PROJ) ==="
echo

for pat in $PATTERNS; do
    # Recurse with find, skipping the download cache and reference sources
    while IFS= read -r f; do
        [ -f "$f" ] || continue
        n=$(count_crlf "$f")
        if [ "$n" -gt 0 ] 2>/dev/null; then
            FOUND=$((FOUND + 1))
            printf '  CRLF  %5s lines  %s\n' "$n" "$f"
            if [ "$DO_FIX" -eq 1 ]; then
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
    echo "  No CRLF files found."
elif [ "$DO_FIX" -eq 1 ]; then
    echo "  Found $FOUND file(s) containing CRLF; converted $FIXED to LF."
else
    echo "  Found $FOUND file(s) containing CRLF. Pass --fix to convert."
fi
