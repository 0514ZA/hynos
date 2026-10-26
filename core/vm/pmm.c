/*
 * HynOS —— 物理页分配器（位图法，纯 C，架构无关）
 *
 * 设计要点：
 *   · 位图放物理地址 0x200000，受管内存从 0x400000 起，低区不参与分配；
 *   · 分配即清零（避免把上一任使用者的数据漏给下一个使用者）；
 *   · 拒绝二次释放与越界释放，便于自检直接暴露"重复释放"这类错误；
 *   · 环形首次适配（从上次位置继续找），避免每次都从低地址重扫。
 *
 * 【已知保守处】内核目前只从引导层拿到"内存总容量"，尚未拿到完整内存映射
 * （BIOS 的 E820 / UEFI 的描述符数组），因此这里按通用 PC 布局跳过
 * 0xE0000000..0x100000000 的标准 PCI 空洞。引导层补齐内存映射后，改为按
 * "可用 RAM 区间"逐一放行即可，本文件其余逻辑不变。
 */
#include <hyn/pmm.h>
#include <hyn/kstring.h>

static u8  *g_bitmap;        /* 恒等映射下可直接访问的位图 */
static u64  g_managed_end;   /* 受管区间上界（不含） */
static u64  g_hint;          /* 环形扫描起点 */
static u32  g_total_pages;   /* 受管页数（含空洞页） */
static u32  g_free_pages;    /* 可用页数 */
static u32  g_alloc_calls;
static int  g_ready;

static inline u32 page_index(u64 pa)
{
    return (u32)((pa - HYN_PMM_BASE) / HYN_PMM_PAGE_SIZE);
}

static int bit_test(u32 i)
{
    return (g_bitmap[i >> 3] >> (i & 7u)) & 1u;
}

static void bit_set(u32 i)
{
    g_bitmap[i >> 3] |= (u8)(1u << (i & 7u));
}

static void bit_clear(u32 i)
{
    g_bitmap[i >> 3] &= (u8)~(1u << (i & 7u));
}

/* 把一段区间标记为可用，并抬高受管上界 */
static void release_range(u64 start, u64 end)
{
    u64 pa;

    start = HYN_ALIGN_UP(start, HYN_PMM_PAGE_SIZE);
    end   = HYN_ALIGN_DOWN(end, HYN_PMM_PAGE_SIZE);
    for (pa = start; pa + HYN_PMM_PAGE_SIZE <= end; pa += HYN_PMM_PAGE_SIZE) {
        u32 i = page_index(pa);
        if (i < HYN_PMM_TOTAL_PAGES && bit_test(i)) {
            bit_clear(i);
            g_free_pages++;
        }
    }
    if (end > g_managed_end) {
        g_managed_end = end;
    }
}

hyn_status_t pmm_init(u64 mem_limit)
{
    u64 top;

    if (mem_limit > HYN_PMM_MAX_PHYS) {
        mem_limit = HYN_PMM_MAX_PHYS;
    }
    if (mem_limit <= HYN_PMM_BASE) {
        return HYN_ENOMEM;
    }

    g_bitmap      = (u8 *)(uptr)HYN_PMM_BITMAP_PHYS;
    g_free_pages  = 0;
    g_total_pages = 0;
    g_managed_end = 0;
    g_hint        = HYN_PMM_BASE;
    g_alloc_calls = 0;
    g_ready       = 0;

    memset(g_bitmap, 0xFF, (unsigned long)HYN_PMM_BITMAP_BYTES);  /* 先全部视为已用 */

    /* 可用 RAM 区间 1：低位 RAM（低区 0x0..0x400000 已被内核/引导栈/位图占用） */
    top = (mem_limit < HYN_PMM_HOLE_START) ? mem_limit : HYN_PMM_HOLE_START;
    if (top > HYN_PMM_BASE) {
        release_range(HYN_PMM_BASE, top);
    }

    /* 可用 RAM 区间 2：4GiB 之上的高端内存（机器真有才放行） */
    if (mem_limit > HYN_PMM_HOLE_END) {
        release_range(HYN_PMM_HOLE_END, mem_limit);
    }

    if (g_free_pages == 0u) {
        return HYN_ENOMEM;
    }

    g_total_pages = page_index(g_managed_end);
    g_ready = 1;
    return HYN_OK;
}

int pmm_ready(void)
{
    return g_ready;
}

int pmm_owns(u64 pa)
{
    return g_ready && pa >= HYN_PMM_BASE && pa < g_managed_end;
}

