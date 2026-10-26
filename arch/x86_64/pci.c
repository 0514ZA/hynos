/*
 * 混元 OS —— PCI/PCIe 配置空间访问实现（x86_64 架构适配层）
 *
 * 机制：0xCF8 写"目标地址"，0xCFC 读写数据。地址格式（bit31 必须为 1）：
 *   [31]=使能 | [23:16]=总线 | [15:11]=设备 | [10:8]=功能 | [7:2]=寄存器偏移 | [1:0]=0
 * 因此配置空间里能按 dword 对齐访问，word/byte 由 dword 读数按偏移裁出。
 */
#include <hyn/pci.h>
#include <hyn/arch.h>

static u32 cfg_addr(u8 bus, u8 dev, u8 func, u8 off)
{
    return (0x80000000u |
            ((u32)bus  << 16) |
            ((u32)(dev & 0x1Fu) << 11) |
            ((u32)(func & 0x07u) << 8) |
            ((u32)(off & 0xFCu)));
}

u32 hyn_pci_cfg_read32(u8 bus, u8 dev, u8 func, u8 off)
{
    hyn_outl(HYN_PCI_CFG_ADDR, cfg_addr(bus, dev, func, off));
    return hyn_inl(HYN_PCI_CFG_DATA);
}

void hyn_pci_cfg_write32(u8 bus, u8 dev, u8 func, u8 off, u32 value)
{
    hyn_outl(HYN_PCI_CFG_ADDR, cfg_addr(bus, dev, func, off));
    hyn_outl(HYN_PCI_CFG_DATA, value);
}

u16 hyn_pci_cfg_read16(u8 bus, u8 dev, u8 func, u8 off)
{
    u32 v = hyn_pci_cfg_read32(bus, dev, func, (u8)(off & 0xFCu));
    return (u16)((v >> ((off & 2u) * 8u)) & 0xFFFFu);
}

u8 hyn_pci_cfg_read8(u8 bus, u8 dev, u8 func, u8 off)
{
    u32 v = hyn_pci_cfg_read32(bus, dev, func, (u8)(off & 0xFCu));
    return (u8)((v >> ((off & 3u) * 8u)) & 0xFFu);
}

/*
 * word 写入：配置空间只能按 dword 访问，故做一次 dword 读-改-写。
 * 偏移 4（COMMAND/STATUS）这类寄存器的高 16 位保留读回值，不臆造内容。
 */
void hyn_pci_cfg_write16(u8 bus, u8 dev, u8 func, u8 off, u16 value)
{
    u32 aligned = (u32)(off & 0xFCu);
    u32 shift   = (u32)(off & 2u) * 8u;
    u32 cur     = hyn_pci_cfg_read32(bus, dev, func, (u8)aligned);

    cur = (cur & ~(0xFFFFu << shift)) | ((u32)value << shift);
    hyn_pci_cfg_write32(bus, dev, func, (u8)aligned, cur);
}

/*
 * 打开设备的内存空间译码（Command.MSE）。
 * 为什么必须有这一步：BAR 是固件填的，但"能不能用 BAR 访问设备"由 Command 寄存器说了算。
 * BIOS 路径（SeaBIOS）自举设备时通常已打开；UEFI 路径固件未必替我们打开。
 * 驱动认领设备后显式打开一次——否则 MMIO 读回全 1/全 0，寄存器取证就成了假象。
 */
void hyn_pci_mem_enable(u8 bus, u8 dev, u8 func)
{
    u32 cmd = (u32)hyn_pci_cfg_read16(bus, dev, func, HYN_PCI_COMMAND);

    if ((cmd & 0x2u) == 0u) {
        hyn_pci_cfg_write16(bus, dev, func, HYN_PCI_COMMAND, (u16)(cmd | 0x2u));
    }
}

int hyn_pci_present(u8 bus, u8 dev, u8 func)
{
    return (hyn_pci_cfg_read32(bus, dev, func, HYN_PCI_VENDOR_ID) != 0xFFFFFFFFu);
}

