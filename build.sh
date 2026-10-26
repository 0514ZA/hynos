#!/usr/bin/env bash
# ============================================================
# 混元 OS 构建脚本（x86_64 裸机内核）
#
# 工具链：ziglang 内置 clang/lld/objcopy（本机无系统 gcc/clang/make）
#   编译 : python -m ziglang cc -target x86_64-freestanding
#   链接 : 同上，使用自研链接脚本
#   转盘 : python -m ziglang objcopy --output-target=binary
# 产物：build/hunyuan.img（BIOS 可直接引导的磁盘镜像）
# ============================================================
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$ROOT/build"
OBJDIR="$BUILD/obj"

ZIGCC="${ZIGCC:-python -m ziglang cc}"
ZIGOBJCOPY="${ZIGOBJCOPY:-python -m ziglang objcopy}"

CFLAGS="-target x86_64-freestanding -ffreestanding -nostdlib -fno-stack-protector \
-fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mno-sse -mno-sse2 \
-mcmodel=kernel -O2 -Wall -Wextra -Wno-unused-parameter -I$BUILD/include -I$ROOT/include"

mkdir -p "$OBJDIR"

# 生成构建时间戳头（避免使用 __DATE__/__TIME__ 破坏可复现构建）
STAMP="${HYN_BUILD_STAMP:-$(date '+%Y-%m-%d %H:%M:%S')}"
mkdir -p "$BUILD/include/hyn"
printf '#ifndef HYN_BUILDSTAMP_H\n#define HYN_BUILDSTAMP_H\n#define HYN_BUILD_STAMP "%s"\n#endif\n' "$STAMP" \
    > "$BUILD/include/hyn/buildstamp.h"

# ============================================================
# Ring3 用户程序（libhyn + 示例）
# 必须排在内核编译之前：内核要把这两个平铺镜像作为 const 数组链进自己（userimg.h）。
# 用户态编译参数与内核刻意不同：
#   · 不加 -mcmodel=kernel（用户代码跑在低地址用户窗口，不是内核高半区）；
#   · 加 -fPIC -fno-plt：代码必须位置无关（镜像会被拷到动态分配的用户页上执行）；
#   · 加 -ffreestanding -fno-builtin -nostdlib：不依赖任何 libc，也不让编译器偷偷生成 memcpy 调用。
# 产物是平铺 .bin，其位置无关性由 tools/mkuserimg.py 在链接后用 ELF 段表硬校验（不是"跑起来没崩"）。
# ============================================================
USERCFLAGS="-target x86_64-freestanding -ffreestanding -nostdlib -fno-stack-protector \
-fno-asynchronous-unwind-tables -fno-unwind-tables -mno-red-zone -mno-sse -mno-sse2 \
-fno-builtin -fPIC -fno-plt -ffunction-sections -fdata-sections \
-O2 -Wall -Wextra -Wno-unused-parameter -I$ROOT/include -I$ROOT/user/include"

echo "[build] 编译 Ring3 用户态库（libhyn）"
ULIB_OBJS=()
i=0
for usrc in "$ROOT/user/lib/libhyn.c" "$ROOT/user/lib/mem.c" "$ROOT/user/lib/entry.S"; do
    uobj="$OBJDIR/user_lib_$i.o"
    $ZIGCC $USERCFLAGS -c "$usrc" -o "$uobj"
    ULIB_OBJS+=("$uobj")
    i=$((i + 1))
done

echo "[build] 编译并链接 Ring3 示例程序（Shell / Agent 骨架）"
for prog in shell agent; do
    $ZIGCC $USERCFLAGS -c "$ROOT/user/examples/$prog.c" -o "$OBJDIR/user_$prog.o"
    $ZIGCC $USERCFLAGS -Wl,--build-id=none -Wl,-T,"$ROOT/user/user.ld" -Wl,-e,_start \
        -Wl,--gc-sections -o "$BUILD/$prog.elf" "$OBJDIR/user_$prog.o" "${ULIB_OBJS[@]}"
done

# 平铺镜像不用 objcopy：本机 ziglang 自带的 objcopy 在这类"基址 0 + 多程序头"的 ELF 上
# 会静默产出 0 字节（不报错也不输出）。改为由 mkuserimg.py 按 ELF 段表自建平铺镜像，
# 顺带把位置无关性（无重定位表 / 无非空 GOT-PLT / 入口在首段内 / 段首尾相接）一并硬校验。
echo "[build] 校验用户程序位置无关性，并生成内核用镜像头"
python "$ROOT/tools/mkuserimg.py" "$BUILD/include/hyn/userimg.h" \
    g_user_shell_img "$BUILD/shell.elf" \
    g_user_agent_img "$BUILD/agent.elf"

