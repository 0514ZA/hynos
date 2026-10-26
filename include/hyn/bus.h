/*
 * 混元 OS —— 硬件总线组接口（ACPI、PCIe、基础硬件管理）
 *
 * 归属：快通道分组故障域 · 硬件总线组。
 * 分层纪律：ACPI/PCI 这类"总线级"代码不进硬核底座，也不进 arch（arch 只提供
 *   x86_64 的配置空间读写原语）；总线组的任务是把这些原语变成"本机组装了什么"。
 *
 * 诚实性说明：
 *   · PCI 设备数是真实枚举结果（HAL 共享枚举表），不是常量；
 *   · ACPI RSDP 是从内存里按签名逐 16 字节扫出来的，并做校验和核对；
 *   · 扫不到就如实记 found = 0，不伪造"找到 ACPI 表"。
 */
#ifndef HYN_BUS_H
#define HYN_BUS_H

#include <hyn/types.h>

/* ACPI 取证：RSDP 与表集合的真实读数 */
typedef struct hyn_bus_acpi {
    u32  rsdp_found;         /* 1 = 在 EBDA 或 0xE0000..0xFFFFF 扫到 "RSD PTR " */
    u32  checksum_ok;        /* RSDP 前 20 字节校验和为 0 */
    u32  ext_ok;             /* 版本 >= 2 时：前 length 字节校验和为 0 */
    u32  revision;
    u32  tables;             /* RSDT/XSDT 表项数 */
    u32  tables_ok;          /* 逐表校验和通过的条目数 */
    u32  rsdt_address;       /* 表集合物理地址（XSDT 存在时取 XSDT） */
    char oem_id[8];          /* 6 字节 + 结尾 NUL */
    char first_table[6];     /* 首张表签名（4 字节）+ NUL */
} hyn_bus_acpi_t;

/* 生命周期（由硬件总线组故障域的 ops 调用） */
hyn_status_t bus_init(void);
hyn_status_t bus_reset(void);      /* 重新枚举 PCI + 重新扫 ACPI */
hyn_status_t bus_quiesce(void);    /* 本组状态归零 */

/* 取证访问器 */
u32 bus_pci_count(void);
u32 bus_pci_bridge_count(void);
const hyn_bus_acpi_t *bus_acpi(void);

#endif /* HYN_BUS_H */