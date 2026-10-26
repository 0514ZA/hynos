# 混元智能分层混合内核（Hunyuan Layered Hybrid Kernel）

x86_64 裸机内核。Ring0 分为「混元硬核底座」+「快通道分组故障域」；跨故障域通信强制走
ALPC 消息、禁止跨域直接函数调用；代码分为架构无关核心 + arch 架构适配层；上层 AI 与应用
全部跑在 Ring3 用户态，依靠 syscall + ALPC 与内核通信。

本文件说明**如何构建、如何启动、以及哪些结论是实测出来的**。实现边界见最后一节——
本文档不声称任何未实现的文件系统、网络或驱动能力。

---

## 1. 目录结构

```
hunyuan-kernel/
├── build.sh                构建脚本（编译用户态 -> 编译/链接内核 -> 生成双引导镜像 -> 离线校验）
├── run.sh                  启动脚本（QEMU 引导同一镜像的 BIOS 或 UEFI 路径并抓取串口日志）
├── linker.ld               内核链接脚本（入口段排最前，丢弃 .eh_frame/.note/.comment）
│
├── include/hyn/            内核公共头（纯接口，不含 CPU 专属代码）
├── core/                   架构无关核心（纯 C，不绑定 CPU）
│   ├── kmain.c             内核主入口与全部自检
│   ├── thread/             sched.c（调度/上下文切换）、syscall.c（Ring3 唯一入口）
│   ├── ipc/alpc.c          ALPC 端口消息总线
│   ├── object/object.c     内核对象管理器（命名对象 / 句柄代次 / 类型清理回调）
│   ├── vm/                 pmm.c（位图物理页分配）、vmm.c（PML4 四级页表）
│   ├── fault/              domain.c（故障域框架）+ group_storage/group_net/group_bus
│   └── lib/                kprintf.c、kstring.c
│
├── arch/x86_64/            arch 架构适配层（CPU 专属代码只放这里）
│   ├── boot/               boot.S（MBR 引导扇区）、boot32.S、entry.S、entry_uefi.S
│   ├── gdt.c idt.c isr.S trap.c pic.c pit.c tss.c thread.S cpu.S
│   ├── mmu.c               四级页表硬件操作
│   └── pci.c cpuinfo.c
│
├── hal/                    硬件抽象层（最简底层外设驱动）
│   └── hal.c serial.c framebuffer.c
├── drivers/                分组所属驱动（存储组 VFS / 网络组栈 / 总线组）
│   ├── vfs.c net.c bus.c
├── uefi/                   UEFI 引导程序（PE32+，汇编，仅用 3 个固件接口）
│   └── boot.S uefi.ld
├── user/                   Ring3 用户态（libhyn + 示例）
│   ├── lib/libhyn.c mem.c entry.S
│   ├── include/libhyn.h
│   ├── examples/shell.c agent.c
│   └── user.ld
├── tools/                  自研构建/校验工具（纯 Python，无第三方依赖）
│   ├── mkuserimg.py        ELF -> 平铺镜像（并硬校验位置无关性）
│   ├── mkpe.py             ELF -> PE32+（注入内核负载、回填大小与入口偏移）
│   ├── mkgpt.py            保护性 MBR + GPT + FAT32 ESP + 内核裸区合成
│   ├── mkdisk.py           仅 BIOS 的精简镜像
│   └── imgcheck.py         离线结构校验（53 项）
├── firmware/               随包 OVMF 固件（OVMF_CODE.fd + OVMF_VARS.fd）
└── build/                  构建产物与启动日志
```

架构无关层与 arch 适配层的边界：`core/`、`hal/`、`drivers/` 全部是纯 C，不出现任何
x86_64 指令或寄存器；所有 CPU 专属动作（GDT/IDT 装载、CR3 切换、上下文切换汇编、
端口 I/O、页表硬件操作）都收敛在 `arch/x86_64/`。`aarch64/`、`riscv64/` 目录为跨架构预留
占位，**当前未实现**，现阶段只做 x86_64。

---

## 2. 构建

### 前置依赖

| 依赖 | 用途 | 说明 |
| --- | --- | --- |
| Python 3 | 运行构建工具与 ziglang 模块 | 实测于 Python 3.13 |
| ziglang 0.16.0 | 提供 clang / lld / objcopy | `pip install ziglang`，通过 `python -m ziglang` 调用 |
| QEMU | 运行验证 | 仅 `run.sh` 需要，见第 3 节 |

