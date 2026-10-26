/*
 * HynOS —— 虚拟内存管理器（纯 C，架构无关）
 *
 * 本文件只决定"映射什么、映射到哪里"，具体页表摆放交给 arch_mmu_*。
 *
 * 初始化流程（vmm_init）：
 *   1) 从物理页分配器取一页作为新的 PML4；
 *   2) 用 2MiB 大页建立 0..4GiB 恒等映射（与引导期布局一致，保证切换 CR3 后
 *      内核代码、数据、引导栈、串口/VGA 的 MMIO 全部依旧可达）；
 *   3) arch_mmu_switch 切换地址空间；
 *   4) 切换后立即自证：CR3 已是新根、内核自身地址恒等可达、窗口尚未映射。
 *      任何一条不成立就返回错误，由 kmain 停机，绝不带着可疑页表继续跑。
 */
#include <hyn/vmm.h>
#include <hyn/mmu.h>
#include <hyn/pmm.h>
#include <hyn/kstring.h>
#include <hyn/ring3.h>                   /* 用户地址窗口：HYN_USER_BASE / HYN_USER_TOP */

static u64 g_root;                              /* 内核 PML4 物理地址 */
static u64 g_window_pa[HYN_VMM_WINDOW_PAGES];   /* 窗口槽位 => 物理页 */
static u8  g_window_used[HYN_VMM_WINDOW_PAGES];
static u32 g_window_count;
static u32 g_map_count;
static u32 g_user_pages;
static u64 g_user_next = HYN_USER_BASE;         /* 用户页顺序分配游标 */
static int g_ready;

static u64 frame_alloc(void *ctx)
{
    HYN_UNUSED(ctx);
    return pmm_alloc_page();
}

static void frame_release(u64 pa, void *ctx)
{
    HYN_UNUSED(ctx);
    (void)pmm_free_page(pa);
}

static const hyn_mmu_alloc_t g_frame_alloc = { frame_alloc, frame_release, NULL };

hyn_status_t vmm_init(void)
{
    hyn_status_t st;

    if (!pmm_ready()) {
        return HYN_EINVAL;
    }
    g_root = pmm_alloc_page();
    if (g_root == 0ull) {
        return HYN_ENOMEM;
    }
    st = arch_mmu_identity_map(&g_frame_alloc, g_root, HYN_VMM_IDENTITY_LIMIT,
                               HYN_MMU_FLAG_KERNEL_RW);
    if (st != HYN_OK) {
        return st;
    }

    arch_mmu_switch(g_root);

    if (arch_mmu_current_root() != g_root) {
        return HYN_EFAULT;                  /* CR3 没换成新根 */
    }
    if (arch_mmu_query(g_root, (u64)(uptr)(const void *)&g_root) == 0ull) {
        return HYN_EFAULT;                  /* 内核数据不再恒等可达 */
    }
    if (arch_mmu_query(g_root, HYN_VMM_WINDOW_BASE) != 0ull) {
        return HYN_EFAULT;                  /* 窗口理应尚未映射 */
    }

    g_ready = 1;
    return HYN_OK;
}

int vmm_ready(void)
{
    return g_ready;
}

u64 vmm_kernel_root(void)
{
    return g_root;
}

u64 vmm_translate(u64 va)
{
    return g_ready ? arch_mmu_query(g_root, va) : 0ull;
}