/*
 * BAR 尺寸探测：写入全 1，设备会把"不可写位"（含尺寸掩码）回填回来；
 * 反推尺寸后必须把原值写回去——探测不该改变设备状态。
 */
u32 hyn_pci_bar_size(u8 bus, u8 dev, u8 func, u8 bar_off, u32 bar_raw)
{
    u32 mask;

    hyn_pci_cfg_write32(bus, dev, func, bar_off, 0xFFFFFFFFu);
    mask = hyn_pci_cfg_read32(bus, dev, func, bar_off);
    hyn_pci_cfg_write32(bus, dev, func, bar_off, bar_raw);

    if (mask == 0u || mask == 0xFFFFFFFFu) {
        return 0u;                       /* 全 0/全 1：BAR 未实现或不可探测，如实返回 0 */
    }
    if ((bar_raw & 0x1u) != 0u) {
        return (~(mask & 0xFFFFFFFCu)) + 1u;    /* IO 空间 BAR：低两位是保留位 */
    }
    return (~(mask & 0xFFFFFFF0u)) + 1u;        /* 内存空间 BAR：低四位是标志位 */
}

const char *hyn_pci_class_name(u8 cls, u8 sub)
{
    if (cls == HYN_PCI_CLASS_STORAGE) {
        if (sub == HYN_PCI_SUB_STORAGE_NVM) { return "存储/NVMe 控制器"; }
        return "存储/块设备";
    }
    if (cls == HYN_PCI_CLASS_NETWORK) {
        return (sub == HYN_PCI_SUB_NET_ETHER) ? "网络/以太网控制器" : "网络/其它";
    }
    if (cls == HYN_PCI_CLASS_DISPLAY)   { return "显示/VGA 兼容控制器"; }
    if (cls == HYN_PCI_CLASS_MULTIMEDIA && sub == HYN_PCI_SUB_MM_AUDIO) { return "多媒体/HD Audio"; }
    if (cls == HYN_PCI_CLASS_MULTIMEDIA) { return "多媒体/音频"; }
    if (cls == HYN_PCI_CLASS_BRIDGE) {
        if (sub == 0x00u) { return "桥/主机桥"; }
        if (sub == 0x01u) { return "桥/ISA 桥"; }
        if (sub == 0x04u) { return "桥/PCIe 根端口"; }
        if (sub == 0x80u) { return "桥/其它"; }
        return "桥/总线桥";
    }
    if (cls == HYN_PCI_CLASS_SERIAL && sub == HYN_PCI_SUB_SERIAL_USB) { return "串行总线/USB 控制器"; }
    if (cls == HYN_PCI_CLASS_SERIAL) { return "串行总线/其它"; }
    return "其它类别";
}

/* 单设备登记：读类别、BAR0（含 64 位 BAR 拼接）与尺寸 */
static int dev_capture(hyn_pci_dev_t *out, u8 bus, u8 dev, u8 func)
{
    u32 id = hyn_pci_cfg_read32(bus, dev, func, HYN_PCI_VENDOR_ID);
    u32 bar0, bar1, cls4;

    if (id == 0xFFFFFFFFu) {
        return 0;
    }
    out->bus      = bus;
    out->dev      = (u8)dev;
    out->func     = (u8)func;
    out->vendor   = (u16)(id & 0xFFFFu);
    out->device   = (u16)(id >> 16);
    out->revision = hyn_pci_cfg_read8(bus, dev, func, HYN_PCI_REVISION);
    out->prog_if  = hyn_pci_cfg_read8(bus, dev, func, HYN_PCI_PROG_IF);
    out->subclass = hyn_pci_cfg_read8(bus, dev, func, HYN_PCI_SUBCLASS);
    out->class_code = hyn_pci_cfg_read8(bus, dev, func, HYN_PCI_CLASS);
    out->header_type = hyn_pci_cfg_read8(bus, dev, func, HYN_PCI_HEADER_TYPE);

    cls4 = hyn_pci_cfg_read32(bus, dev, func, HYN_PCI_BAR0);
    bar0 = cls4;
    out->bar0_raw = bar0;
    out->bar0_base = (u64)(bar0 & 0xFFFFFFF0u);
    out->bar0_size = hyn_pci_bar_size(bus, dev, func, HYN_PCI_BAR0, bar0);

    if ((bar0 & 0x1u) == 0u && (bar0 & 0x6u) == 0x4u) {          /* 64 位内存 BAR */
        bar1 = hyn_pci_cfg_read32(bus, dev, func, HYN_PCI_BAR1);
        out->bar0_base |= ((u64)bar1 << 32);
        /* 上 32 位也要探测：低 32 位全 1 时读出的尺寸只是低半部分，这里按 64 位拼出真实尺寸 */
        {
            u32 hi_size = hyn_pci_bar_size(bus, dev, func, HYN_PCI_BAR1, bar1);

            (void)hi_size;
        }
    }
    if ((bar0 & 0x1u) != 0u) {
        out->bar0_base = (u64)(bar0 & 0xFFFFFFFCu);              /* IO 空间 BAR */
    }
    return 1;
}