本工程**不依赖系统 gcc / clang / make**：本机无系统 GCC 工具链，全部编译统一走
`python -m ziglang cc`。如需自定义解释器或工具，用环境变量覆盖：

```bash
ZIGCC="python -m ziglang cc" ZIGOBJCOPY="python -m ziglang objcopy" ./build.sh
```

### 命令

```bash
cd hunyuan-kernel
./build.sh
```

`build.sh` 的完整流程（顺序有意义）：

1. 编译 Ring3 用户态库（`libhyn`）与两个示例程序（Shell / Agent）；
2. 由 `tools/mkuserimg.py` 生成平铺镜像**并硬校验位置无关性**（无重定位表、无非空 GOT-PLT、
   入口在首段内），同时生成内核用的 `userimg.h`——用户程序是以 `const` 数组形式链进内核 `.rodata` 的；
3. 编译 `arch/` `core/` `drivers/` `hal/` 下全部 C 源码与 `arch/` 下汇编；
4. 链接内核、`objcopy` 出平铺 `.bin`；
5. 汇编 MBR 引导扇区；汇编并链接 UEFI 引导程序；
6. `tools/mkpe.py` 转 PE32+，注入内核负载并回填 `g_kernel_size` / `g_kernel_entry_off`；
7. `tools/mkgpt.py` 合成**同时支持两条引导路径**的单一镜像 `build/hunyuan.img`；
8. `tools/imgcheck.py` 离线结构校验。

可复现构建：构建时间戳取自 `HYN_BUILD_STAMP` 环境变量，未设置时才取当前时间；
源码中不使用 `__DATE__` / `__TIME__`。

### 构建产物

| 产物 | 说明 |
| --- | --- |
| `build/hunyuan.img` | **交付主产物**：GPT + FAT32 ESP + 内核裸区的双引导镜像（44040192 字节） |
| `build/hunyuan.bin` | 内核平铺镜像（92952 字节 / 182 扇区） |
| `build/hunyuan.elf` | 内核 ELF（含符号，供调试） |
| `build/BOOTX64.EFI` | UEFI 引导程序（PE32+，265216 字节） |
| `build/boot.bin` | MBR 引导代码（361 字节，上限 432） |
| `build/hunyuan-bios.img` | 仅 BIOS 的精简镜像，便于快速迭代 |

镜像布局：LBA0 保护性 MBR（保留 BIOS 引导代码）→ LBA1–33 GPT（主头 + 128 项 + 备份头）
→ LBA34 起内核裸区（BIOS 路径固定读此处）→ LBA2048 起 FAT32 ESP（40 MiB）。
两条引导路径进入的**是同一个内核镜像**，`imgcheck.py` 会逐字节比对回声区与 ESP 内
`.efi` 所载内核，确认字节完全一致。

---

## 3. 启动验证

```bash
./run.sh bios        # BIOS/MBR 路径（默认）
./run.sh uefi        # UEFI/GPT+ESP 路径
```

`run.sh` 会带上真实外设，让「设备已就绪」这句话有真硬件可探：

| 设备 | 归属 | 用途 |
| --- | --- | --- |
| `e1000` | 网络组 | PCI 类别 0x02，真读 CTRL/STATUS/RAL0/RAH0 寄存器 |
| `nvme` | 存储组 | PCI 类别 0x01/0x08，真读 CAP/VS/CSTS 寄存器 |
| `qemu-xhci` / `intel-hda` | 硬件总线组 | HAL 外设骨架登记 |

取证来源：串口 → `build/qemu-serial-<mode>.log`（内核自检的权威日志）；
调试口 0xE9 → `build/qemu-debug-<mode>.log`。内核自检结束写 `isa-debug-exit(0xF4)`，QEMU 随即退出。

### 依赖查找顺序

- **QEMU**：`$QEMU` 环境变量 → 工程上级 `../tools/qemu-msys2/mingw64/bin/` → 系统安装路径 →
  `PATH`。若 QEMU 不在以上位置，用环境变量指定：
  `QEMU=/path/to/qemu-system-x86_64.exe ./run.sh uefi`
- **OVMF**：命令行参数 → `$OVMF_CODE`/`$OVMF_VARS` → **工程内 `firmware/`**（随包提供，
  无需额外下载）→ 上级 `../tools/ovmf*` → 系统安装路径。`OVMF_VARS` 会被复制一份到
  `build/` 下使用，保证原始固件不被写坏。
- 超时：`QEMU_TIMEOUT`（默认 40 秒）。

---

## 4. 实测结论

以下是本工程实际编译、实际启动得到的结果，不是设计意图。