objname() {
    # 把源码路径映射成唯一的目标文件名
    local rel="${1#$ROOT/}"
    printf '%s/%s.o\n' "$OBJDIR" "${rel//\//_}"
}

OBJS=()
echo "[build] 编译 C 源码"
while IFS= read -r src; do
    obj="$(objname "$src")"
    $ZIGCC $CFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done < <(find "$ROOT/arch" "$ROOT/core" "$ROOT/drivers" "$ROOT/hal" -name '*.c' | sort)

echo "[build] 汇编架构层 .S"
while IFS= read -r src; do
    obj="$(objname "$src")"
    $ZIGCC $CFLAGS -c "$src" -o "$obj"
    OBJS+=("$obj")
done < <(find "$ROOT/arch" -name '*.S' ! -name 'boot.S' | sort)

echo "[build] 链接内核镜像"
$ZIGCC $CFLAGS -Wl,--build-id=none -Wl,-T,"$ROOT/linker.ld" -Wl,-e,_start64 \
    -Wl,-z,max-page-size=4096 \
    -o "$BUILD/hunyuan.elf" "${OBJS[@]}"

$ZIGOBJCOPY --output-target=binary "$BUILD/hunyuan.elf" "$BUILD/hunyuan.bin"

echo "[build] 汇编引导扇区"
$ZIGCC $CFLAGS -c "$ROOT/arch/x86_64/boot/boot.S" -o "$OBJDIR/boot.o"
$ZIGCC $CFLAGS -Wl,--build-id=none -Wl,-T,"$ROOT/arch/x86_64/boot/boot.ld" \
    -o "$BUILD/boot.elf" "$OBJDIR/boot.o"
$ZIGOBJCOPY --output-target=binary "$BUILD/boot.elf" "$BUILD/boot.bin"

echo "[build] 汇编并链接 UEFI 引导程序（PE32+ 前身）"
$ZIGCC $CFLAGS -c "$ROOT/uefi/boot.S" -o "$OBJDIR/uefi_boot.S.o"
$ZIGCC $CFLAGS -Wl,--build-id=none -Wl,-T,"$ROOT/uefi/uefi.ld" -Wl,-e,efi_main \
    -Wl,-z,max-page-size=4096 -o "$BUILD/uefi.elf" "$OBJDIR/uefi_boot.S.o"

echo "[build] ELF -> PE32+，并注入内核负载与回填字段"
python "$ROOT/tools/mkpe.py" "$BUILD/uefi.elf" "$BUILD/hunyuan.bin" \
    "$BUILD/hunyuan.elf" "$BUILD/BOOTX64.EFI"

echo "[build] 合成双引导磁盘镜像（保护性 MBR + GPT + FAT32 ESP + 内核裸区）"
python "$ROOT/tools/mkgpt.py" "$BUILD/boot.bin" "$BUILD/hunyuan.bin" \
    "$BUILD/BOOTX64.EFI" "$BUILD/hunyuan.img"

echo "[build] 离线结构校验（MBR / GPT / FAT32 / PE / 两条路径内核字节一致性）"
python "$ROOT/tools/imgcheck.py" "$BUILD/hunyuan.img" "$BUILD/boot.bin" \
    "$BUILD/hunyuan.bin" "$BUILD/hunyuan.elf" "$BUILD/BOOTX64.EFI"

echo "[build] 另生成仅 BIOS 的精简镜像，便于快速迭代"
python "$ROOT/tools/mkdisk.py" "$BUILD/boot.bin" "$BUILD/hunyuan.bin" "$BUILD/hunyuan-bios.img"

ls -l "$BUILD/hunyuan.elf" "$BUILD/uefi.elf" "$BUILD/hunyuan.bin" "$BUILD/boot.bin" \
      "$BUILD/BOOTX64.EFI" "$BUILD/hunyuan.img" "$BUILD/hunyuan-bios.img"
echo "[build] 完成：build/hunyuan.img 同时支持 BIOS(MBR) 与 UEFI(GPT+ESP) 两条引导路径"