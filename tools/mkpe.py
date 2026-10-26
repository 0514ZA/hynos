#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HynOS —— UEFI 引导程序 ELF -> PE32+ 转换与构建期注入工具

把 uefi/boot.S 经 uefi/uefi.ld 链接出的 ELF64 转成固件可加载的 PE32+ 可执行体
（EFI Application，Subsystem = 10），并完成两项构建期注入：

  1) 内核镜像字节写入 .data 末尾预留的"内嵌负载区"（符号 __kernel_payload_start）
  2) 回填 g_kernel_size / g_kernel_entry_off 两个字段（按符号名定位）

产物为单一 PE 文件：内核负载随 PE 镜像一起被固件加载，引导程序因此不需要读磁盘、
不需要文件系统协议，只依赖 ConOut / GetMemoryMap / ExitBootServices 三个固件接口。

RVA 约定（重要）：
  ELF 链接基址是 0x10000000，而 PE 的第一个节必须从 RVA 0x1000 开始（RVA 0 留给
  PE 头）。因此整块装载内容被统一平移 IMAGE_SHIFT = 0x1000：
      PE 中的 RVA = IMAGE_SHIFT + (ELF 地址 - 装载基址)
  代码全部 RIP 相对寻址，整体平移不改变任何相对距离，故运行行为与 ELF 布局一致。

固件加载器（EDK2 BasePeCoffLib）硬约束（踩坑记录，勿改回去）：
  * 每个节的 VirtualAddress 必须是 SectionAlignment 的整数倍。早先 .bss/.reloc 只做
    4 字节对齐，固件直接判 Load Error，UEFI 路径连引导程序第一条日志都没输出。
  * .reloc 数据目录若存在，其指向的重定位块必须结构合法（SizeOfBlock 至少 8 且能让
    遍历前进）。早先写成"8 字节全零"的伪空表，被加载器判为非法重定位表。
  现在统一放一个"仅含 IMAGE_REL_BASED_ABSOLUTE(0) 占位项"的合法块：加载器按规范
  忽略该类项，本程序又确实不需要任何真实重定位。

用法:
    mkpe.py <loader.elf> <kernel.bin> <kernel.elf> <out.efi>
