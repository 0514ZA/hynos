/*
 * HynOS —— 物理页分配器接口（位图法，架构无关）
 *
 * 位图本身放在物理地址 0x200000（内核镜像与引导栈之上），受管内存从 0x400000 起：
 * 低区留给内核镜像、引导栈、位图以及未来的早期固定结构。这样位图不占 .bss，
 * 不参与链接脚本布局，也不会和引导栈抢地址。
 */
#ifndef HYN_PMM_H
#define HYN_PMM_H

#include <hyn/types.h>

#define HYN_PMM_PAGE_SIZE    4096ull
#define HYN_PMM_BITMAP_PHYS  0x200000ull                            /* 位图所在物理地址 */
#define HYN_PMM_BASE         0x400000ull                            /* 受管内存起点 */
#define HYN_PMM_MAX_PHYS     0x100000000ull                         /* 位图覆盖上限：4GiB */
#define HYN_PMM_TOTAL_PAGES  (HYN_PMM_MAX_PHYS / HYN_PMM_PAGE_SIZE) /* 1048576 页 */
#define HYN_PMM_BITMAP_BYTES (HYN_PMM_TOTAL_PAGES / 8u)             /* 128KiB */

/* 标准 PC 的 PCI/MMIO 空洞：引导层补齐完整内存映射前先按此保守跳过 */
#define HYN_PMM_HOLE_START 0xE0000000ull
#define HYN_PMM_HOLE_END   0x100000000ull

/* mem_limit 取自引导信息的内存总容量；可用区间 = 受管区间 - 空洞 - 低区 */
hyn_status_t pmm_init(u64 mem_limit);
int  pmm_ready(void);

/* 分配：返回 4K 对齐的物理页地址（内容已清零）；0 表示失败 */
u64  pmm_alloc_page(void);
u64  pmm_alloc_pages(u32 n);            /* n 页连续物理页 */

/* 释放：返回 1 成功 / 0 拒绝（非受管、未对齐、二次释放） */
int  pmm_free_page(u64 pa);
u32  pmm_free_pages(u64 pa, u32 n);

/* 保留一段物理区间（故障域、早期固定结构用） */
hyn_status_t pmm_reserve(u64 pa, u64 bytes);

int  pmm_owns(u64 pa);
u64  pmm_managed_bytes(void);
u64  pmm_free_bytes(void);
u32  pmm_free_page_count(void);
u32  pmm_total_page_count(void);
u32  pmm_alloc_count(void);

#endif /* HYN_PMM_H */