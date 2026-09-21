#!/usr/bin/env bash
# 清理构建中间产物（BoTapMusic）。
#
# 只删除被 .gitignore 忽略、且可以重新生成的编译缓存：目标文件、链接中间文件、
# 预编译头、调试符号、MSBuild 跟踪文件。不会碰：
#   - 版本库里的任何文件（脚本会先检查，发现构建产物进了版本库就直接退出）
#   - Release/ 运行目录里的 exe、dll、皮肤、语言文件和配置
#   - x64/Release/research/ 下的逆向素材
#   - .apk、apk_out/、.upstream-archive/、.tools/ 这些离线参考
#
# 用法：
#   scripts/clean-build.sh            # 列出将被删除的文件并统计，然后删除
#   scripts/clean-build.sh --dry-run  # 只看统计，不删
#
# 删掉之后下次编译会全量重建，这是预期行为。
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$root"

dry_run=0
if [ "${1:-}" = "--dry-run" ]; then
    dry_run=1
elif [ $# -gt 0 ]; then
    echo "用法: scripts/clean-build.sh [--dry-run]" >&2
    exit 2
fi

# 构建产物不应该进版本库：真进去了就别自动删，先让人处理
if git ls-files | grep -qiE '\.(obj|iobj|ipdb|pdb|pch|tlog|ilk|idb|bsc|lastbuildstate|res|tlh|tli)$'; then
    echo "版本库里存在构建产物，请先处理这些文件再清理。" >&2
    exit 1
fi

mapfile -d '' -t candidates < <(
    find . -name .git -prune -o -type f \( \
        -name '*.obj' -o -name '*.iobj' -o -name '*.ipdb' -o -name '*.pdb' -o \
        -name '*.pch' -o -name '*.tlog' -o -name '*.ilk' -o -name '*.idb' -o \
        -name '*.bsc' -o -name '*.lastbuildstate' -o -name '*.res' -o \
        -name '*.tlh' -o -name '*.tli' \) -print0
)

count=0
total=0
kept=0
for file in "${candidates[@]}"; do
    # 逆向素材目录里可能有同名后缀的研究文件，按路径排除
    case "$file" in
        */research/*) continue ;;
        # 运行目录里紧挨着 exe 的符号文件留着：排查崩溃时还要用
        ./Release/*.pdb|./x64/Release/*.pdb) kept=$((kept + 1)); continue ;;
    esac
    size=$(stat -c %s -- "$file")
    count=$((count + 1))
    total=$((total + size))
    if [ "$dry_run" -eq 0 ]; then
        rm -f -- "$file"
    fi
done

printf '%s: %d 个文件，%.1f MB（保留运行目录下的 %d 个符号文件）\n' \
    "$([ "$dry_run" -eq 1 ] && echo '可清理' || echo '已清理')" \
    "$count" \
    "$(awk -v bytes="$total" 'BEGIN { printf "%.1f", bytes / 1048576 }')" \
    "$kept"
