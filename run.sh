#!/usr/bin/env bash
# ============================================================
# HynOS 启动脚本：用 QEMU 引导 build/hynos.img 并抓取自检日志
#
#   ./run.sh                           BIOS/MBR 路径（默认）
#   ./run.sh uefi                      UEFI/GPT+ESP 路径（需要 OVMF 固件）
#   ./run.sh uefi <OVMF_CODE.fd> <OVMF_VARS.fd>   显式指定固件
#
# 两条路径引导的是同一个 build/hynos.img（GPT 与 MBR 共存，内核字节同一份）。
#
# 取证来源：
#   串口输出      -> build/qemu-serial-<mode>.log   （内核自检的权威日志）
#   调试口 0xE9   -> build/qemu-debug-<mode>.log
#   退出方式      : 内核自检结束写 isa-debug-exit(0xF4)，QEMU 随即退出
# ============================================================
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE="${1:-bios}"
OVMF_CODE_ARG="${2:-}"
OVMF_VARS_ARG="${3:-}"
IMG="$ROOT/build/hynos.img"
SERIAL_LOG="$ROOT/build/qemu-serial-$MODE.log"
DEBUG_LOG="$ROOT/build/qemu-debug-$MODE.log"
# 传给 QEMU 的一律用相对路径，避免 MSYS 路径在 "file:/c/..." 形态下不被转换
IMG_REL="build/hynos.img"
SERIAL_LOG_REL="build/qemu-serial-$MODE.log"
DEBUG_LOG_REL="build/qemu-debug-$MODE.log"

abs_to_win() {
    if command -v cygpath >/dev/null 2>&1; then cygpath -w "$1"; else printf '%s' "$1"; fi
}
TIMEOUT_SEC="${QEMU_TIMEOUT:-40}"

find_qemu() {
    if [ -n "${QEMU:-}" ] && [ -x "${QEMU}" ]; then echo "$QEMU"; return 0; fi
    for cand in \
        "$ROOT/../tools/qemu-msys2/mingw64/bin/qemu-system-x86_64.exe" \
        "$ROOT/../tools/qemu/qemu-system-x86_64.exe" \
        "$ROOT/../tools/qemu/qemu-system-x86_64" \
        "/c/Program Files/qemu/qemu-system-x86_64.exe" \
        "/c/QEMU/qemu-system-x86_64.exe"
    do
        [ -x "$cand" ] && { echo "$cand"; return 0; }
    done
    if command -v qemu-system-x86_64.exe >/dev/null 2>&1; then
        command -v qemu-system-x86_64.exe; return 0
    fi
    if command -v qemu-system-x86_64 >/dev/null 2>&1; then
        command -v qemu-system-x86_64; return 0
    fi
    return 1
}

# 找 OVMF：优先命令行与环境变量，其次工程 firmware/ 与 tools/ovmf/（含 .deb 解包结果）
find_ovmf() {
    if [ -n "$OVMF_CODE_ARG" ] && [ -n "$OVMF_VARS_ARG" ]; then
        echo "$OVMF_CODE_ARG|$OVMF_VARS_ARG"; return 0
    fi
    if [ -n "${OVMF_CODE:-}" ] && [ -n "${OVMF_VARS:-}" ]; then
        echo "$OVMF_CODE|$OVMF_VARS"; return 0
    fi
    local code="" vars="" dir c v
    for dir in "$ROOT/firmware" "$ROOT/../tools/ovmf" "$ROOT/../tools/ovmf/usr/share/OVMF" \
               "$ROOT/../tools/ovmf-bin/usr/share/edk2/x64" \
               "$ROOT/../tools/qemu-msys2/mingw64/share/qemu" \
               "/c/Program Files/qemu/share" "/c/Program Files (x86)/qemu/share"; do
        [ -d "$dir" ] || continue
        for c in OVMF_CODE.fd OVMF_CODE.4m.fd OVMF_CODE_4M.fd OVMF_CODE.secboot.fd \
                 OVMF_CODE.secboot.4m.fd code.fd edk2-x86_64-code.fd edk2-i386-code.fd; do
            [ -z "$code" ] && [ -f "$dir/$c" ] && code="$dir/$c"
        done
        for v in OVMF_VARS.fd OVMF_VARS.4m.fd OVMF_VARS_4M.fd vars.fd \
                 edk2-x86_64-vars.fd edk2-i386-vars.fd; do
            [ -z "$vars" ] && [ -f "$dir/$v" ] && vars="$dir/$v"
        done
        [ -n "$code" ] && [ -n "$vars" ] && break
    done
    [ -n "$code" ] && [ -n "$vars" ] && { echo "$code|$vars"; return 0; }
    return 1
}

