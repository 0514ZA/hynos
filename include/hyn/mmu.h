/*
 * HynOS —— MMU（分页）架构接口
 *
 * 页表格式是 CPU 专属能力：x86_64 用 PML4 四级页表，未来 aarch64/riscv64 各有
 * 自己的一套。所以本头文件只声明"上层需要哪些分页能力"，实现在 arch/<arch>/mmu.c，
 * 架构无关层（core/vm/vmm.c）只通过这里的接口操作页表，不直接碰页表内存布局。
 *
 * 约定：物理页 0 不作为数据页使用，因此"翻译返回 0"统一表示"未映射"。
 */
#ifndef HYN_MMU_H
#define HYN_MMU_H

#include <hyn/types.h>

#define HYN_MMU_PAGE_SIZE     4096ull        /* 基础页 4KiB */
#define HYN_MMU_LARGE_SIZE    0x200000ull    /* x86_64 2MiB 大页（x86_64 专属粒度） */
#define HYN_MMU_TABLE_ENTRIES 512u           /* 每级页表 512 项 */

/* 页表项权限位（x86_64 语义；其它架构按同名语义自行翻译） */
#define HYN_MMU_P_PRESENT  (1ull << 0)
#define HYN_MMU_P_RW       (1ull << 1)
#define HYN_MMU_P_USER     (1ull << 2)
#define HYN_MMU_P_PWT      (1ull << 3)
#define HYN_MMU_P_PCD      (1ull << 4)
#define HYN_MMU_P_ACCESSED (1ull << 5)
#define HYN_MMU_P_DIRTY    (1ull << 6)
#define HYN_MMU_P_LARGE    (1ull << 7)
#define HYN_MMU_P_GLOBAL   (1ull << 8)
#define HYN_MMU_P_NX       (1ull << 63)

#define HYN_MMU_PA_MASK    0x000FFFFFFFFFF000ull

/* 常用组合 */
#define HYN_MMU_FLAG_KERNEL_RO (HYN_MMU_P_PRESENT)
#define HYN_MMU_FLAG_KERNEL_RW (HYN_MMU_P_PRESENT | HYN_MMU_P_RW)
#define HYN_MMU_FLAG_USER_RW   (HYN_MMU_P_PRESENT | HYN_MMU_P_RW | HYN_MMU_P_USER)

/*
 * 设备 MMIO 窗口：必须是不可缓存的（PWT+PCD 两条都要，单靠 MTRR 不够，
 * 因为设备寄存器没有"缓存一致性"这回事——缓存住就等于读到陈旧值）。
 */
#define HYN_MMU_FLAG_MMIO_RW   (HYN_MMU_P_PRESENT | HYN_MMU_P_RW | HYN_MMU_P_PWT | HYN_MMU_P_PCD)

/*
 * 页表页由架构无关层提供：架构层不依赖 core/vm 的分配器，只持有回调，
 * 这样"页表页从哪来"这一策略留在 core 层。
 */
typedef struct hyn_mmu_alloc {
    u64  (*alloc)(void *ctx);            /* 返回 4K 对齐的物理页；0 表示失败 */
    void (*release)(u64 pa, void *ctx);  /* 回收页表页；可为 NULL（表示不回收） */
    void *ctx;
} hyn_mmu_alloc_t;

/* 建立 [0, limit) 的恒等映射（大页粒度），供内核"物理地址即虚拟地址"使用 */
hyn_status_t arch_mmu_identity_map(const hyn_mmu_alloc_t *a, u64 pml4_pa, u64 limit, u64 flags);

/* 4K 粒度映射 / 取消映射（必要时按需分配中间页表） */
hyn_status_t arch_mmu_map(const hyn_mmu_alloc_t *a, u64 pml4_pa, u64 va, u64 pa, u64 flags);
hyn_status_t arch_mmu_unmap(const hyn_mmu_alloc_t *a, u64 pml4_pa, u64 va);

/* 地址翻译：返回物理地址；0 表示未映射（含大页拆分后的页内偏移） */
u64 arch_mmu_query(u64 pml4_pa, u64 va);

/* 地址空间切换与 TLB 维护 */
void arch_mmu_switch(u64 pml4_pa);
u64  arch_mmu_current_root(void);
void arch_mmu_flush(u64 va);

/* 规范地址判定（x86_64 48 位） */
int arch_mmu_is_canonical(u64 va);

#endif /* HYN_MMU_H */