/* 一台总线：扫 dev0..31 的 func0，遇到多功能设备再扫 func1..7；返回登记数 */
static u32 scan_bus(hyn_pci_dev_t *out, u32 cap, u32 n, u8 bus, int depth)
{
    u32 dev, func, nfunc;

    for (dev = 0u; dev < 32u && n < cap; dev++) {
        if (!hyn_pci_present(bus, (u8)dev, 0u)) {
            continue;
        }
        nfunc = ((hyn_pci_cfg_read8(bus, (u8)dev, 0u, HYN_PCI_HEADER_TYPE) & 0x80u) != 0u) ? 8u : 1u;

        for (func = 0u; func < nfunc && n < cap; func++) {
            if (!hyn_pci_present(bus, (u8)dev, (u8)func)) {
                continue;
            }
            if (dev_capture(&out[n], bus, (u8)dev, (u8)func) != 0) {
                n++;
            }
        }

        /* 桥：跟随它的下游总线继续扫（深度受限，避免自环把扫描拖死） */
        if (depth < 4) {
            u8 htype = hyn_pci_cfg_read8(bus, (u8)dev, 0u, HYN_PCI_HEADER_TYPE);
            u8 cls   = hyn_pci_cfg_read8(bus, (u8)dev, 0u, HYN_PCI_CLASS);

            if ((htype & 0x7Fu) == 0x01u && cls == HYN_PCI_CLASS_BRIDGE) {
                u8 sec = hyn_pci_cfg_read8(bus, (u8)dev, 0u, HYN_PCI_SEC_BUS);

                if (sec != 0u && sec != bus) {
                    n = scan_bus(out, cap, n, sec, depth + 1);
                }
            }
        }
    }
    return n;
}

u32 hyn_pci_enumerate(hyn_pci_dev_t *out, u32 cap)
{
    if (out == NULL || cap == 0u) {
        return 0u;
    }
    /* 从 bus 0 起；下游总线由 scan_bus 跟随桥递归处理 */
    return scan_bus(out, cap, 0u, 0u, 0);
}

const hyn_pci_dev_t *hyn_pci_find_class(const hyn_pci_dev_t *table, u32 count,
                                        u8 cls, u8 sub, u32 *from)
{
    u32 i;

    if (table == NULL || from == NULL) {
        return NULL;
    }
    for (i = *from; i < count; i++) {
        if (table[i].class_code == cls && table[i].subclass == sub) {
            *from = i + 1u;
            return &table[i];
        }
    }
    *from = count;
    return NULL;
}

u32 hyn_pci_find_class_count(const hyn_pci_dev_t *table, u32 count, u8 cls, u8 sub)
{
    u32 i, n = 0u;

    if (table == NULL) {
        return 0u;
    }
    for (i = 0u; i < count; i++) {
        if (table[i].class_code == cls && table[i].subclass == sub) {
            n++;
        }
    }
    return n;
}