static u32 window_slot(u64 va)
{
    u64 off;

    if (va < HYN_VMM_WINDOW_BASE) {
        return HYN_VMM_WINDOW_PAGES;
    }
    off = va - HYN_VMM_WINDOW_BASE;
    if ((off & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull) {
        return HYN_VMM_WINDOW_PAGES;
    }
    off /= HYN_MMU_PAGE_SIZE;
    if (off >= HYN_VMM_WINDOW_PAGES) {
        return HYN_VMM_WINDOW_PAGES;
    }
    return (u32)off;
}

hyn_status_t vmm_map_window(u64 *out_va, u64 pa)
{
    u32 i;
    hyn_status_t st;

    if (!g_ready || out_va == NULL || pa == 0ull) {
        return HYN_EINVAL;
    }
    if ((pa & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull) {
        return HYN_EINVAL;
    }
    for (i = 0; i < HYN_VMM_WINDOW_PAGES; i++) {
        if (!g_window_used[i]) {
            break;
        }
    }
    if (i >= HYN_VMM_WINDOW_PAGES) {
        return HYN_ENOMEM;
    }

    st = arch_mmu_map(&g_frame_alloc, g_root,
                      HYN_VMM_WINDOW_BASE + (u64)i * HYN_MMU_PAGE_SIZE,
                      pa, HYN_MMU_FLAG_KERNEL_RW);
    if (st != HYN_OK) {
        return st;
    }

    g_window_used[i] = 1u;
    g_window_pa[i] = pa;
    g_window_count++;
    g_map_count++;
    *out_va = HYN_VMM_WINDOW_BASE + (u64)i * HYN_MMU_PAGE_SIZE;
    return HYN_OK;
}

hyn_status_t vmm_alloc_window(u64 *out_va)
{
    u64 pa;
    hyn_status_t st;

    if (!g_ready || out_va == NULL) {
        return HYN_EINVAL;
    }
    pa = pmm_alloc_page();
    if (pa == 0ull) {
        return HYN_ENOMEM;
    }
    st = vmm_map_window(out_va, pa);
    if (st != HYN_OK) {
        (void)pmm_free_page(pa);
        return st;
    }
    return HYN_OK;
}

hyn_status_t vmm_free_window(u64 va)
{
    u32 i = window_slot(va);
    u64 pa;

    if (!g_ready || i >= HYN_VMM_WINDOW_PAGES || g_window_used[i] == 0u) {
        return HYN_EINVAL;
    }
    if (arch_mmu_unmap(&g_frame_alloc, g_root, va) != HYN_OK) {
        return HYN_EFAULT;
    }
    pa = g_window_pa[i];
    g_window_used[i] = 0u;
    g_window_pa[i] = 0ull;
    if (g_window_count > 0u) {
        g_window_count--;
    }
    if (g_map_count > 0u) {
        g_map_count--;
    }
    if (pa != 0ull) {
        (void)pmm_free_page(pa);
    }
    return HYN_OK;
}

hyn_status_t vmm_map_page(u64 va, u64 pa, u64 flags)
{
    hyn_status_t st;

    if (!g_ready) {
        return HYN_EINVAL;
    }
    /* 恒等映射区已被 2MiB 大页覆盖：在里面塞 4K 页需要做大页拆分，本里程碑不做 */
    if (va < HYN_VMM_IDENTITY_LIMIT) {
        return HYN_EBUSY;
    }
    if (vmm_translate(va) != 0ull) {
        return HYN_EBUSY;
    }
    st = arch_mmu_map(&g_frame_alloc, g_root, va, pa, flags);
    if (st == HYN_OK) {
        g_map_count++;
    }
    return st;
}

hyn_status_t vmm_unmap_page(u64 va)
{
    hyn_status_t st = arch_mmu_unmap(&g_frame_alloc, g_root, va);

    if (st == HYN_OK && g_map_count > 0u) {
        g_map_count--;
    }
    return st;
}

/*
 * 设备 MMIO 窗口补映射（存在的理由见 vmm.h 注释）。
 *
 * 逐页判断而不是整段判断，是因为"已覆盖"和"需补映射"可能混在同一窗口里：
 *   低 4GiB 内的 BAR 已被 2MiB 大页恒等映射覆盖 → 查表命中，直接跳过、不动页表；
 *   4GiB 以上的 64 位 BAR（OVMF 会把 NVMe 放这儿）没有任何映射 → 建 4K 页补上。
 * 恒等区内若查表未命中，说明布局与预期不符：如实返回错误，不去拆大页硬塞。
 */
hyn_status_t vmm_map_mmio(u64 pa, u64 size)
{
    u64 off = 0ull;
    hyn_status_t st;

    if (!g_ready || size == 0ull) {
        return HYN_EINVAL;
    }
    if (pa + size < pa) {                    /* 回绕：这个窗口根本不存在 */
        return HYN_EINVAL;
    }

    for (off = 0ull; off < size; off += HYN_MMU_PAGE_SIZE) {
        u64 va = pa + off;

        if (vmm_translate(va) == va) {
            continue;                        /* 已同址可访问：不动页表 */
        }
        if (va < HYN_VMM_IDENTITY_LIMIT) {
            return HYN_EBUSY;                /* 恒等区内未命中：不拆大页，如实拒绝 */
        }
        st = vmm_map_page(va, pa + off, HYN_MMU_FLAG_MMIO_RW);
        if (st != HYN_OK) {
            return st;
        }
    }

    /* 逐页复核：返回 OK 只代表"真的能按物理地址访问"，而不是"尝试过" */
    for (off = 0ull; off < size; off += HYN_MMU_PAGE_SIZE) {
        if (vmm_translate(pa + off) != pa + off) {
            return HYN_EFAULT;
        }
    }
    return HYN_OK;
}

/*
 * 用户页分配：只在用户窗口内顺序推进游标，绝不落到内核恒等映射区。
 * 页面内容由 pmm_alloc_page 保证清零——Ring3 拿到的第一手内存没有上一任数据。
 */
hyn_status_t vmm_alloc_user_page(u64 *out_va)
{
    u64 va, pa;
    hyn_status_t st;

    if (!g_ready || out_va == NULL) {
        return HYN_EINVAL;
    }
    if (g_user_next + HYN_MMU_PAGE_SIZE > HYN_USER_TOP) {
        return HYN_ENOMEM;                  /* 用户窗口用尽：如实报错，不越界映射 */
    }
    va = g_user_next;
    pa = pmm_alloc_page();
    if (pa == 0ull) {
        return HYN_ENOMEM;
    }
    st = vmm_map_page(va, pa, HYN_MMU_FLAG_USER_RW);
    if (st != HYN_OK) {
        (void)pmm_free_page(pa);
        return st;
    }
    g_user_next = va + HYN_MMU_PAGE_SIZE;
    g_user_pages++;
    *out_va = va;
    return HYN_OK;
}

/* 回收：先按 VA 取出物理页，再取消映射、归还物理页；游标不回退（避免与在用页撞车） */
hyn_status_t vmm_free_user_page(u64 va)
{
    u64 pa;

    if (!g_ready || va < HYN_USER_BASE || va >= HYN_USER_TOP ||
        (va & (HYN_MMU_PAGE_SIZE - 1ull)) != 0ull) {
        return HYN_EINVAL;
    }
    pa = vmm_translate(va);
    if (pa == 0ull) {
        return HYN_EINVAL;
    }
    if (vmm_unmap_page(va) != HYN_OK) {
        return HYN_EFAULT;
    }
    (void)pmm_free_page(pa);
    if (g_user_pages > 0u) {
        g_user_pages--;
    }
    return HYN_OK;
}

u32 vmm_user_page_count(void)
{
    return g_user_pages;
}

u32 vmm_window_used(void)
{
    return g_window_count;
}

u32 vmm_mapping_count(void)
{
    return g_map_count;
}