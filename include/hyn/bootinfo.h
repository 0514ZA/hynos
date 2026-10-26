/*
 * 混元 OS —— 引导信息结构
 *
 * 由自研引导扇区在进入长模式前写入物理地址 0x6000，并以指针传给 kernel_main。
 * 该结构是内核了解"机器有多大、内核镜像多大"的唯一早期来源。
 */
#ifndef HYN_BOOTINFO_H
#define HYN_BOOTINFO_H

#include <hyn/types.h>

#define HYN_BOOTINFO_MAGIC 0x48594E31u   /* 'HYN1' */
#define HYN_BOOTINFO_ADDR  0x6000u

/* 引导方式：两条引导路径写同一结构的同一字段，内核据此区分来源 */
#define HYN_BOOT_METHOD_BIOS 0u          /* 自研 MBR 引导代码（EDD 读盘） */
#define HYN_BOOT_METHOD_UEFI 1u          /* 固件加载 BOOTX64.EFI（内嵌负载） */

typedef struct boot_info {
    u32 magic;              /* HYN_BOOTINFO_MAGIC */
    u32 total_mem_bytes;    /* 物理内存总容量（BIOS: E801 推算；UEFI: 内存映射推算） */
    u32 kernel_bytes;       /* 内核镜像字节数 */
    u32 kernel_sectors;     /* 内核镜像占用扇区数（UEFI 路径无扇区概念，写 0） */
    u32 boot_method;        /* HYN_BOOT_METHOD_* */
    u32 reserved0;          /* 保持 8 字节对齐，后续扩展用（两条路径都写 0） */
} boot_info_t;

#endif /* HYN_BOOTINFO_H */