hyn_status_t pmm_reserve(u64 pa, u64 bytes)
{
    u64 end, p;

    if (!pmm_owns(pa)) {
        return HYN_EINVAL;
    }
    end = pa + HYN_ALIGN_UP(bytes, HYN_PMM_PAGE_SIZE);
    for (p = pa; p < end; p += HYN_PMM_PAGE_SIZE) {
        u32 i = page_index(p);
        if (i < g_total_pages && !bit_test(i)) {
            bit_set(i);
            g_free_pages--;
        }
    }
    return HYN_OK;
}

static u64 take_page(u32 i)
{
    u64 pa;

    bit_set(i);
    g_free_pages--;
    g_alloc_calls++;
    pa = HYN_PMM_BASE + (u64)i * HYN_PMM_PAGE_SIZE;
    g_hint = pa + HYN_PMM_PAGE_SIZE;
    memset((void *)(uptr)pa, 0, (unsigned long)HYN_PMM_PAGE_SIZE);
    return pa;
}

u64 pmm_alloc_page(void)
{
    u32 start, k;

    if (!g_ready) {
        return 0ull;
    }
    start = (g_hint >= HYN_PMM_BASE) ? page_index(g_hint) : 0u;
    for (k = 0; k < g_total_pages; k++) {
        u32 i = start + k;
        if (i >= g_total_pages) {
            i -= g_total_pages;             /* 环形回绕 */
        }
        if (!bit_test(i)) {
            return take_page(i);
        }
    }
    return 0ull;
}

u64 pmm_alloc_pages(u32 n)
{
    u32 start, k, i;

    if (!g_ready || n == 0u) {
        return 0ull;
    }
    if (n > g_free_pages) {
        return 0ull;
    }
    start = (g_hint >= HYN_PMM_BASE) ? page_index(g_hint) : 0u;

    for (k = 0; k < g_total_pages; k++) {
        u32 base = start + k;
        u32 run;

        if (base >= g_total_pages) {
            base -= g_total_pages;
        }
        if (bit_test(base)) {
            continue;
        }
        for (run = 1; run < n; run++) {
            u32 j = base + run;
            if (j >= g_total_pages) {
                j -= g_total_pages;
            }
            if (bit_test(j)) {
                break;
            }
        }
        if (run < n) {
            k += run - 1u;                  /* 跳过这段不连续区间 */
            continue;
        }
        for (i = 0; i < n; i++) {
            u32 j = base + i;
            u64 pa;
            if (j >= g_total_pages) {
                j -= g_total_pages;
            }
            pa = HYN_PMM_BASE + (u64)j * HYN_PMM_PAGE_SIZE;
            memset((void *)(uptr)pa, 0, (unsigned long)HYN_PMM_PAGE_SIZE);
            bit_set(j);
            g_free_pages--;
            g_alloc_calls++;
        }
        {
            u64 first = HYN_PMM_BASE + (u64)base * HYN_PMM_PAGE_SIZE;
            g_hint = first + (u64)n * HYN_PMM_PAGE_SIZE;
            return first;
        }
    }
    return 0ull;
}

int pmm_free_page(u64 pa)
{
    u32 i;

    if (!pmm_owns(pa) || (pa & (HYN_PMM_PAGE_SIZE - 1ull)) != 0ull) {
        return 0;
    }
    i = page_index(pa);
    if (i >= g_total_pages || !bit_test(i)) {
        return 0;                           /* 未分配或二次释放：拒绝 */
    }
    bit_clear(i);
    g_free_pages++;
    if (pa < g_hint) {
        g_hint = pa;
    }
    return 1;
}

u32 pmm_free_pages(u64 pa, u32 n)
{
    u32 freed = 0;
    u32 i;

    for (i = 0; i < n; i++) {
        if (!pmm_free_page(pa + (u64)i * HYN_PMM_PAGE_SIZE)) {
            break;
        }
        freed++;
    }
    return freed;
}

u64 pmm_managed_bytes(void)
{
    return (g_managed_end > HYN_PMM_BASE) ? (g_managed_end - HYN_PMM_BASE) : 0ull;
}

u64 pmm_free_bytes(void)
{
    return (u64)g_free_pages * HYN_PMM_PAGE_SIZE;
}

u32 pmm_free_page_count(void)
{
    return g_free_pages;
}

u32 pmm_total_page_count(void)
{
    return g_total_pages;
}

u32 pmm_alloc_count(void)
{
    return g_alloc_calls;
}