#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
HynOS —— 用户程序镜像检查 + 生成内核可链接的 C 头

做两件事：

一、位置无关性硬校验（不靠"跑起来没崩"来判断）
   用户程序会被内核整段拷到**动态分配**的用户页上执行，链接基址是 0。因此必须满足：
     1) 没有重定位表（SHT_RELA/SHT_REL）——链接期已把所有引用定死，且必须是 RIP 相对的；
     2) 没有非空的 .got/.got.plt/.plt/.interp/.dynamic ——没有走 GOT/PLT/动态链接；
3) 入口地址落在**有初始内容的已分配段**里 ——内核把镜像首字节摊在基址处，
        入口只能指向真实代码（不能指向空洞或纯 bss）；
     4) 已分配段之间只允许**空隙**（链接器对齐所致，按地址零填充即可还原），
        **绝不允许重叠** ——重叠会静默覆盖代码，属于镜像错位；
     5) 未初始化段（NOBITS/.bss）按零填充写进镜像 ——内核给的用户页本就是零，
        显式填零与"整段拷贝"等价，且镜像长度即真实占用，便于人工核对。
    任何一条不满足都直接报错退出，让构建失败，而不是把不确定性留到运行期。

二、把平铺镜像写成 C 数组（build/include/hyn/userimg.h）
   为什么自己拼镜像而不用 objcopy：本机 ziglang 自带的 objcopy 对这份 ELF 静默产出 0 字节
   （不报错也不输出），直接拼段表反而更可控、更能复核。生成 C 数组后，内核侧只是普通
   const 数组引用，链接脚本不必新增段、汇编器不必处理相对路径。

用法：
  python tools/mkuserimg.py <out.h> <数组名> <elf> [<数组名> <elf> ...]
