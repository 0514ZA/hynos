#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
混元 OS —— 单镜像双引导合成工具（保护性 MBR + GPT + FAT32 ESP + BIOS 内核裸区）

一个镜像文件同时满足两条引导路径：

  BIOS/MBR 路径（传统）：
    LBA 0        保护性 MBR：前 432 字节是自研引导代码，0x1BE 起是保护性分区项
                 回填字段 0x1B0=内核扇区数、0x1B4=内核字节数
    LBA 34 起    内核镜像裸区（引导代码按固定 LBA 34 用 EDD 读入）

  UEFI/GPT 路径（现代）：
    LBA 1        GPT 主头，LBA 2..33 分区项数组
    LBA 2048      FAT32 EFI 系统分区（ESP），内含 \\EFI\\BOOT\\BOOTX64.EFI
    末尾 33 扇区   GPT 备份头 + 备份分区项数组

两条路径进入的是**同一份内核字节**：裸区放的是 build/hunyuan.bin，
BOOTX64.EFI 内嵌的负载也是 build/hunyuan.bin（由 tools/mkpe.py 注入）。
ESP 用 512 字节簇，故 40MiB 规模下簇数 80608 ≥ 65525，符合 FAT32 判定阈值。

用法:
    mkgpt.py <boot_code.bin> <kernel.bin> <BOOTX64.EFI> <out.img>
