/*
 * 混元 OS —— x86_64 分页实现（PML4 四级页表）
 *
 * 职责边界：只做"页表这一件事"——建恒等映射、4K 映射/取消映射、地址翻译、
 * 地址空间切换。页表页从架构无关层传入的分配器回调获取，故本文件不依赖 core/vm。
 *
 * 前提：内核运行在恒等映射下（物理地址 == 内核虚拟地址），因此页表物理页可直接
 * 用 (u64 *)pa 访问；窗口类虚拟地址（如 0xFFFF9000...）只能经 query/map 操作。
 */
#include <hyn/mmu.h>
#include <hyn/arch.h>
#include <hyn/cpu.h>

#define IDX_PML4(va) (((va) >> 39) & 0x1FFull)
#define IDX_PDPT(va) (((va) >> 30) & 0x1FFull)
#define IDX_PD(va)   (((va) >> 21) & 0x1FFull)
#define IDX_PT(va)   (((va) >> 12) & 0x1FFull)

static inline u64 *table_of(u64 pa)
{
    return (u64 *)(uptr)pa;
}

static int table_is_empty(const u64 *t)
{
    u32 i;
    for (i = 0; i < HYN_MMU_TABLE_ENTRIES; i++) {
        if (t[i] != 0ull) {
            return 0;
        }
    }
    return 1;
}

int arch_mmu_is_canonical(u64 va)
{
    u64 top = va >> 48;
    if (((va >> 47) & 1ull) != 0ull) {
        return top == 0xFFFFull;
    }
    return top == 0ull;
}