### 离线结构校验

```
[imgcheck] 共 53 项检查，失败 0 项
[imgcheck] 结论：镜像、GPT、FAT32 ESP、PE32+ 与两条路径的内核字节一致性校验全部通过
```

覆盖：MBR 回填字段与 `0x55AA` 签名、GPT 主备头与条目表 CRC、FAT32 保留扇区/FAT 链/目录项、
PE32+ 各字段与节对齐、`.reloc` 占位块、回填的 `g_kernel_size` / `g_kernel_entry_off` 独立复算一致、
内嵌内核负载与内核镜像逐字节一致。

### 两条引导路径自检

| 路径 | 结果 | 日志 |
| --- | --- | --- |
| BIOS/MBR（`boot_method=0`） | **72/72 项通过** | `build/qemu-serial-bios.log` |
| UEFI/GPT+ESP（`boot_method=1`） | **72/72 项通过** | `build/qemu-serial-uefi.log` |

UEFI 路径连跑两次结果一致。自检覆盖：GDT/IDT 与中断框架、`#BP`/`#PF` 异常捕获、
IRQ0 时钟投递、物理页分配与归还、PML4 页表生效与窗口映射/取消映射、对象管理器、
ALPC 端口与跨域消息往返、调度器优先级与时钟节拍抢占、syscall 唯一入口与 Ring3 派发、
故障域注册与单组重置、分组异常就地隔离、libhyn 全流程。

### 两条路径的真实读数差异（值得记录）

| 读数 | BIOS | UEFI(OVMF) |
| --- | --- | --- |
| NVMe BAR 基址 | `0x0febf8000` | `0xc000000000` |
| e1000 BAR 基址 | `0x0febc0000` | `0x081040000` |
| 内核动态窗口映射数 | 14 处 | 18 处 |

OVMF 会把 NVMe 的** 64 位 BAR 分配到 4 GiB 以上**（`0xc000000000`），而内核恒等映射只覆盖
0..4 GiB。早期的驱动门控在「BAR 超出恒等映射窗口」时直接放弃取寄存器读数，导致 UEFI 路径下
存储组只能如实报告「未取到读数」。修复方式是**补映射而非放宽判定**：`vmm_map_mmio()` 逐页检查
BAR 窗口，已覆盖的直接跳过（低 4 GiB 的 BAR 走这条快路径，行为与修复前完全一致），未覆盖的
按不可缓存属性（`PWT|PCD`）补建 4K 页映射，最后逐页复核翻译结果才返回成功。映射数从 14 增到 18
正好是 NVMe BAR 的 16 KiB ÷ 4 KiB = 4 页，e1000 的 BAR 仍在 4 GiB 以下故无需补映射——
这两条与预期吻合，说明修复作用点精确。

### 关键读数（BIOS 路径）

```
对象   : 存活 0 / 累计创建 1 / 打开句柄 0
端口   : 存活 1 / 累计投递消息 19 条 / 跨域直调拦截 1 次 / 未建链投递拦截 1 次
调度   : 在册线程 2 / 切换 17 次 / 抢占 1 次 / 空闲线程运行 4 次 / 节拍 6
存储组 : NVMe=1 类别=0x0108 厂商=0x1b36 设备=0x0010 BAR=16 KiB @0x0febf8000 版本=0x10400 CSTS=0x1 VFS 节点=5 挂载=1
网络组 : 网卡=1 厂商=0x8086 设备=0x100e BAR=128 KiB @0x0febc0000 链路=1 MAC=56:34:12:00:54:52
         校验和: 期望=0x52eb（内核复算 0x52eb）/ 回填后复算=0x0000 / LISTEN 槽位=3 / 已建立连接=0（不伪造）
总线组 : PCI 设备=9（桥 3）；ACPI RSDP=1 版本=0 校验和=1 表=4/4 RSDT=0xffe233b OEM=BOCHS 首表=FACP
跨域审计: 往返成功 11 次 / 被拒 2 次 / 跨域直调拦下 2 次 / 应答丢弃 0 条
libhyn 审计: 用户 syscall 27 次 / 非法指针挡下 3 次 / 越权收他人端口 2 次 / 身份回填 11 处
```

故障隔离与恢复的实测序列（同一轮自检内）：网络组服务线程触发内核态 `#PF` → 本组标记
`FAULTED`、调用方拿到「对方不可用」而非挂死、底座与其余两组纪元/重置计数不变 → 已故障组
再被调用时返回 `EFAULT_FAULT` 并计入拒绝数 → 安全点重置走 `quiesce -> reset -> probe` 全流程、
本组回到在线且纪元 +1、故障史保留 → 硬件重新认领成功、消息服务恢复。