另外会在每个 ELF 同目录写出同名 .bin（平铺镜像，供人工对照/离线核对）。
"""
import os
import struct
import sys

SHT_RELA = 4
SHT_REL = 9
SHT_DYNAMIC = 6
SHT_NOBITS = 8
SHF_ALLOC = 0x2
BANNED_ALLOC_NAMES = (".got", ".got.plt", ".plt", ".plt.got", ".interp", ".dynamic",
                      ".rela.dyn", ".rel.dyn", ".rela.plt", ".iplt", ".got.plt.sec")


def read_elf(path):
    with open(path, "rb") as fp:
        blob = fp.read()
    if blob[:4] != b"\x7fELF":
        raise SystemExit("[mkuserimg] 不是 ELF 文件：%s" % path)
    if blob[4] != 2 or blob[5] != 1:
        raise SystemExit("[mkuserimg] 只支持 ELF64 小端：%s" % path)
    (e_type, e_machine, _ver, e_entry, _phoff, e_shoff, _flags, _ehsize,
     _phentsize, _phnum, e_shentsize, e_shnum, e_shstrndx) = struct.unpack_from(
        "<HHIQQQIHHHHHH", blob, 16)

    secs = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        (name_off, s_type, s_flags, s_addr, s_offset, s_size, s_link, s_info,
         s_align, s_entsize) = struct.unpack_from("<IIQQQQIIQQ", blob, off)
        secs.append({"name_off": name_off, "type": s_type, "flags": s_flags,
                     "addr": s_addr, "offset": s_offset, "size": s_size,
                     "align": s_align, "entsize": s_entsize})

    strtab_off = secs[e_shstrndx]["offset"] if e_shnum > 0 else 0
    for s in secs:
        end = blob.index(b"\x00", strtab_off + s["name_off"])
        s["name"] = blob[strtab_off + s["name_off"]:end].decode("utf-8", "replace")
    return blob, e_type, e_entry, secs


def verify_and_extract(elf_path, tag):
    blob, e_type, e_entry, secs = read_elf(elf_path)
    problems = []

    if e_type != 2:
        problems.append("ELF 类型不是 ET_EXEC（e_type=%d）" % e_type)

    for s in secs:
        if s["type"] in (SHT_RELA, SHT_REL):
            problems.append("残留重定位表：%s (type=%d, %d 项)"
                            % (s["name"], s["type"], s["size"] // (s["entsize"] or 1)))
        if s["type"] == SHT_DYNAMIC:
            problems.append("残留动态段：%s" % s["name"])
        if s["name"] in BANNED_ALLOC_NAMES and s["size"] > 0:
            problems.append("出现了非空的功能段：%s（%d 字节）" % (s["name"], s["size"]))

    alloc = sorted([s for s in secs if (s["flags"] & SHF_ALLOC)],
                   key=lambda s: (s["addr"], s["type"] == SHT_NOBITS))
    if not alloc:
        problems.append("没有任何已分配段")
        raise_system_exit(problems, elf_path)

    first = alloc[0]
    if first["addr"] != 0:
        problems.append("首个已分配段 %s 不在地址 0（addr=0x%x）" % (first["name"], first["addr"]))
    entry_ok = any(s["type"] != SHT_NOBITS and s["addr"] <= e_entry < s["addr"] + s["size"]
                   for s in alloc)
    if not entry_ok:
        problems.append("入口 0x%x 不属于任何有初始内容的已分配段 —— 镜像首字节不是入口代码" % e_entry)

    # 拼接平铺镜像（详细规则见文件头"一、位置无关性硬校验"第 4、5 条）
    image = bytearray()
    for s in alloc:
        if s["type"] == SHT_NOBITS:
            if s["addr"] + s["size"] <= len(image):
                continue
            if s["addr"] < len(image):
                problems.append("未初始化段 %s (0x%x+%d) 与前一已分配段重叠"
                                % (s["name"], s["addr"], s["size"]))
                break
            image.extend(b"\x00" * (s["addr"] + s["size"] - len(image)))
            continue
        if s["size"] == 0:
            continue                       # LLD 常留下 size=0 的空 .got，不参与镜像
        if s["addr"] < len(image):
            problems.append("已分配段重叠：%s 起始 0x%x 落在已填充区域 0x%x 之内"
                            % (s["name"], s["addr"], len(image)))
            break
        if s["addr"] > len(image):
            image.extend(b"\x00" * (s["addr"] - len(image)))
        image += blob[s["offset"]:s["offset"] + s["size"]]

    if problems:
        raise_system_exit(problems, elf_path)

    text = ", ".join("%s@0x%x(%d)" % (s["name"], s["addr"], s["size"]) for s in alloc)
    print("[mkuserimg] %-14s 校验通过：e_entry=0x%x / %d 字节 / 已分配段 %s / 无重定位·GOT·PLT"
          % (tag, e_entry, len(image), text))
    return bytes(image)


def raise_system_exit(problems, elf_path):
    for p in problems:
        sys.stderr.write("[mkuserimg] 位置无关性校验失败（%s）：%s\n"
                         % (os.path.basename(elf_path), p))
    raise SystemExit(1)


def emit_array(out, name, blob, indent="    "):
    out.write("const unsigned long %s_size = %dul;\n" % (name, len(blob)))
    out.write("const unsigned char %s[] = {\n" % name)
    line = indent
    for i, b in enumerate(blob):
        line += "0x%02x," % b
        if (i + 1) % 16 == 0:
            out.write(line + "\n")
            line = indent
        elif (i + 1) % 8 == 0:
            line += " "
    if line.strip():
        out.write(line + "\n")
    out.write("};\n\n")


def main(argv):
    if len(argv) < 4 or (len(argv) - 2) % 2 != 0:
        sys.stderr.write(__doc__)
        return 2

    header = argv[1]
    groups = [(argv[i], argv[i + 1]) for i in range(2, len(argv), 2)]

    parts = []
    summaries = []
    for name, elf_path in groups:
        base = os.path.basename(elf_path)
        tag = base
        image = verify_and_extract(elf_path, tag)
        if not image:
            sys.stderr.write("[mkuserimg] 用户程序镜像为空：%s\n" % elf_path)
            return 1
        bin_path = os.path.splitext(elf_path)[0] + ".bin"
        with open(bin_path, "wb") as fp:
            fp.write(image)
        parts.append((name, "/* 来自 %s（%d 字节） */\n" % (base, len(image)), image))
        summaries.append("%s=%d 字节" % (base, len(image)))

    import io
    buf = io.StringIO()
    for name, body, image in parts:
        buf.write(body)
        emit_array(buf, name, image)

    os.makedirs(os.path.dirname(header), exist_ok=True)
    with open(header, "w", encoding="utf-8", newline="\n") as fp:
        fp.write("/* 由 tools/mkuserimg.py 生成，请勿手改。 */\n")
        fp.write("#ifndef HYN_USERIMG_H\n#define HYN_USERIMG_H\n\n")
        fp.write(buf.getvalue())
        fp.write("#endif /* HYN_USERIMG_H */\n")

    print("[mkuserimg] 已生成 %s（%s）" % (header, "；".join(summaries)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))