/* 取得（必要时建立）下一级页表 */
static hyn_status_t child_table(const hyn_mmu_alloc_t *a, u64 *entry, u64 flags, u64 **out)
{
    u64 pa;

    if ((*entry & HYN_MMU_P_PRESENT) != 0ull) {
        if ((*entry & HYN_MMU_P_LARGE) != 0ull) {
            return HYN_EBUSY;               /* 该区间已是大页：本版本不做大页拆分 */
        }
        *entry |= (flags & HYN_MMU_P_USER); /* 上层权限位必须逐级下传 */
        *out = table_of(*entry & HYN_MMU_PA_MASK);
        return HYN_OK;
    }
    if (a == NULL || a->alloc == NULL) {
        return HYN_ENOMEM;
    }
    pa = a->alloc(a->ctx);
    if (pa == 0ull || (pa & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull) {
        return HYN_ENOMEM;
    }
    *entry = (pa & HYN_MMU_PA_MASK) | HYN_MMU_P_PRESENT | HYN_MMU_P_RW |
             (flags & (HYN_MMU_P_USER | HYN_MMU_P_PWT | HYN_MMU_P_PCD));
    *out = table_of(pa);
    return HYN_OK;
}

hyn_status_t arch_mmu_identity_map(const hyn_mmu_alloc_t *a, u64 pml4_pa, u64 limit, u64 flags)
{
    u64 *pml4;
    u64 gib;

    if (pml4_pa == 0ull || limit == 0ull) {
        return HYN_EINVAL;
    }
    limit = HYN_ALIGN_UP(limit, HYN_MMU_LARGE_SIZE);

    pml4 = table_of(pml4_pa);
    /*
     * 必须按标准四级走法摆：PML4 项 -> PDPT 项 -> PD 项（PS=1 的 2MiB 大页）。
     *   · 4GiB 恒等映射落在 PML4[0]（一个 PML4 项覆盖 512GiB），其下的 PDPT 项 0..3
     *     各覆盖 1GiB；
     *   · 早先按"1GiB 粒度占 PML4 槽位"的摆法把 PML4[1..3] 也填了，看似也能翻译
     *     0..1GiB，实际把 PDPT 索引 0..511 全填成了大页——用户地址窗口开在 256GiB
     *     （PML4[0] 下的 PDPT 项 256）会被这些大页提前占住，用户页映射只能撞 EBUSY。
     *     高位地址空间必须保持干净，故这里按标准走法重建。
     */
    for (gib = 0ull; gib < limit; gib += 0x40000000ull) {   /* 每个 PDPT 项覆盖 1GiB */
        u64 *pdpt = NULL;
        u64 *pd = NULL;
        hyn_status_t st;
        u32 i;

        st = child_table(a, &pml4[IDX_PML4(gib)], flags, &pdpt);
        if (st != HYN_OK) {
            return st;
        }
        st = child_table(a, &pdpt[IDX_PDPT(gib)], flags, &pd);
        if (st != HYN_OK) {
            return st;
        }
        for (i = 0; i < HYN_MMU_TABLE_ENTRIES; i++) {
            u64 pa = gib + (u64)i * HYN_MMU_LARGE_SIZE;
            if (pa >= limit) {
                break;
            }
            pd[i] = (pa & HYN_MMU_PA_MASK) | HYN_MMU_P_PRESENT | HYN_MMU_P_RW |
                    HYN_MMU_P_LARGE | (flags & HYN_MMU_P_USER);
        }
    }
    return HYN_OK;
}

hyn_status_t arch_mmu_map(const hyn_mmu_alloc_t *a, u64 pml4_pa, u64 va, u64 pa, u64 flags)
{
    u64 *pml4, *pdpt = NULL, *pd = NULL, *pt = NULL;
    hyn_status_t st;

    if (pml4_pa == 0ull || !arch_mmu_is_canonical(va)) {
        return HYN_EFAULT;
    }
    if ((va & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull ||
        (pa & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull) {
        return HYN_EINVAL;
    }

    pml4 = table_of(pml4_pa);
    st = child_table(a, &pml4[IDX_PML4(va)], flags, &pdpt);
    if (st != HYN_OK) {
        return st;
    }
    st = child_table(a, &pdpt[IDX_PDPT(va)], flags, &pd);
    if (st != HYN_OK) {
        return st;
    }
    st = child_table(a, &pd[IDX_PD(va)], flags, &pt);
    if (st != HYN_OK) {
        return st;
    }
    if ((pt[IDX_PT(va)] & HYN_MMU_P_PRESENT) != 0ull) {
        return HYN_EBUSY;                   /* 已映射：拒绝覆盖，避免静默丢页 */
    }
    pt[IDX_PT(va)] = (pa & HYN_MMU_PA_MASK) | flags;
    arch_mmu_flush(va);
    return HYN_OK;
}

hyn_status_t arch_mmu_unmap(const hyn_mmu_alloc_t *a, u64 pml4_pa, u64 va)
{
    u64 *pml4, *pdpt, *pd, *pt;
    u64 e, pa;

    if (pml4_pa == 0ull || !arch_mmu_is_canonical(va)) {
        return HYN_EFAULT;
    }
    pml4 = table_of(pml4_pa);
    e = pml4[IDX_PML4(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull) {
        return HYN_ENOENT;
    }
    pdpt = table_of(e & HYN_MMU_PA_MASK);
    e = pdpt[IDX_PDPT(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull || (e & HYN_MMU_P_LARGE) != 0ull) {
        return HYN_ENOENT;
    }
    pd = table_of(e & HYN_MMU_PA_MASK);
    e = pd[IDX_PD(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull) {
        return HYN_ENOENT;
    }
    if ((e & HYN_MMU_P_LARGE) != 0ull) {
        pd[IDX_PD(va)] = 0ull;              /* 取消 2MiB 大页映射 */
        arch_mmu_flush(va);
        return HYN_OK;
    }
    pt = table_of(e & HYN_MMU_PA_MASK);
    if ((pt[IDX_PT(va)] & HYN_MMU_P_PRESENT) == 0ull) {
        return HYN_ENOENT;
    }
    pt[IDX_PT(va)] = 0ull;
    arch_mmu_flush(va);

    /* 自底向上回收空页表：页表页不是数据页，留着会白占物理内存 */
    if (table_is_empty(pt) && a != NULL && a->release != NULL) {
        pa = pd[IDX_PD(va)] & HYN_MMU_PA_MASK;
        pd[IDX_PD(va)] = 0ull;
        a->release(pa, a->ctx);
        if (table_is_empty(pd)) {
            pa = pdpt[IDX_PDPT(va)] & HYN_MMU_PA_MASK;
            pdpt[IDX_PDPT(va)] = 0ull;
            a->release(pa, a->ctx);
        }
    }
    return HYN_OK;
}

u64 arch_mmu_query(u64 pml4_pa, u64 va)
{
    u64 *pml4, *pdpt, *pd, *pt;
    u64 e, base;

    if (pml4_pa == 0ull || !arch_mmu_is_canonical(va)) {
        return 0ull;
    }
    pml4 = table_of(pml4_pa);
    e = pml4[IDX_PML4(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull) {
        return 0ull;
    }
    pdpt = table_of(e & HYN_MMU_PA_MASK);
    e = pdpt[IDX_PDPT(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull) {
        return 0ull;
    }
    if ((e & HYN_MMU_P_LARGE) != 0ull) {    /* 1GiB 页（本内核不建，但翻译要正确） */
        base = e & HYN_MMU_PA_MASK;
        return base | (va & 0x3FFFFFFFull);
    }
    pd = table_of(e & HYN_MMU_PA_MASK);
    e = pd[IDX_PD(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull) {
        return 0ull;
    }
    if ((e & HYN_MMU_P_LARGE) != 0ull) {    /* 2MiB 大页 */
        base = e & HYN_MMU_PA_MASK;
        return base | (va & (HYN_MMU_LARGE_SIZE - 1ull));
    }
    pt = table_of(e & HYN_MMU_PA_MASK);
    e = pt[IDX_PT(va)];
    if ((e & HYN_MMU_P_PRESENT) == 0ull) {
        return 0ull;
    }
    return (e & HYN_MMU_PA_MASK) | (va & (HYN_MMU_PAGE_SIZE - 1ull));
}

void arch_mmu_switch(u64 pml4_pa)
{
    hyn_write_cr3((uptr)pml4_pa);
}

u64 arch_mmu_current_root(void)
{
    return (u64)(uptr)hyn_read_cr3() & HYN_MMU_PA_MASK;
}

void arch_mmu_flush(u64 va)
{
    __asm__ __volatile__("invlpg (%0)" :: "r"((uptr)va) : "memory");
}