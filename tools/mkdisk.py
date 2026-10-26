#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
混元 OS —— 精简 BIOS 镜像生成工具（仅 MBR 路径，用于快速迭代）

它是 tools/mkgpt.py 的"只留 BIOS 路径"版本：磁盘布局与双引导镜像保持一致，
只是不写 GPT 分区表与 FAT32 ESP，因此产物体积从约 42MB 降到几十 KB：

    LBA 0      : 保护性 MBR（自研引导代码 + 回填字段 + 0x55AA）
    LBA 1..33  : 留空（双引导镜像里是 GPT 主头与分区项数组）
    LBA 34 起  : 内核镜像裸区（与引导代码写死的起始 LBA 一致）

正式交付与 UEFI 路径一律使用 tools/mkgpt.py 产出的 build/hunyuan.img；
本工具只保证"BIOS 路径的引导行为与整体镜像完全一致"。

回填字段（引导汇编按固定物理地址读取，不得超过 432 字节代码空间）：
    0x7C00 + 0x1B0 : u16 内核占用扇区数
    0x7C00 + 0x1B4 : u32 内核字节数
"""
import sys

SECTOR = 512
BOOT_CODE_MAX = 0x1B0         # 432 字节：0x1BE 起是 MBR 分区表，不允许覆盖
SEC_OFF = 0x1B0
SIZE_OFF = 0x1B4
KERNEL_RAW_LBA = 34           # 与 arch/x86_64/boot/boot.S 的 KERNEL_LBA 一致
MAX_KERNEL = 0x60000          # 384KB：引导期读入区 0x10000..0x70000


def die(msg):
    print("[mkdisk] 错误: %s" % msg, file=sys.stderr)
    sys.exit(1)


def main():
    if len(sys.argv) != 4:
        die("用法: mkdisk.py <boot_code.bin> <kernel.bin> <out.img>")
    boot_path, kernel_path, out_path = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(boot_path, "rb") as f:
        boot_code = f.read()
    with open(kernel_path, "rb") as f:
        kernel = f.read()

    if len(boot_code) > BOOT_CODE_MAX:
        die("引导代码 %d 字节超出 %d 字节上限（会覆盖回填字段或分区表）"
            % (len(boot_code), BOOT_CODE_MAX))
    if not kernel:
        die("内核镜像为空")
    if len(kernel) > MAX_KERNEL:
        die("内核镜像 %d 字节超过引导期读入区上限 %d 字节" % (len(kernel), MAX_KERNEL))

    sectors = (len(kernel) + SECTOR - 1) // SECTOR
    if sectors > 0xFFFF:
        die("内核扇区数 %d 超出 u16 表示范围" % sectors)

    mbr = bytearray(SECTOR)
    mbr[0:len(boot_code)] = boot_code
    mbr[SEC_OFF:SEC_OFF + 2] = sectors.to_bytes(2, "little")
    mbr[SIZE_OFF:SIZE_OFF + 4] = len(kernel).to_bytes(4, "little")
    mbr[510:512] = b"\x55\xaa"

    padded = kernel
    if len(kernel) % SECTOR:
        padded += b"\x00" * (SECTOR - len(kernel) % SECTOR)

    with open(out_path, "wb") as f:
        f.write(bytes(mbr))
        f.write(b"\x00" * (SECTOR * (KERNEL_RAW_LBA - 1)))
        f.write(padded)

    print("[mkdisk] 精简 BIOS 镜像：引导代码 %d/%d 字节；内核 %d 字节 / %d 扇区 @ LBA %d；"
          "总计 %d 字节 -> %s"
          % (len(boot_code), BOOT_CODE_MAX, len(kernel), sectors, KERNEL_RAW_LBA,
             SECTOR * (KERNEL_RAW_LBA + len(padded) // SECTOR), out_path))


if __name__ == "__main__":
    main()