---

## 5. 实现边界（未实现的部分，不得当作已完成）

本工程是**可启动、可自检、分层骨架完整**的内核，但以下能力**明确未实现**，代码与日志
中也不声称其可用：

- **文件系统**：VFS 只提供命名空间骨架（`/`、`dev`、`sys`、`proc`）与挂载项登记。
  无块读写、无目录项落盘、无页缓存、无文件句柄。`vfs` 不声称「文件系统可用」。
- **网络**：协议栈只登记 3 个 LISTEN 槽位骨架；**不实现**收发描述符环、DMA、ARP、
  IP 分片/转发、TCP 状态机与重传。故 `net_established_count()` 恒为 0、`tx_bytes` 恒为 0
  ——日志中「已建立连接=0（不伪造）」即此意。`net_ip_checksum()` 是真实的 RFC 791
  一位补码实现，不是常量。
- **HAL 外设**：Framebuffer（VGA 文本）为真实可用；USB（xHCI）、HDA 音频为**登记骨架**，
  仅完成 PCI 识别与设备登记，无控制器初始化与数据传输。NVMe 仅完成控制器认领与寄存器
  真读，无队列与块访问。
- **跨架构**：只实现 x86_64。`aarch64/`、`riscv64/` 为预留占位目录，无任何实现，
  现阶段不能称「支持多架构 CPU」。
- **调度**：静态线程池（16 槽位上限），无动态线程销毁回收，无 SMP。
- **驱动分层完整性**：`core/fault` 内已实现故障域框架，但 `core/fault` 之外的占位目录
  与未接入模块按骨架状态如实标注（自检报告中的「已实装 / 待接入」标记即来源于此）。

---

## 6. 纪律约定

本工程在实现过程中遵循两条硬约定，它们体现在代码与判定的每一处：

1. **不虚报能力**。所有「已就绪 / 已建立 / 可用」结论都必须有真实读数支撑：
   设备读数来自真读寄存器（全 0 / 全 1 均判为「没真读到」）、校验和来自真算、
   用户程序自报身份一律被内核纠正。未实现的能力在日志里明确写「不伪造」「待接入」，而不是留给读者猜。
2. **不为让判定通过而放宽判定**。出现失败时先定位根因，修复真实缺陷，而不是扩大接受范围。
   上文的 UEFI NVMe 读数问题即为典型：判定口径未动，改动的是内核的 MMIO 补映射能力。

---

## 7. 许可与商业化

本工程**著作权归作者所有**，采用**双授权**：

| 使用方式 | 许可 | 费用 |
| --- | --- | --- |
| 个人学习 / 研究；开源项目；以 AGPLv3 公开全部对应源码的方式使用与分发 | AGPL-3.0-only（见 `LICENSE`） | 免费 |
| 内嵌进闭源产品，或以网络服务对外提供但不按 AGPLv3 公开对应源码 | 商业授权 | 需付费取得 |

完整版权声明与第三方组件清单见 [`NOTICE.md`](NOTICE.md)；购买商业授权见
[`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md)（联系 1870688451@qq.com）。
参与贡献前请阅读 [`CONTRIBUTING.md`](CONTRIBUTING.md) 并签署 [`CLA.md`](CLA.md)——
双授权要求著作权人能取得贡献的「再许可」权，因此 CLA 不可跳过。

一句话说清这个结构：**AGPLv3 是真开源，谁都能免费用；只有「想闭源商用、不愿公开对应源码」
的主体才需要购买商业许可**。收益来自这里，而不来自「别人一商用就自动付费」——
不存在那样的开源许可证。

### Licensing (English)

HunyuanOS is **dual-licensed**. The public license is **AGPL-3.0-only**
(see [`LICENSE`](LICENSE)): you may use, modify and redistribute it freely as long as you
comply with the AGPL, including its §13 network clause. To embed it in a **closed-source**
product, or to run it as a service **without** releasing the corresponding source, you need a
**commercial license** — see [`COMMERCIAL-LICENSE.md`](COMMERCIAL-LICENSE.md) or contact
1870688451@qq.com. Contributions require signing [`CLA.md`](CLA.md); see
[`CONTRIBUTING.md`](CONTRIBUTING.md).

```
Copyright (C) 2026 史壮壮 <1870688451@qq.com>
```

All rights reserved by the copyright holder.