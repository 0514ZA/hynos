/*
 * HynOS —— PCI/PCIe 配置空间访问（arch 层硬件能力）
 *
 * 为什么放在 arch：x86_64 的配置空间靠 0xCF8/0xCFC 端口机制拿到，是 CPU/平台专属动作；
 * 上层（故障域与 HAL）只消费"看到了什么设备"这类事实，不关心怎么读的。
 *
 * 定位：这是"探测真实硬件"的底座能力。故障域说"存储组已认领 NVMe"、HAL 说"网卡就绪"，
 * 都必须建立在这套枚举结果上——否则任何设备状态都只是内核里的一句声明。
 *
 * 如实标注的局限：
 *   · 只做总线枚举与 BAR 尺寸探测，不做设备功能初始化（NVMe 队列、e1000 收发环等
 *     留待后续里程碑）；
 *   · MMIO 寄存器读取要求地址落在当前恒等映射窗口（0..4GiB）内，超出的如实记为
 *     "窗口未覆盖"，绝不硬读（内核态硬读会直接触发 #PF）。
 */
#ifndef HYN_PCI_H
#define HYN_PCI_H

#include <hyn/types.h>

#define HYN_PCI_CFG_ADDR   0x0CF8u
#define HYN_PCI_CFG_DATA   0x0CFCu

/* 配置空间常用偏移 */
#define HYN_PCI_VENDOR_ID    0x00u
#define HYN_PCI_DEVICE_ID    0x02u
#define HYN_PCI_COMMAND      0x04u
#define HYN_PCI_REVISION     0x08u
#define HYN_PCI_PROG_IF      0x09u
#define HYN_PCI_SUBCLASS     0x0Au
#define HYN_PCI_CLASS        0x0Bu
#define HYN_PCI_HEADER_TYPE  0x0Eu
#define HYN_PCI_BAR0         0x10u
#define HYN_PCI_BAR1         0x14u
#define HYN_PCI_SEC_BUS      0x19u
#define HYN_PCI_INT_LINE     0x3Cu

/* 常用类别（class / subclass） */
#define HYN_PCI_CLASS_STORAGE   0x01u
#define HYN_PCI_SUB_STORAGE_NVM 0x08u      /* NVM 控制器（NVMe） */
#define HYN_PCI_CLASS_NETWORK   0x02u
#define HYN_PCI_SUB_NET_ETHER   0x00u
#define HYN_PCI_CLASS_DISPLAY   0x03u
#define HYN_PCI_CLASS_MULTIMEDIA 0x04u
#define HYN_PCI_SUB_MM_AUDIO    0x03u      /* HD Audio 控制器 */
#define HYN_PCI_CLASS_BRIDGE    0x06u
#define HYN_PCI_CLASS_SERIAL    0x0Cu
#define HYN_PCI_SUB_SERIAL_USB  0x03u

#define HYN_PCI_MAX_DEVICES  24u           /* 枚举结果静态表容量 */
#define HYN_PCI_CMD_MEMORY_SPACE 0x0002u   /* Command.MSE：允许用 BAR 访问设备 */

typedef struct hyn_pci_dev {
    u8  bus;
    u8  dev;
    u8  func;
    u8  header_type;
    u16 vendor;
    u16 device;
    u8  class_code;
    u8  subclass;
    u8  prog_if;
    u8  revision;
    u32 bar0_raw;          /* BAR0 原始值（含标志位） */
    u64 bar0_base;         /* MMIO 基址；64 位 BAR 时把 BAR1 高 32 位拼上 */
    u32 bar0_size;         /* 尺寸探测结果；0 表示未能探测（如全 1 回读） */
    u32 reserved;
} hyn_pci_dev_t;

/* 单次 dword/word/byte 配置空间访问 */
u32  hyn_pci_cfg_read32(u8 bus, u8 dev, u8 func, u8 off);
u16  hyn_pci_cfg_read16(u8 bus, u8 dev, u8 func, u8 off);
u8   hyn_pci_cfg_read8(u8 bus, u8 dev, u8 func, u8 off);
void hyn_pci_cfg_write32(u8 bus, u8 dev, u8 func, u8 off, u32 value);
void hyn_pci_cfg_write16(u8 bus, u8 dev, u8 func, u8 off, u16 value);
void hyn_pci_mem_enable(u8 bus, u8 dev, u8 func);   /* 打开 Command.MSE：MMIO 前必做 */

/* 设备存在性：vendor != 0xFFFF（这是配置空间对"空槽"的约定） */
int  hyn_pci_present(u8 bus, u8 dev, u8 func);

/*
 * 枚举：从 bus 0 起扫全部 dev/func（含多功能设备），并跟随 PCI 桥的下游总线。
 * 返回写入 out 的设备数（不超过 cap）。枚举是只读操作，对设备无副作用。
 */
u32  hyn_pci_enumerate(hyn_pci_dev_t *out, u32 cap);

/* 按类别在已有枚举结果里查找；*from 传 0 起，命中后由函数自增，NULL 表示没有了 */
const hyn_pci_dev_t *hyn_pci_find_class(const hyn_pci_dev_t *table, u32 count,
                                        u8 cls, u8 sub, u32 *from);
u32  hyn_pci_find_class_count(const hyn_pci_dev_t *table, u32 count, u8 cls, u8 sub);

/* BAR 尺寸探测（写全 1 再回读的标准做法，探测后原值已还原） */
u32  hyn_pci_bar_size(u8 bus, u8 dev, u8 func, u8 bar_off, u32 bar_raw);

const char *hyn_pci_class_name(u8 cls, u8 sub);

#endif /* HYN_PCI_H */