"""
import struct
import sys
import zlib

SECTOR = 512
BOOT_CODE_MAX = 0x1B0          # 432：回填字段 0x1B0/0x1B4 之前的全部空间
SEC_OFF = 0x1B0                # u16 内核占用扇区数
SIZE_OFF = 0x1B4               # u32 内核字节数
MBR_PART_OFF = 0x1BE
MAX_KERNEL = 384 * 1024        # 与引导期读入区 0x10000..0x70000 一致

KERNEL_RAW_LBA = 34            # 内核裸区固定 LBA（引导代码中写死）
KERNEL_RAW_SECTORS = 768       # 预留 384KB，允许内核后续增长

ESP_LBA = 2048                 # 1MiB 对齐
ESP_SECTORS = 81920            # 40 MiB
ESP_RESERVED = 32
ESP_FATSZ = 640
ESP_CLUSTER_SECTORS = 1
ESP_FATS = 2
ESP_ROOT_CLUSTER = 2

GPT_ENTRY_COUNT = 128
GPT_ENTRY_SIZE = 128
GPT_ENTRY_BYTES = GPT_ENTRY_COUNT * GPT_ENTRY_SIZE

# GUID 固定写死：保证镜像可复现（非随机）
DISK_GUID = bytes.fromhex("48594e4b000000000000000000000001")
ESP_UNIQUE_GUID = bytes.fromhex("48594e4b0000000000000000000000ef")
ESP_TYPE_GUID = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")   # EFI System Partition
FAT32_EOC = 0x0FFFFFFF
# 目录项时间戳固定值（2026-10-26，时间 0），避免严格 FAT 驱动把全零时间视为非法
FAT_DATE = ((2026 - 1980) << 9) | (10 << 5) | 26


def die(msg):
    print("[mkgpt] 错误: %s" % msg, file=sys.stderr)
    sys.exit(1)


def align_up(value, align):
    return (value + align - 1) // align * align


def build_protective_mbr(boot_code, kernel_bytes, disk_sectors):
    if len(boot_code) > BOOT_CODE_MAX:
        die("引导代码 %d 字节超过 %d 字节上限（会覆盖回填字段或分区表）"
            % (len(boot_code), BOOT_CODE_MAX))
    sectors = (kernel_bytes + SECTOR - 1) // SECTOR
    if sectors > 0xFFFF:
        die("内核扇区数 %d 超出 u16 表示范围" % sectors)

    mbr = bytearray(SECTOR)
    mbr[0:len(boot_code)] = boot_code
    struct.pack_into("<H", mbr, SEC_OFF, sectors)
    struct.pack_into("<I", mbr, SIZE_OFF, kernel_bytes)
    # 保护性分区项：类型 0xEE，覆盖 LBA 1..末尾
    mbr[MBR_PART_OFF] = 0x00
    mbr[MBR_PART_OFF + 1:MBR_PART_OFF + 4] = b"\x00\x02\x00"
    mbr[MBR_PART_OFF + 4] = 0xEE
    mbr[MBR_PART_OFF + 5:MBR_PART_OFF + 8] = b"\xff\xff\xff"
    struct.pack_into("<II", mbr, MBR_PART_OFF + 8, 1, min(disk_sectors - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    return bytes(mbr)


def build_gpt_entry(type_guid, unique_guid, first_lba, last_lba, attrs, name):
    entry = bytearray(GPT_ENTRY_SIZE)
    entry[0:16] = type_guid
    entry[16:32] = unique_guid
    struct.pack_into("<QQQ", entry, 32, first_lba, last_lba, attrs)
    name_bytes = name.encode("utf-16-le")
    if len(name_bytes) > 72:
        die("分区名过长")
    entry[56:56 + len(name_bytes)] = name_bytes
    return bytes(entry)


def build_gpt_header(my_lba, alt_lba, first_usable, last_usable, entries_lba, entries_crc):
    hdr = bytearray(92)
    hdr[0:8] = b"EFI PART"
    struct.pack_into("<II", hdr, 8, 0x00010000, 92)
    struct.pack_into("<QQQQ", hdr, 24, my_lba, alt_lba, first_usable, last_usable)
    hdr[56:72] = DISK_GUID
    struct.pack_into("<QII", hdr, 72, entries_lba, GPT_ENTRY_COUNT, GPT_ENTRY_SIZE)
    struct.pack_into("<I", hdr, 88, entries_crc)
    crc = zlib.crc32(bytes(hdr)) & 0xFFFFFFFF
    struct.pack_into("<I", hdr, 16, crc)
    return bytes(hdr)


def dir_entry(name11, attr, cluster, size):
    e = bytearray(32)
    e[0:11] = name11
    e[11] = attr
    struct.pack_into("<HH", e, 14, 0, FAT_DATE)          # 创建时间 / 日期
    struct.pack_into("<H", e, 18, FAT_DATE)              # 最后访问日期
    struct.pack_into("<HH", e, 22, 0, FAT_DATE)          # 修改时间 / 日期
    struct.pack_into("<H", e, 20, (cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", e, 26, cluster & 0xFFFF)
    struct.pack_into("<I", e, 28, size)
    return bytes(e)


def dot_entries(cluster, parent):
    return (dir_entry(b".          ", 0x10, cluster, 0)
            + dir_entry(b"..         ", 0x10, parent, 0))


def build_esp(loader):
    clusters = (ESP_SECTORS - ESP_RESERVED - ESP_FATS * ESP_FATSZ) // ESP_CLUSTER_SECTORS
    if clusters < 65525:
        die("ESP 簇数 %d < 65525，会退化成 FAT16，不符合 FAT32 ESP 要求" % clusters)
    if (clusters + 2) * 4 > ESP_FATSZ * SECTOR:
        die("FAT 表 %d 字节容不下 %d 个簇项" % (ESP_FATSZ * SECTOR, clusters))

    data_start_sector = ESP_RESERVED + ESP_FATS * ESP_FATSZ

    def cluster_off(n):
        return (data_start_sector + (n - ESP_ROOT_CLUSTER) * ESP_CLUSTER_SECTORS) * SECTOR

    file_clusters = (len(loader) + SECTOR - 1) // SECTOR
    first_file_cluster = ESP_ROOT_CLUSTER + 3          # 2=根, 3=EFI, 4=BOOT, 5=文件首簇
    if first_file_cluster + file_clusters - 1 > clusters + 1:
        die("引导程序 %d 字节放不进 ESP" % len(loader))

    esp = bytearray(ESP_SECTORS * SECTOR)

    # ---- 引导扇区 + FSInfo（含备份） ----
    bs = bytearray(SECTOR)
    bs[0:3] = b"\xeb\x58\x90"
    bs[3:11] = b"HYNOS   "
    struct.pack_into("<H", bs, 0x0B, SECTOR)
    bs[0x0D] = ESP_CLUSTER_SECTORS
    struct.pack_into("<H", bs, 0x0E, ESP_RESERVED)
    bs[0x10] = ESP_FATS
    struct.pack_into("<H", bs, 0x11, 0)
    struct.pack_into("<H", bs, 0x13, 0)
    bs[0x15] = 0xF8
    struct.pack_into("<H", bs, 0x16, 0)
    struct.pack_into("<H", bs, 0x18, 63)
    struct.pack_into("<H", bs, 0x1A, 255)
    struct.pack_into("<I", bs, 0x1C, ESP_LBA)
    struct.pack_into("<I", bs, 0x20, ESP_SECTORS)
    struct.pack_into("<I", bs, 0x24, ESP_FATSZ)
    struct.pack_into("<H", bs, 0x28, 0)
    struct.pack_into("<H", bs, 0x2A, 0)
    struct.pack_into("<I", bs, 0x2C, ESP_ROOT_CLUSTER)
    struct.pack_into("<H", bs, 0x30, 1)
    struct.pack_into("<H", bs, 0x32, 6)
    bs[0x40] = 0x80
    bs[0x42] = 0x29
    struct.pack_into("<I", bs, 0x43, 0x48594E01)
    bs[0x47:0x47 + 11] = b"HYNEFI     "
    bs[0x52:0x52 + 8] = b"FAT32   "
    bs[510:512] = b"\x55\xaa"
    esp[0:SECTOR] = bs

    used = 4 + file_clusters                    # 根/EFI/BOOT 三簇 + 文件簇
    fsi = bytearray(SECTOR)
    struct.pack_into("<I", fsi, 0, 0x41615252)
    struct.pack_into("<I", fsi, 484, 0x61417272)
    struct.pack_into("<I", fsi, 488, clusters - used)
    struct.pack_into("<I", fsi, 492, first_file_cluster + file_clusters)
    struct.pack_into("<I", fsi, 508, 0xAA550000)
    esp[SECTOR:SECTOR * 2] = fsi
    esp[SECTOR * 6:SECTOR * 7] = bs             # 备份引导扇区
    esp[SECTOR * 7:SECTOR * 8] = fsi            # 备份 FSInfo

    # ---- FAT 表（两份，内容一致） ----
    fat = bytearray(ESP_FATSZ * SECTOR)

    def set_fat(n, value):
        struct.pack_into("<I", fat, n * 4, value)

    set_fat(0, 0x0FFFFFF8)
    set_fat(1, 0x0FFFFFFF)
    set_fat(ESP_ROOT_CLUSTER, FAT32_EOC)
    set_fat(ESP_ROOT_CLUSTER + 1, FAT32_EOC)                 # EFI 目录
    set_fat(ESP_ROOT_CLUSTER + 2, FAT32_EOC)                 # BOOT 目录
    for i in range(file_clusters):
        cur = first_file_cluster + i
        set_fat(cur, FAT32_EOC if i == file_clusters - 1 else cur + 1)
    for k in range(ESP_FATS):
        start = (ESP_RESERVED + k * ESP_FATSZ) * SECTOR
        esp[start:start + len(fat)] = fat

    # ---- 目录树：根 -> EFI -> BOOT -> BOOTX64.EFI ----
    # FAT32 根目录是普通簇链，规范上不使用 "." / ".."（那是子目录才有的项）
    root = dir_entry(b"HYNEFI     ", 0x08, 0, 0)             # 卷标项
    root += dir_entry(b"EFI        ", 0x10, ESP_ROOT_CLUSTER + 1, 0)
    esp[cluster_off(ESP_ROOT_CLUSTER):cluster_off(ESP_ROOT_CLUSTER) + len(root)] = root

    efi = dot_entries(ESP_ROOT_CLUSTER + 1, ESP_ROOT_CLUSTER)
    efi += dir_entry(b"BOOT       ", 0x10, ESP_ROOT_CLUSTER + 2, 0)
    esp[cluster_off(ESP_ROOT_CLUSTER + 1):cluster_off(ESP_ROOT_CLUSTER + 1) + len(efi)] = efi

    boot = dot_entries(ESP_ROOT_CLUSTER + 2, ESP_ROOT_CLUSTER + 1)
    # 8.3 名恰好 11 字节 = 8 字节主名 + 3 字节扩展名：'BOOTX64' + 'EFI'
    # （写成 b"BOOTX64EFI " 会把主名读成 'BOOTX64E'、扩展名读成 'FI '，固件找不到 BOOTX64.EFI）
    boot += dir_entry(b"BOOTX64 EFI", 0x20, first_file_cluster, len(loader))
    esp[cluster_off(ESP_ROOT_CLUSTER + 2):cluster_off(ESP_ROOT_CLUSTER + 2) + len(boot)] = boot

    esp[cluster_off(first_file_cluster):cluster_off(first_file_cluster) + len(loader)] = loader
    return bytes(esp)


def main():
    if len(sys.argv) != 5:
        die("用法: mkgpt.py <boot_code.bin> <kernel.bin> <BOOTX64.EFI> <out.img>")
    boot_path, kernel_path, loader_path, out_path = sys.argv[1:5]

    with open(boot_path, "rb") as f:
        boot_code = f.read()
    with open(kernel_path, "rb") as f:
        kernel = f.read()
    with open(loader_path, "rb") as f:
        loader = f.read()

    if not kernel:
        die("内核镜像为空")
    if len(kernel) > MAX_KERNEL:
        die("内核 %d 字节超过 %d 字节（引导期读入区上限）" % (len(kernel), MAX_KERNEL))
    if len(kernel) > KERNEL_RAW_SECTORS * SECTOR:
        die("内核 %d 字节超过裸区 %d 字节" % (len(kernel), KERNEL_RAW_SECTORS * SECTOR))
    if not loader:
        die("UEFI 引导程序为空")
    if loader[:2] != b"MZ":
        die("UEFI 引导程序不是 PE 文件（缺少 MZ 头）")

    disk_sectors = align_up(ESP_LBA + ESP_SECTORS + 33, 2048)
    last_lba = disk_sectors - 1
    first_usable = KERNEL_RAW_LBA
    last_usable = disk_sectors - 34
    if ESP_LBA + ESP_SECTORS - 1 > last_usable:
        die("ESP 越出 GPT 可用区")

    esp_entries = build_gpt_entry(ESP_TYPE_GUID, ESP_UNIQUE_GUID,
                                  ESP_LBA, ESP_LBA + ESP_SECTORS - 1, 0, "EFI System")
    entries = esp_entries + b"\0" * (GPT_ENTRY_BYTES - len(esp_entries))
    entries_crc = zlib.crc32(entries) & 0xFFFFFFFF

    img = bytearray(disk_sectors * SECTOR)
    img[0:SECTOR] = build_protective_mbr(boot_code, len(kernel), disk_sectors)
    # GPT 头只有 92 字节，必须补齐到整扇区再写入：切片赋值长度不等会改变镜像长度
    img[SECTOR * 1:SECTOR * 2] = build_gpt_header(1, last_lba, first_usable,
                                                  last_usable, 2,
                                                  entries_crc).ljust(SECTOR, b"\0")
    img[SECTOR * 2:SECTOR * 34] = entries
    img[SECTOR * KERNEL_RAW_LBA:SECTOR * KERNEL_RAW_LBA + len(kernel)] = kernel
    img[SECTOR * ESP_LBA:SECTOR * (ESP_LBA + ESP_SECTORS)] = build_esp(loader)
    img[SECTOR * (disk_sectors - 33):SECTOR * (disk_sectors - 1)] = entries
    img[SECTOR * last_lba:SECTOR * disk_sectors] = build_gpt_header(
        last_lba, 1, first_usable, last_usable, disk_sectors - 33,
        entries_crc).ljust(SECTOR, b"\0")

    if len(img) != disk_sectors * SECTOR:
        die("镜像长度 %d 与预期 %d 不符（扇区写入长度不一致）"
            % (len(img), disk_sectors * SECTOR))

    with open(out_path, "wb") as f:
        f.write(bytes(img))

    print("[mkgpt] 引导代码 %d/%d 字节；内核 %d 字节 / %d 扇区 @ LBA %d；"
          "ESP %d 扇区 @ LBA %d（%d 字节 BOOTX64.EFI）；镜像 %d 扇区 / %d 字节 -> %s"
          % (len(boot_code), BOOT_CODE_MAX, len(kernel),
             (len(kernel) + SECTOR - 1) // SECTOR, KERNEL_RAW_LBA,
             ESP_SECTORS, ESP_LBA, len(loader), disk_sectors, len(img), out_path))


if __name__ == "__main__":
    main()