if [ ! -f "$IMG" ]; then
    echo "[run] 找不到 $IMG，请先执行 ./build.sh" >&2
    exit 1
fi

QEMU_BIN="$(find_qemu)" || {
    echo "[run] 未找到 qemu-system-x86_64；可用环境变量 QEMU=/path/to/qemu-system-x86_64.exe 指定" >&2
    echo "[run] 取固件与 QEMU 的说明见 README.md『双路径启动验证』一节" >&2
    exit 2
}
echo "[run] 引导路径: $MODE     QEMU: $QEMU_BIN"
rm -f "$SERIAL_LOG" "$DEBUG_LOG"

# 真实外设：HAL 与三大故障域"探测真实硬件"这件事必须有真设备可探，
# 否则任何"设备已就绪"的日志都只是空话。
#   e1000 网卡   -> 网络组（PCI 类别 0x02）
#   NVMe 控制器  -> 存储组（PCI 类别 0x01/0x08，需要一个空块设备后端）
#   xHCI / HDA   -> HAL 外设骨架（PCI 类别 0x0C/0x03 与 0x04/0x03）
NVME_IMG="$ROOT/build/nvme0.img"
if [ ! -f "$NVME_IMG" ]; then
    # 先 dd（coreutils 自带），失败再用 python；两条都失败就如实报"未生成"。
    dd if=/dev/zero of="$NVME_IMG" bs=1M count=8 status=none 2>/dev/null || true
    if [ ! -f "$NVME_IMG" ]; then
        python -c "f=open(r'''$NVME_IMG''','wb'); f.write(b'\0'*(8*1024*1024)); f.close()" 2>/dev/null || true
    fi
fi

PERIPH_ARGS=(
    -netdev "user,id=net0"
    -device "e1000,netdev=net0"
    -device "qemu-xhci"
    -device "intel-hda"
    -device "hda-duplex"
)
if [ -f "$NVME_IMG" ]; then
    PERIPH_ARGS+=(
        -drive "file=$NVME_IMG,format=raw,if=none,id=nvme0"
        -device "nvme,drive=nvme0,serial=HYNVM0001"
    )
else
    echo '[run] 警告：未生成 build/nvme0.img，本轮不带 NVMe 控制器（存储组会如实报：未探测到）' >&2
fi

ARGS=(
    -drive "file=$IMG_REL,format=raw,if=ide,index=0"
    -m 256M
    -display none
    -no-reboot
    -device isa-debug-exit,iobase=0xf4,iosize=0x04
    -serial "file:$SERIAL_LOG_REL"
    -debugcon "file:$DEBUG_LOG_REL"
    -global isa-debugcon.iobase=0xe9
    "${PERIPH_ARGS[@]}"
)

case "$MODE" in
    bios|mbr)
        ;;
    uefi)
        OVMF="$(find_ovmf)" || {
            echo "[run] 未找到 OVMF 固件（OVMF_CODE.fd + OVMF_VARS.fd）。" >&2
            echo "[run] 可放到 hynos-kernel/firmware/ 或用 ./run.sh uefi <CODE.fd> <VARS.fd> 指定。" >&2
            exit 3
        }
        CODE="${OVMF%%|*}"
        VARS_SRC="${OVMF##*|}"
        # VARS 必须可写：拷一份到 build/ 下使用，保持原始固件不被改动
        VARS_RW="build/OVMF_VARS-$MODE.fd"
        cp "$VARS_SRC" "$ROOT/$VARS_RW"
        CODE_W="$(abs_to_win "$CODE")"
        echo "[run] OVMF: $CODE + $ROOT/$VARS_RW"
        ARGS=(
            -drive "if=pflash,format=raw,unit=0,readonly=on,file=$CODE_W"
            -drive "if=pflash,format=raw,unit=1,file=$VARS_RW"
            "${ARGS[@]}"
        )
        ;;
    *)
        echo "[run] 未知路径 '$MODE'，只支持 bios / uefi" >&2
        exit 1
        ;;
esac

cd "$ROOT" || exit 1
set +e
if command -v timeout >/dev/null 2>&1; then
    timeout "$TIMEOUT_SEC" "$QEMU_BIN" "${ARGS[@]}"
else
    "$QEMU_BIN" "${ARGS[@]}"
fi
rc=$?
set -e

echo "[run] QEMU 退出码: $rc"
echo "[run] ==== 串口日志 ($SERIAL_LOG) ===="
cat "$SERIAL_LOG" 2>/dev/null || echo "(无串口输出)"