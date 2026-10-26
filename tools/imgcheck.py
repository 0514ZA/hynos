#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
混元 OS —— 双引导镜像离线结构校验工具

对 mkgpt.py 产出的镜像做"只读、可重复"的结构取证，逐项打印结论：
  A. 保护性 MBR：0x55AA、自研引导代码字节、回填字段、保护性分区项
  B. GPT：主/备份头 CRC、分区项数组 CRC、ESP 分区项边界与类型 GUID
  C. 内核裸区：与内核镜像逐字节比对（BIOS 路径真正读的那份字节）
  D. FAT32 ESP：BPB、簇数 ≥ 65525、两份 FAT 一致、目录链与簇链、
                从 ESP 中取回 BOOTX64.EFI 并与构建产物逐字节比对
  E. PE32+：DOS/PE 头、机器与子系统、节与数据目录、.
               内嵌负载与内核字节一致、两个构建期回填字段与内核符号一致

用法:
    imgcheck.py <img> <boot_code.bin> <kernel.bin> <kernel.elf> <BOOTX64.EFI>
退出码：0 = 全部通过；1 = 存在失败项
"""
import json
import os
import struct
import sys
import zlib

SECTOR = 512
KERNEL_RAW_LBA = 34
ESP_LBA = 2048
ESP_SECTORS = 81920
ESP_RESERVED = 32
ESP_FATSZ = 640
ESP_ROOT_CLUSTER = 2
KERNEL_BIN_BASE = 0x100000
MACHINE_AMD64 = 0x8664
PE32P_MAGIC = 0x20B
SUBSYSTEM_EFI_APP = 10
ESP_TYPE_GUID = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")

FAILURES = []
CHECKS = 0


def check(name, ok, detail=""):
    global CHECKS
    CHECKS += 1
    if not ok:
        FAILURES.append(name)
    print("  [%s] %s%s" % ("通过" if ok else "失败", name,
                           ("  —— " + detail) if detail else ""))


def elf_symbols(path):
    """只取符号表里的 名字->值，用于独立复算内核 _start_uefi 偏移。"""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        raise SystemExit("镜像校验：%s 不是 ELF" % path)
    shoff, = struct.unpack_from("<Q", d, 0x28)
    shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
    secs = []
    for i in range(shnum):
        off = shoff + i * shentsize
        name, stype, flags, addr, offset, size, link, info, align, ent = \
            struct.unpack_from("<IIQQQQIIQQ", d, off)
        secs.append((name, stype, flags, addr, offset, size, link))
    out = {}
    for (_n, stype, _fl, _a, offset, size, link) in secs:
        if stype != 2:                      # SHT_SYMTAB
            continue
        stroff = secs[link][4]
        for i in range(size // 24):
            n, _i2, _o2, _sh, value, _sz = struct.unpack_from("<IBBHQQ", d, offset + i * 24)
            if n == 0:
                continue
            end = d.index(b"\0", stroff + n)
            nm = d[stroff + n:end].decode("utf-8", "replace")
            if nm:
                out[nm] = value
    return out


def check_mbr(img, boot_code, kernel):
    print("[A] 保护性 MBR（LBA 0）")
    mbr = img[0:SECTOR]
    check("结束标志 0x55AA", mbr[510:512] == b"\x55\xaa")
    check("自研引导代码字节一致", mbr[0:len(boot_code)] == boot_code,
          "%d 字节" % len(boot_code))
    sectors = struct.unpack_from("<H", mbr, 0x1B0)[0]
    kbytes = struct.unpack_from("<I", mbr, 0x1B4)[0]
    check("回填扇区数（0x1B0）", sectors == (len(kernel) + SECTOR - 1) // SECTOR,
          "%d 扇区" % sectors)
    check("回填字节数（0x1B4）", kbytes == len(kernel), "%d 字节" % kbytes)
    check("保护性分区项类型 0xEE", mbr[0x1BE + 4] == 0xEE)
    start, size = struct.unpack_from("<II", mbr, 0x1BE + 8)
    check("保护性分区项覆盖 LBA 1..末尾",
          start == 1 and size == min(len(img) // SECTOR - 1, 0xFFFFFFFF),
          "start=%d size=%d" % (start, size))
    check("引导代码未越界覆盖回填字段", len(boot_code) <= 0x1B0,
          "上限 432 字节")


def check_gpt(img):
    print("[B] GPT 分区表")
    disk_sectors = len(img) // SECTOR
    last = disk_sectors - 1
    hdr = img[SECTOR * 1:SECTOR * 2]
    check("主头签名 EFI PART", hdr[0:8] == b"EFI PART")
    revision, hsize, hcrc, _res = struct.unpack_from("<IIII", hdr, 8)
    check("主头版本 1.0 / 头长 92", revision == 0x00010000 and hsize == 92)
    body = bytearray(hdr[:hsize])
    struct.pack_into("<I", body, 16, 0)
    check("主头 CRC32 自洽", (zlib.crc32(bytes(body)) & 0xFFFFFFFF) == hcrc,
          "0x%08x" % hcrc)
    my, alt, first_usable, last_usable = struct.unpack_from("<QQQQ", hdr, 24)
    entries_lba, count, esize = struct.unpack_from("<QII", hdr, 72)
    array_crc, = struct.unpack_from("<I", hdr, 88)
    check("主头 LBA 布局（My=1, Alt=末扇区）", my == 1 and alt == last,
          "My=%d Alt=%d" % (my, alt))
    check("分区项数组位置与规格", entries_lba == 2 and count == 128 and esize == 128)
    check("分区项数组 CRC32 自洽",
          (zlib.crc32(img[SECTOR * 2:SECTOR * 34]) & 0xFFFFFFFF) == array_crc,
          "0x%08x" % array_crc)

    esp_type = img[SECTOR * 2:SECTOR * 2 + 16]
    esp_first, esp_last = struct.unpack_from("<QQ", img, SECTOR * 2 + 32)
    esp_name = img[SECTOR * 2 + 56:SECTOR * 2 + 56 + 36].decode("utf-16-le").rstrip("\0")
    check("ESP 类型 GUID 为 EFI System", esp_type == ESP_TYPE_GUID)
    check("ESP 边界落在可用区内且与生成参数一致",
          esp_first == ESP_LBA and esp_last == ESP_LBA + ESP_SECTORS - 1
          and first_usable <= esp_first and esp_last <= last_usable,
          "LBA %d..%d，可用区 %d..%d" % (esp_first, esp_last, first_usable, last_usable))
    check("ESP 分区名", esp_name == "EFI System", repr(esp_name))

    bhdr = img[SECTOR * last:SECTOR * (last + 1)]
    check("备份头签名 EFI PART", bhdr[0:8] == b"EFI PART")
    bbody = bytearray(bhdr[:92])
    bcrc, = struct.unpack_from("<I", bhdr, 16)
    struct.pack_into("<I", bbody, 16, 0)
    check("备份头 CRC32 自洽", (zlib.crc32(bytes(bbody)) & 0xFFFFFFFF) == bcrc)
    b_my, b_alt, b_entries, b_crc = struct.unpack_from("<QQQI", bhdr, 24)[0], \
        struct.unpack_from("<QQQI", bhdr, 24)[1], \
        struct.unpack_from("<Q", bhdr, 72)[0], \
        struct.unpack_from("<I", bhdr, 88)[0]
    check("备份头指向末扇区且互指主头", b_my == last and b_alt == 1)
    check("备份分区项数组与主数组一致",
          img[SECTOR * (disk_sectors - 33):SECTOR * (disk_sectors - 1)]
          == img[SECTOR * 2:SECTOR * 34] and b_entries == disk_sectors - 33
          and b_crc == array_crc)


def check_kernel_raw(img, kernel):
    print("[C] BIOS 路径内核裸区（LBA 34 起）")
    off = SECTOR * KERNEL_RAW_LBA
    check("裸区内核字节与构建产物一致", img[off:off + len(kernel)] == kernel,
          "%d 字节 @ LBA %d" % (len(kernel), KERNEL_RAW_LBA))


def fat_chain(fat, start):
    chain = []
    cur = start
    while cur not in (0, 0x0FFFFFFF, 0x0FFFFFF8) and len(chain) < 100000:
        chain.append(cur)
        nxt, = struct.unpack_from("<I", fat, cur * 4)
        cur = nxt & 0x0FFFFFFF
    return chain


def check_esp(img, loader_efi):
    print("[D] FAT32 EFI 系统分区")
    base = SECTOR * ESP_LBA
    bs = img[base:base + SECTOR]
    byts, = struct.unpack_from("<H", bs, 0x0B)
    spc = bs[0x0D]
    rsvd, = struct.unpack_from("<H", bs, 0x0E)
    nfats = bs[0x10]
    fatsz32, = struct.unpack_from("<I", bs, 0x24)
    tot32, = struct.unpack_from("<I", bs, 0x20)
    root_clus, = struct.unpack_from("<I", bs, 0x2C)
    fsinfo, = struct.unpack_from("<H", bs, 0x30)
    bkboot, = struct.unpack_from("<H", bs, 0x32)
    check("BPB 参数（512 字节/扇区、1 扇区/簇、2 份 FAT）",
          byts == 512 and spc == 1 and nfats == 2 and rsvd == ESP_RESERVED,
          "byts=%d spc=%d nfats=%d rsvd=%d" % (byts, spc, nfats, rsvd))
    check("总扇区与 FAT 大小与生成参数一致",
          tot32 == ESP_SECTORS and fatsz32 == ESP_FATSZ,
          "tot=%d fatsz=%d" % (tot32, fatsz32))
    check("RootClus / FSInfo / 备份引导扇区", root_clus == ESP_ROOT_CLUSTER
          and fsinfo == 1 and bkboot == 6)
    check("ESP 引导扇区结束标志 0x55AA", bs[510:512] == b"\x55\xaa")
    fsi_main = img[base + SECTOR:base + 2 * SECTOR]
    fsi_bak = img[base + 7 * SECTOR:base + 8 * SECTOR]
    check("FSInfo 主副本签名有效",
          struct.unpack_from("<I", fsi_main, 0)[0] == 0x41615252
          and struct.unpack_from("<I", fsi_main, 484)[0] == 0x61417272
          and struct.unpack_from("<I", fsi_main, 508)[0] == 0xAA550000,
          "RRaA/rrAa/尾签")
    check("FSInfo 备份副本与主副本一致", fsi_bak == fsi_main)
    check("备份引导扇区与主副本一致", img[base + 6 * SECTOR:base + 7 * SECTOR] == bs)

    clusters = (tot32 - rsvd - nfats * fatsz32) // spc
    check("簇数 ≥ 65525（判定为 FAT32）", clusters >= 65525, "%d 簇" % clusters)
    fat1 = img[base + rsvd * SECTOR:base + (rsvd + fatsz32) * SECTOR]
    fat2 = img[base + (rsvd + fatsz32) * SECTOR:base + (rsvd + 2 * fatsz32) * SECTOR]
    check("两份 FAT 内容一致", fat1 == fat2)

    data_start = rsvd + nfats * fatsz32

    def clus_off(n):
        return base + (data_start + (n - ESP_ROOT_CLUSTER) * spc) * SECTOR

    def read_dir(cluster):
        out = []
        raw = img[clus_off(cluster):clus_off(cluster) + SECTOR]
        for i in range(16):
            e = raw[i * 32:(i + 1) * 32]
            if len(e) < 32 or e[0] == 0:
                break
            # 8.3 名 = 前 8 字节主名 + 后 3 字节扩展名，各自右侧空格填充。必须按规范
            # 拆分再拼回，否则磁盘上的错名（如 BOOTX64E.FI）会与裸 11 字节期望串
            # "刚好相等"而漏检，固件却找不到 BOOTX64.EFI。
            nm11 = e[0:11]
            base = nm11[0:8].decode("latin-1").rstrip(" ")
            ext = nm11[8:11].decode("latin-1").rstrip(" ")
            name = base + ("." + ext if ext else "")
            attr = e[11]
            hi, = struct.unpack_from("<H", e, 20)
            lo, = struct.unpack_from("<H", e, 26)
            size, = struct.unpack_from("<I", e, 28)
            out.append((name, attr, (hi << 16) | lo, size))
        return out

    root = read_dir(ESP_ROOT_CLUSTER)
    names = [r[0] for r in root]
    check("根目录含 EFI 子目录", "EFI" in names, " / ".join(names))
    efi = read_dir(ESP_ROOT_CLUSTER + 1)
    check("EFI 目录含 BOOT 子目录", "BOOT" in [e[0] for e in efi])
    boot = read_dir(ESP_ROOT_CLUSTER + 2)
    entry = [b for b in boot if b[0] == "BOOTX64.EFI"]
    check("BOOT 目录含 8.3 名 BOOTX64.EFI 的文件", len(entry) == 1,
          " / ".join(n for n, _a, _c, _s in boot))
    if len(entry) == 1:
        _n, attr, clus, size = entry[0]
        check("文件大小与构建产物一致", size == len(loader_efi),
              "%d 字节" % size)
        chain = fat_chain(fat1, clus)
        check("文件簇链长度覆盖文件大小", len(chain) * SECTOR >= size,
              "%d 簇" % len(chain))
        check("文件簇链连续且无环", chain == list(range(clus, clus + len(chain))))
        data = b"".join(img[clus_off(c):clus_off(c) + SECTOR] for c in chain)[:size]
        check("从 ESP 取回的 BOOTX64.EFI 与构建产物逐字节一致", data == loader_efi)
        return data, clus, chain
    return None, None, None


def check_pe(loader_bytes, kernel, kernel_elf, sidecar_path):
    print("[E] PE32+ 引导程序")
    pe = loader_bytes
    check("DOS 头 MZ", pe[0:2] == b"MZ")
    e_lfanew, = struct.unpack_from("<I", pe, 0x3C)
    check("PE 签名", struct.unpack_from("<I", pe, e_lfanew)[0] == 0x00004550)
    coff = e_lfanew + 4
    machine, nsec, _ts, _ps, _ns, optsz, chars = struct.unpack_from("<HHIIIHH", pe, coff)
    check("机器 0x8664（x86-64）", machine == MACHINE_AMD64, "0x%04x" % machine)
    opt = coff + 20
    magic, = struct.unpack_from("<H", pe, opt)
    check("可选头 PE32+（0x20B）", magic == PE32P_MAGIC)
    entry, = struct.unpack_from("<I", pe, opt + 16)
    image_base, = struct.unpack_from("<Q", pe, opt + 24)
    size_image, = struct.unpack_from("<I", pe, opt + 56)
    headers_size, = struct.unpack_from("<I", pe, opt + 60)
    subsystem, = struct.unpack_from("<H", pe, opt + 68)
    check("Subsystem = EFI Application (10)", subsystem == SUBSYSTEM_EFI_APP,
          "%d" % subsystem)
    check("镜像非零基址（固件可直接按地址装载）", image_base != 0, "0x%x" % image_base)

    secs = []
    for i in range(nsec):
        o = opt + optsz + i * 40
        nm = pe[o:o + 8].rstrip(b"\0").decode("latin-1")
        vsize, va, rawsize, rawoff, _pr, _pl, _nr, _nl, schars = \
            struct.unpack_from("<IIIIIIHHI", pe, o + 8)
        secs.append((nm, vsize, va, rawsize, rawoff, schars))
    check("节头数量 ≥ 2（含 .text）", nsec >= 2, " / ".join(s[0] for s in secs))
    text = [s for s in secs if s[0] == ".text"]
    check("第一个节为 .text 且含入口",
          bool(text) and text[0][2] <= entry < text[0][2] + text[0][1],
          "entry RVA 0x%x" % entry)
    relocs = struct.unpack_from("<II", pe, opt + 112 + 5 * 8)
    check("存在 .reloc 数据目录（合法占位块，可被固件重定位）",
          relocs[0] != 0 and relocs[1] == 12, "size=%d" % relocs[1])
    check("每一节的 RVA 按 SectionAlignment(0x1000) 对齐",
          all(s[2] % 0x1000 == 0 for s in secs),
          " / ".join("%s@0x%x" % (s[0], s[2]) for s in secs))
    check("SizeOfImage 按 SectionAlignment 对齐且覆盖全部节",
          size_image % 0x1000 == 0 and size_image >= max(s[2] + s[1] for s in secs),
          "0x%x" % size_image)
    check("镜像大小不小于头长度", size_image >= headers_size, "0x%x" % size_image)

    with open(sidecar_path, "r", encoding="utf-8") as f:
        meta = json.load(f)
    text_off = meta["headers_size"]

    def rva_to_off(rva):
        return text_off + (rva - meta["text_rva"])

    fsize_off = rva_to_off(meta["kernel_size_field_rva"])
    fentry_off = rva_to_off(meta["kernel_entry_field_rva"])
    poff = rva_to_off(meta["payload_rva"])
    check("回填字段 g_kernel_size 等于内核字节数",
          struct.unpack_from("<Q", pe, fsize_off)[0] == len(kernel),
          "%d" % len(kernel))
    symbols = elf_symbols(kernel_elf)
    expect_entry_off = symbols["_start_uefi"] - KERNEL_BIN_BASE
    got_entry_off, = struct.unpack_from("<Q", pe, fentry_off)
    check("回填字段 g_kernel_entry_off 独立复算一致",
          got_entry_off == expect_entry_off, "0x%x" % got_entry_off)
    check("内嵌内核负载与内核镜像逐字节一致",
          pe[poff:poff + len(kernel)] == kernel, "%d 字节" % len(kernel))
    check("负载不超过预留区", len(kernel) <= meta["payload_reserved"],
          "%d / %d" % (len(kernel), meta["payload_reserved"]))


def main():
    if len(sys.argv) != 6:
        print("用法: imgcheck.py <img> <boot_code.bin> <kernel.bin> <kernel.elf> <BOOTX64.EFI>",
              file=sys.stderr)
        sys.exit(2)
    img_path, boot_path, kernel_path, kernel_elf, efi_path = sys.argv[1:6]
    with open(img_path, "rb") as f:
        img = f.read()
    with open(boot_path, "rb") as f:
        boot_code = f.read()
    with open(kernel_path, "rb") as f:
        kernel = f.read()
    with open(efi_path, "rb") as f:
        efi = f.read()

    print("[imgcheck] 镜像 %s：%d 扇区 / %d 字节" % (os.path.basename(img_path),
                                                    len(img) // SECTOR, len(img)))
    check_mbr(img, boot_code, kernel)
    check_gpt(img)
    check_kernel_raw(img, kernel)
    esp_efi, _clus, _chain = check_esp(img, efi)
    if esp_efi is not None:
        check_pe(esp_efi, kernel, kernel_elf, efi_path + ".json")
    print("[imgcheck] 共 %d 项检查，失败 %d 项" % (CHECKS, len(FAILURES)))
    if FAILURES:
        print("[imgcheck] 失败项：%s" % "、".join(FAILURES))
        sys.exit(1)
    print("[imgcheck] 结论：镜像、GPT、FAT32 ESP、PE32+ 与两条路径的内核字节一致性校验全部通过")


if __name__ == "__main__":
    main()