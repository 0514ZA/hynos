/*
 * 混元 OS —— 硬件总线组实现（ACPI 表发现 + PCI/PCIe 枚举清点）
 *
 * 做的事只有两类，都要求"读数可核对"：
 *   1) 真实扫 ACPI：按 "RSD PTR " 签名在 EBDA 与 0xE0000..0xFFFFF 逐 16 字节找表根，
 *      并做 RSDP 校验和、扩展校验和、逐表校验和核对；
 *   2) 清点 PCI/PCIe：复用 HAL 的一次枚举结果（同一份真值，避免两次扫描结论不一致），
 *      统计设备数与桥数。
 *
 * 不实现：AML 解释器、设备电源管理、热插拔、MSI/MSI-X 配置。故本文件不声称"能驱动设备"。
 * 内存安全前提：ACPI 表地址必须落在当前恒等映射窗口（0..4GiB）内，超出即放弃读取——
 *   内核态硬读未映射地址会直接触发 #PF（那正由故障域接管）。
 */
#include <hyn/bus.h>
#include <hyn/pci.h>
#include <hyn/hal.h>
#include <hyn/vmm.h>
#include <hyn/kstring.h>

#define ACPI_SCAN_LO   0xE0000ull
#define ACPI_SCAN_HI   0x100000ull
#define ACPI_MAX_TBL   64u          /* 逐表校验的防呆上限 */

static hyn_bus_acpi_t g_acpi;
static u32            g_pci_devices;
static u32            g_pci_bridges;

/* 恒等映射窗口内的物理地址读取 */
static u8  rd8(u64 pa)  { return *(volatile u8  *)(uptr)pa; }
static u16 rd16(u64 pa) { return *(volatile u16 *)(uptr)pa; }
static u32 rd32(u64 pa) { return *(volatile u32 *)(uptr)pa; }

static int sig_eq(const char *want, u64 pa, u32 n)
{
    u32 i;

    for (i = 0u; i < n; i++) {
        if ((char)rd8(pa + (u64)i) != want[i]) {
            return 0;
        }
    }
    return 1;
}

/* ACPI 校验和：求和后低 8 位必须为 0 */
static int chksum_zero(u64 pa, u32 len)
{
    u32 i, sum = 0u;

    for (i = 0u; i < len; i++) {
        sum += (u32)rd8(pa + (u64)i);
    }
    return ((sum & 0xFFu) == 0u) ? 1 : 0;
}

/* 找 RSDP：先按 BDA 里的 EBDA 段址扫 1KiB，再扫 0xE0000..0xFFFFF */
static u64 acpi_find_rsdp(void)
{
    u16 seg = rd16(0x40Eu);                 /* BDA: EBDA 段址 */
    u64 base = (u64)seg << 4;
    u64 p;

    if (base >= 0x400ull && (base + 1024ull) <= 0xA0000ull) {
        for (p = base; p < base + 1024ull; p += 16ull) {
            if (sig_eq("RSD PTR ", p, 8u)) {
                return p;
            }
        }
    }
    for (p = ACPI_SCAN_LO; p < ACPI_SCAN_HI; p += 16ull) {
        if (sig_eq("RSD PTR ", p, 8u)) {
            return p;
        }
    }
    return 0ull;
}