"""
import json
import os
import struct
import sys

PE_SECTION_ALIGN = 0x1000
PE_FILE_ALIGN = 0x200
PE32P_MAGIC = 0x20B
MACHINE_AMD64 = 0x8664
SUBSYSTEM_EFI_APP = 10
DOS_STUB_OFF = 0x80
OPT_HDR_SIZE = 0xF0
IMAGE_SHIFT = 0x1000            # .text 的 RVA，也是 ELF 布局到 PE RVA 的统一平移量
KERNEL_BIN_BASE = 0x100000
RELOC_SIZE = 12                 # 8 字节块头 + 2 字节 ABSOLUTE 项 + 2 字节对齐填充
SHF_ALLOC = 0x2
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_NOBITS = 8


def die(msg):
    print("[mkpe] 错误: %s" % msg, file=sys.stderr)
    sys.exit(1)


def align_up(value, align):
    return (value + align - 1) // align * align


class Elf64:
    """只解析本工具需要的内容：可装载节 + 符号表。"""

    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if len(d) < 64 or d[:4] != b"\x7fELF" or d[4] != 2 or d[5] != 1:
            die("%s 不是小端 ELF64 文件" % path)
        self.entry, = struct.unpack_from("<Q", d, 0x18)
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        if shnum == 0 or shentsize < 64:
            die("%s 缺少有效的节头表" % path)
        self.sections = []
        for i in range(shnum):
            off = shoff + i * shentsize
            name, stype, flags, addr, offset, size, link, _info, _align, _ent = \
                struct.unpack_from("<IIQQQQIIQQ", d, off)
            self.sections.append({"name": name, "type": stype, "flags": flags,
                                  "addr": addr, "offset": offset, "size": size,
                                  "link": link})
        shstr = self.sections[shstrndx]
        for s in self.sections:
            pos = shstr["offset"] + s["name"]
            end = d.index(b"\0", pos)
            s["sname"] = d[pos:end].decode("utf-8", "replace")
        self.symbols = {}
        for s in self.sections:
            if s["type"] != SHT_SYMTAB:
                continue
            strtab = self.sections[s["link"]]
            for i in range(s["size"] // 24):
                o = s["offset"] + i * 24
                n, _info, _other, _shndx, value, _size = struct.unpack_from("<IBBHQQ", d, o)
                if n == 0:
                    continue
                pos = strtab["offset"] + n
                end = d.index(b"\0", pos)
                name = d[pos:end].decode("utf-8", "replace")
                if name:
                    self.symbols[name] = value

    def sym(self, name):
        if name not in self.symbols:
            die("%s 中找不到符号 %s（链接时是否被 strip 掉了？）" % (self.path, name))
        return self.symbols[name]


def build_pe(img, secs, base, entry_rva, size_of_image, headers_size):
    buf = bytearray(headers_size)
    buf[0:2] = b"MZ"
    msg = b"This program cannot be run in DOS mode.\r\r\n$"
    buf[0x4E:0x4E + len(msg)] = msg
    struct.pack_into("<I", buf, 0x3C, DOS_STUB_OFF)

    struct.pack_into("<I", buf, DOS_STUB_OFF, 0x00004550)          # 'PE\0\0'
    coff = DOS_STUB_OFF + 4
    struct.pack_into("<HHIIIHH", buf, coff,
                     MACHINE_AMD64, len(secs), 0, 0, 0, OPT_HDR_SIZE, 0x0022)

    opt = coff + 20
    text_raw = secs[0][3]
    init_data_raw = sum(s[3] for s in secs[1:])
    struct.pack_into("<HBBIIIII", buf, opt,
                     PE32P_MAGIC, 0, 0,
                     text_raw,                                  # SizeOfCode
                     init_data_raw,                             # SizeOfInitializedData
                     0,                                         # SizeOfUninitializedData
                     entry_rva,
                     secs[0][2])                                # BaseOfCode
    struct.pack_into("<QIIHHHHHHIIIIHHQQQQII", buf, opt + 24,
                     base, PE_SECTION_ALIGN, PE_FILE_ALIGN,
                     0, 0, 0, 0, 0, 0,
                     0,                                         # Win32VersionValue
                     size_of_image, headers_size, 0,
                     SUBSYSTEM_EFI_APP, 0,
                     0x100000, 0x1000, 0, 0, 0, 16)
    reloc_rva, reloc_size = secs[-1][2], secs[-1][1]
    struct.pack_into("<II", buf, opt + 112 + 5 * 8, reloc_rva, reloc_size)

    sh = opt + OPT_HDR_SIZE
    for i, (name, vsize, va, rawsize, rawoff, chars) in enumerate(secs):
        o = sh + i * 40
        buf[o:o + 8] = name.ljust(8, b"\0")
        struct.pack_into("<IIIIIIHHI", buf, o + 8,
                         vsize, va, rawsize, rawoff, 0, 0, 0, 0, chars)
    return bytes(buf)


def verify_pe(pe, meta, kernel):
    """产物自检：重新解析 PE，核对结构与两项注入结果。"""

    def fail(m):
        die("产物 PE 自检失败: %s" % m)

    e_lfanew, = struct.unpack_from("<I", pe, 0x3C)
    if pe[0:2] != b"MZ" or struct.unpack_from("<I", pe, e_lfanew)[0] != 0x00004550:
        fail("DOS/PE 签名异常")
    coff = e_lfanew + 4
    machine, nsec, _ts, _psym, _nsym, optsz, _chars = struct.unpack_from("<HHIIIHH", pe, coff)
    if machine != MACHINE_AMD64 or nsec != meta["sections"] or optsz != OPT_HDR_SIZE:
        fail("COFF 头字段异常")
    opt = coff + 20
    magic, = struct.unpack_from("<H", pe, opt)
    if magic != PE32P_MAGIC:
        fail("不是 PE32+")
    entry, = struct.unpack_from("<I", pe, opt + 16)
    base, = struct.unpack_from("<Q", pe, opt + 24)
    size_image, = struct.unpack_from("<I", pe, opt + 56)
    subsystem, = struct.unpack_from("<H", pe, opt + 68)
    if entry != meta["entry_rva"] or base != meta["image_base"] \
            or size_image != meta["size_of_image"]:
        fail("入口/基址/镜像大小不一致")
    if subsystem != SUBSYSTEM_EFI_APP:
        fail("Subsystem 不是 EFI Application")
    if size_image % PE_SECTION_ALIGN:
        fail("SizeOfImage 未按 SectionAlignment 对齐")

    # 节表：VA 与文件偏移的对齐是固件加载器的硬要求，逐个核对
    sh = opt + OPT_HDR_SIZE
    reloc_raw_off = None
    for i in range(nsec):
        o = sh + i * 40
        _vsize, va, rawsize, rawoff, _pr, _pl, _nr, _nl, _chars = \
            struct.unpack_from("<IIIIIIHHI", pe, o + 8)
        if va % PE_SECTION_ALIGN:
            fail("节 #%d 的 RVA 0x%x 未按 SectionAlignment 0x%x 对齐"
                 % (i, va, PE_SECTION_ALIGN))
        if rawsize and rawoff % PE_FILE_ALIGN:
            fail("节 #%d 的文件偏移 0x%x 未按 FileAlignment 0x%x 对齐"
                 % (i, rawoff, PE_FILE_ALIGN))
        if i == nsec - 1:
            reloc_raw_off = rawoff

    reloc_rva, reloc_size = struct.unpack_from("<II", pe, opt + 112 + 5 * 8)
    if reloc_rva != meta["reloc_rva"] or reloc_size != RELOC_SIZE:
        fail(".reloc 目录缺失或大小异常")
    blk_va, blk_size = struct.unpack_from("<II", pe, reloc_raw_off)
    if blk_va != IMAGE_SHIFT or blk_size != RELOC_SIZE:
        fail(".reloc 占位块内容异常（块头应为 RVA 0x%x / SizeOfBlock %d）"
             % (IMAGE_SHIFT, RELOC_SIZE))

    def rva_to_off(rva):
        return meta["headers_size"] + (rva - IMAGE_SHIFT)

    if struct.unpack_from("<Q", pe, rva_to_off(meta["kernel_size_field_rva"]))[0] != len(kernel):
        fail("g_kernel_size 回填值不等于内核字节数")
    if struct.unpack_from("<Q", pe, rva_to_off(meta["kernel_entry_field_rva"]))[0] \
            != meta["kernel_entry_off"]:
        fail("g_kernel_entry_off 回填值与内核 _start_uefi 偏移不一致")
    poff = rva_to_off(meta["payload_rva"])
    if pe[poff:poff + len(kernel)] != kernel:
        fail("内嵌负载字节与内核镜像不一致")


def main():
    if len(sys.argv) != 5:
        die("用法: mkpe.py <loader.elf> <kernel.bin> <kernel.elf> <out.efi>")
    loader_elf, kernel_bin, kernel_elf, out_path = sys.argv[1:5]

    lle = Elf64(loader_elf)
    kle = Elf64(kernel_elf)
    with open(kernel_bin, "rb") as f:
        kernel = f.read()
    if not kernel:
        die("内核镜像为空")

    kernel_entry_off = kle.sym("_start_uefi") - KERNEL_BIN_BASE
    if not 0 < kernel_entry_off < len(kernel):
        die("_start_uefi 偏移 %d 越界（内核共 %d 字节）" % (kernel_entry_off, len(kernel)))

    alloc = [s for s in lle.sections
             if (s["flags"] & SHF_ALLOC) and s["type"] in (SHT_PROGBITS, SHT_NOBITS) and s["size"]]
    if not alloc:
        die("引导程序 ELF 没有可装载节")
    base = min(s["addr"] for s in alloc)
    progbits = sorted([s for s in alloc if s["type"] == SHT_PROGBITS], key=lambda s: s["addr"])
    nobits = [s for s in alloc if s["type"] == SHT_NOBITS]

    # 按 ELF 布局拼出装载内容（偏移相对于装载基址）
    img = bytearray()
    for s in progbits:
        off = s["addr"] - base
        if off < 0:
            die("节 %s 地址低于装载基址" % s["sname"])
        if len(img) < off:
            img.extend(b"\0" * (off - len(img)))
        chunk = lle.data[s["offset"]:s["offset"] + s["size"]]
        if len(chunk) != s["size"]:
            die("节 %s 的文件内容不完整" % s["sname"])
        img[off:off + s["size"]] = chunk
    text_size = len(img)

    bss_off = bss_size = 0
    if nobits:
        bss_off = min(s["addr"] for s in nobits) - base
        bss_end = max(s["addr"] + s["size"] for s in nobits) - base
        if bss_off < text_size:
            die(".bss 与已装载内容重叠")
        bss_size = bss_end - bss_off

    payload_off = lle.sym("__kernel_payload_start") - base
    payload_reserve = lle.sym("__kernel_payload_end") - lle.sym("__kernel_payload_start")
    if payload_reserve <= 0 or not 0 <= payload_off < text_size:
        die("内嵌负载区未在链接脚本中正确预留")
    if len(kernel) > payload_reserve:
        die("内核镜像 %d 字节超过内嵌负载预留 %d 字节" % (len(kernel), payload_reserve))

    fsize_off = lle.sym("g_kernel_size") - base
    fentry_off = lle.sym("g_kernel_entry_off") - base
    struct.pack_into("<Q", img, fsize_off, len(kernel))
    struct.pack_into("<Q", img, fentry_off, kernel_entry_off)
    img[payload_off:payload_off + len(kernel)] = kernel

    entry_off = lle.entry - base
    if not 0 <= entry_off < text_size:
        die("入口 efi_main 不在已装载内容内")

    # ELF 布局偏移 -> PE RVA（统一平移 IMAGE_SHIFT）。
    # 每个节的 RVA 都必须按 SectionAlignment 对齐，否则固件加载器直接拒绝加载。
    entry_rva = IMAGE_SHIFT + entry_off
    bss_rva = align_up(IMAGE_SHIFT + bss_off, PE_SECTION_ALIGN) if bss_size else 0
    reloc_rva = align_up(max(IMAGE_SHIFT + text_size, bss_rva + bss_size), PE_SECTION_ALIGN)
    # 结构合法的占位重定位块：一条 IMAGE_REL_BASED_ABSOLUTE(0) 项 + 2 字节填充，
    # SizeOfBlock = 12，遍历会正常推进到块尾，加载器按规范忽略该类型的项。
    reloc_block = struct.pack("<IIHH", IMAGE_SHIFT, RELOC_SIZE, 0, 0)
    reloc_size = RELOC_SIZE
    size_of_image = align_up(reloc_rva + reloc_size, PE_SECTION_ALIGN)
    text_raw = align_up(text_size, PE_FILE_ALIGN)
    reloc_raw = align_up(reloc_size, PE_FILE_ALIGN)

    secs = []
    add = secs.append
    add((b".text", text_size, IMAGE_SHIFT, text_raw, 0, 0x60000020))
    if bss_size:
        add((b".bss", bss_size, bss_rva, 0, 0, 0xC0000080))
    add((b".reloc", reloc_size, reloc_rva, reloc_raw, 0, 0x42000040))
    for s in secs:
        if s[2] % PE_SECTION_ALIGN:
            die("节 %s 的 RVA 0x%x 未按 SectionAlignment 0x%x 对齐"
                % (s[0].decode(), s[2], PE_SECTION_ALIGN))

    headers_size = align_up(DOS_STUB_OFF + 4 + 20 + OPT_HDR_SIZE + len(secs) * 40, PE_FILE_ALIGN)
    rawoff = headers_size
    for i, s in enumerate(secs):
        if s[3]:
            secs[i] = (s[0], s[1], s[2], s[3], rawoff, s[5])
            rawoff += s[3]

    headers = build_pe(img, secs, base, entry_rva, size_of_image, headers_size)
    out = bytearray(headers)
    out += img
    out += b"\0" * (text_raw - text_size)
    out += reloc_block
    out += b"\0" * (reloc_raw - reloc_size)
    with open(out_path, "wb") as f:
        f.write(bytes(out))

    meta = {
        "efi": os.path.basename(out_path),
        "pe_size": len(out),
        "image_base": base,
        "sections": len(secs),
        "headers_size": headers_size,
        "text_rva": IMAGE_SHIFT,
        "text_size": text_size,
        "text_raw": text_raw,
        "entry_rva": entry_rva,
        "bss_rva": bss_rva,
        "bss_size": bss_size,
        "reloc_rva": reloc_rva,
        "reloc_size": reloc_size,
        "size_of_image": size_of_image,
        "payload_rva": IMAGE_SHIFT + payload_off,
        "payload_reserved": payload_reserve,
        "payload_size": len(kernel),
        "kernel_bytes": len(kernel),
        "kernel_size_field_rva": IMAGE_SHIFT + fsize_off,
        "kernel_entry_field_rva": IMAGE_SHIFT + fentry_off,
        "kernel_entry_off": kernel_entry_off,
    }
    verify_pe(bytes(out), meta, kernel)
    with open(out_path + ".json", "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False, indent=2, sort_keys=True)

    print("[mkpe] PE32+ %d 字节；.text RVA 0x1000 / %d 字节；入口 RVA 0x%x；"
          "内嵌负载 %d 字节 @ RVA 0x%x（预留 %d）；内核 _start_uefi 偏移 0x%x -> %s"
          % (len(out), text_size, entry_rva, len(kernel), meta["payload_rva"],
             payload_reserve, kernel_entry_off, out_path))


if __name__ == "__main__":
    main()