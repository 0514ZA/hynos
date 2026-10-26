/*
 * HynOS —— 全局描述符表（GDT）接口
 *
 * 表项布局（与 include/hyn/cpu.h 的选择子严格对应）：
 *   0x00 空 | 0x08 32 位内核代码 | 0x10 内核数据 | 0x18 64 位内核代码
 *   0x20 用户数据(RPL3) | 0x28 64 位用户代码(RPL3) | 0x30 TSS（占 2 项）
 */
#ifndef HYN_GDT_H
#define HYN_GDT_H

#include <hyn/types.h>

#define HYN_GDT_ENTRIES 16

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) hyn_gdtr_t;

/* 构造一个 8 字节长描述符（64 位模式下 base 通常为 0） */
u64 hyn_gdt_make_desc(u32 base, u32 limit, u8 access, u8 flags);

void hyn_gdt_init(void);
void hyn_gdt_set_entry(u32 index, u64 raw);
u64  hyn_gdt_get_entry(u32 index);

#endif /* HYN_GDT_H */