static void acpi_scan(void)
{
    u64 rsdp = acpi_find_rsdp();
    u64 addr;
    u32 entry_size;
    u32 tbl_len;
    u32 count, i;

    memset(&g_acpi, 0, (unsigned long)sizeof(g_acpi));
    if (rsdp == 0ull) {
        return;                             /* 扫不到就是扫不到：found = 0 */
    }

    g_acpi.rsdp_found  = 1u;
    g_acpi.revision    = (u32)rd8(rsdp + 15ull);
    g_acpi.checksum_ok = (u32)chksum_zero(rsdp, 20u);
    for (i = 0u; i < 6u; i++) {
        g_acpi.oem_id[i] = (char)rd8(rsdp + 9ull + (u64)i);
    }
    g_acpi.oem_id[6] = '\0';

    /* 表集合地址：版本 >= 2 且给了 XSDT 就优先用 XSDT（64 位） */
    {
        u64 xsdt = 0ull;
        u64 rsdt = (u64)rd32(rsdp + 16ull);

        if (g_acpi.revision >= 2u) {
            u32 rsdp_len = rd32(rsdp + 20ull);
            xsdt = (u64)rd32(rsdp + 24ull) | ((u64)rd32(rsdp + 28ull) << 32);
            if (rsdp_len >= 36u && rsdp_len <= 4096u) {
                g_acpi.ext_ok = (u32)chksum_zero(rsdp, rsdp_len);
            }
        }
        addr       = (xsdt != 0ull) ? xsdt : rsdt;
        entry_size = (xsdt != 0ull) ? 8u : 4u;
    }
    g_acpi.rsdt_address = (u32)(addr & 0xFFFFFFFFull);
    if (addr == 0ull || (addr + 36ull) > HYN_VMM_IDENTITY_LIMIT) {
        return;                             /* 表不在映射窗口：不硬读 */
    }

    tbl_len = rd32(addr + 4ull);
    if (tbl_len < 36u || (addr + (u64)tbl_len) > HYN_VMM_IDENTITY_LIMIT) {
        return;
    }
    count = (tbl_len - 36u) / entry_size;
    if (count > ACPI_MAX_TBL) {
        count = ACPI_MAX_TBL;
    }
    g_acpi.tables = count;

    for (i = 0u; i < count; i++) {
        u64 entry = addr + 36ull + ((u64)i * (u64)entry_size);
        u64 t;
        u32 tl;
        u32 k;

        if ((entry + (u64)entry_size) > HYN_VMM_IDENTITY_LIMIT) {
            break;
        }
        t = (entry_size == 8u)
            ? ((u64)rd32(entry) | ((u64)rd32(entry + 4ull) << 32))
            : (u64)rd32(entry);
        if (t == 0ull || (t + 36ull) > HYN_VMM_IDENTITY_LIMIT) {
            continue;
        }
        tl = rd32(t + 4ull);
        if (tl < 36u || (t + (u64)tl) > HYN_VMM_IDENTITY_LIMIT) {
            continue;
        }
        if (chksum_zero(t, tl) != 0) {
            g_acpi.tables_ok++;
        }
        if (i == 0u) {
            for (k = 0u; k < 4u; k++) {
                g_acpi.first_table[k] = (char)rd8(t + (u64)k);
            }
            g_acpi.first_table[4] = '\0';
        }
    }
}

/* 一次完整总线盘点：PCI 枚举 + ACPI 扫描 */
static void bus_scan_all(void)
{
    const hyn_pci_dev_t *t;
    u32 count = 0u;
    u32 i;

    g_pci_devices = hal_pci_rescan();
    t = hal_pci_table(&count);
    g_pci_bridges = 0u;
    for (i = 0u; i < count; i++) {
        if (t != NULL && t[i].class_code == HYN_PCI_CLASS_BRIDGE) {
            g_pci_bridges++;
        }
    }
    acpi_scan();
}

hyn_status_t bus_init(void)
{
    bus_scan_all();
    return HYN_OK;
}

hyn_status_t bus_reset(void)
{
    bus_scan_all();
    return HYN_OK;
}

hyn_status_t bus_quiesce(void)
{
    memset(&g_acpi, 0, (unsigned long)sizeof(g_acpi));
    g_pci_devices = 0u;
    g_pci_bridges = 0u;
    return HYN_OK;
}

u32 bus_pci_count(void)         { return g_pci_devices; }
u32 bus_pci_bridge_count(void)  { return g_pci_bridges; }
const hyn_bus_acpi_t *bus_acpi(void) { return &g_acpi; }