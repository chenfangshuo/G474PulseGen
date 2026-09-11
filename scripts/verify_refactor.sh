#!/bin/sh
# =============================================================================
# G474PulseGen —— 重构的静态验证
#
# 用途: 用固件产物证明"这次改动没有改变行为"。
#       改代码前存基线, 改完比对。零成本, 能挡住绝大多数低级错误。
#
# 用法:
#   scripts/verify_refactor.sh save      # 改动前: 采集基线
#   scripts/verify_refactor.sh check     # 改动后: 与基线比对
#
# 判据:
#   逐字节一致            -> 纯重构, 行为必然不变, 可直接提交
#   仅体积变化 + 符号一致 -> 实现改了但接口没动, 需人工确认是否符合预期
#   符号集合有增删        -> 接口变了, 必须人工审查
#
# 为什么比"编译通过"强:
#   编译通过只说明语法与类型正确, 完全不保证行为不变 —— 例如把 GPIOA 写成
#   GPIOB 一样能编译过。产物不变才是行为不变的证据。
#
# 先决条件 (重要):
#   构建必须可复现。本工程已满足 (未使用 __DATE__/__TIME__, 同一源码两次
#   全新构建产物 md5 相同)。但若你改动了**优化等级或工具链版本**, 逐字节
#   比对即失效 —— 那时只能退回段大小 + 实机波形。
#
# 详见 CONTRIBUTING.md「改动后怎么验证」一节。
# =============================================================================

set -e

ELF="${ELF:-build/Debug/G474PulseGen.elf}"
DIR="${DIR:-.verify}"

die() { echo "错误: $*" >&2; exit 1; }

need_tools() {
    command -v arm-none-eabi-objcopy >/dev/null 2>&1 \
        || die "找不到 arm-none-eabi-objcopy, 请确认工具链在 PATH 中 (见 README §9.1)"
    command -v arm-none-eabi-nm >/dev/null 2>&1 \
        || die "找不到 arm-none-eabi-nm, 请确认工具链在 PATH 中 (见 README §9.1)"
}

# 归一化 LTO 内部符号名。
# 必须做, 否则 diff 全是噪音: LTO 会给每个编译单元加 <file>.c.<8位hash> 后缀,
# 并给私有化符号加 .lto_priv.N 后缀 —— 只要该单元内容变了, hash 就变。
nm_norm() {
    arm-none-eabi-nm --defined-only "$1" 2>/dev/null \
        | awk '{print $3, $2}' \
        | sed -E 's/\.lto_priv\.[0-9]+//; s/\.(c|h)\.[0-9a-f]{8}$/.\1/' \
        | sort
}

usage() {
    sed -n '3,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

case "${1:-}" in
save)
    need_tools
    [ -f "$ELF" ] || die "找不到 $ELF —— 请先编译: cmake --build build/Debug"
    mkdir -p "$DIR"
    arm-none-eabi-objcopy -O binary "$ELF" "$DIR/base.bin"
    arm-none-eabi-size "$ELF" > "$DIR/base.size"
    nm_norm "$ELF" > "$DIR/base.nm"
    echo "✓ 基线已保存到 $DIR/"
    echo "  产物: $(wc -c < "$DIR/base.bin" | tr -d ' ') 字节"
    echo "  符号: $(wc -l < "$DIR/base.nm" | tr -d ' ') 个"
    echo ""
    echo "  现在可以改代码了。改完编译后运行: $0 check"
    ;;

check)
    need_tools
    [ -f "$DIR/base.bin" ] || die "没有基线。请先在**改动前**运行: $0 save"
    [ -f "$ELF" ] || die "找不到 $ELF —— 请先编译: cmake --build build/Debug"

    arm-none-eabi-objcopy -O binary "$ELF" "$DIR/new.bin"

    if cmp -s "$DIR/base.bin" "$DIR/new.bin"; then
        echo "✓ 逐字节一致 —— 纯重构, 行为必然不变。可直接提交。"
        echo "  ($(wc -c < "$DIR/new.bin" | tr -d ' ') 字节)"
        exit 0
    fi

    echo "△ 产物有差异 —— 需要人工判断"
    echo ""
    echo "  体积: $(wc -c < "$DIR/base.bin" | tr -d ' ') -> $(wc -c < "$DIR/new.bin" | tr -d ' ') 字节"
    echo ""
    echo "--- 段大小 (text/data/bss 各自的增减) ---"
    arm-none-eabi-size "$ELF" | sed 's/^/  /'
    echo ""
    echo "--- 符号集合差异 (已归一化 LTO 后缀) ---"
    nm_norm "$ELF" > "$DIR/new.nm"
    if diff -u "$DIR/base.nm" "$DIR/new.nm" | sed 's/^/  /'; then
        echo "  无差异 —— 接口未变, 属实现变更。"
        echo ""
        echo "  这与「纯重构」的预期不符时应做**隔离验证**: 把可疑的那一项改动"
        echo "  还原后重编, 看差异是否消失, 从而定位是哪一项引入的。"
    else
        echo ""
        echo "  ⚠ 符号有增删 —— 接口变了。请逐条确认是否都是本次改动的预期结果。"
    fi
    ;;

*)
    usage
    ;;
esac
