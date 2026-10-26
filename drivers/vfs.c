/*
 * 混元 OS —— 存储组 VFS 实现（骨架 + 真实 NVMe 控制器认领）
 *
 * 只做三件事，且都要求"证据可核对"：
 *   1) 建立 VFS 命名空间骨架（根 / dev / sys / proc）——这是接口骨架，不是假磁盘内容；
 *   2) 认领真实 NVMe 控制器：走 HAL 的 PCI 事实 + 真读 MMIO 寄存器（CAP/VS/CSTS）；
 *   3) 只有真的认领到控制器，才建立挂载项并把 backend_ok 置 1。
 *
 * 不实现：块读写、目录项落盘、页缓存、文件句柄。故本文件不声称"文件系统可用"。
 * 故障域纪律：本组任何状态都可由 vfs_quiesce 一次清空，vfs_reset 之后必须能独立重建。
 */
#include <hyn/vfs.h>
#include <hyn/hal.h>
#include <hyn/pci.h>
#include <hyn/vmm.h>
#include <hyn/kstring.h>

/* 内核恒等映射只覆盖 0..4GiB，4GiB 以上的 64 位 BAR 须先补映射（见先检查再读那一段） */

#define NVME_REG_CAP   0x00u
#define NVME_REG_VS    0x08u
#define NVME_REG_CSTS  0x1Cu

static hyn_vfs_node_t   g_nodes[HYN_VFS_MAX_NODES];
static hyn_vfs_mount_t  g_mounts[HYN_VFS_MAX_MOUNTS];
static hyn_nvme_regs_t  g_regs;
static u32 g_node_count;
static u32 g_mount_count;

/* ---- 节点表 ---- */

static u32 node_add(const char *name, u32 kind, u32 parent, u64 size)
{
    u32 i;

    for (i = 0u; i < HYN_VFS_MAX_NODES; i++) {
        if (g_nodes[i].used == 0u) {
            g_nodes[i].used   = 1u;
            g_nodes[i].kind   = kind;
            g_nodes[i].parent = parent;
            g_nodes[i].refs   = 0u;
            g_nodes[i].size   = size;
            (void)strncpy_safe(g_nodes[i].name, name, HYN_VFS_NAME_MAX);
            g_node_count++;
            return i;
        }
    }
    return 0xFFFFFFFFu;                 /* 表满：如实返回失败，不丢节点充数 */
}

static u32 mount_add(u32 node, const char *fs, const char *backend)
{
    u32 i;

    for (i = 0u; i < HYN_VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used == 0u) {
            g_mounts[i].used = 1u;
            g_mounts[i].node = node;
            (void)strncpy_safe(g_mounts[i].fs, fs, 16);
            (void)strncpy_safe(g_mounts[i].backend, backend, 24);
            g_mount_count++;
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/* ---- 真实 NVMe 控制器认领：只有硬件真的在读，才置 reg_ok ---- */

static void nvme_claim(void)
{
    const hal_device_t   *dev = hal_device_by_kind(HAL_DEV_NVME);
    const hal_pci_fact_t *f   = hal_device_fact(dev);

    memset(&g_regs, 0, (unsigned long)sizeof(g_regs));
    if (f == NULL || f->found == 0u) {
        return;                          /* 本机没有该控制器：found=0 就是诚实结论 */
    }
    g_regs.found         = 1u;
    g_regs.vendor        = f->vendor;
    g_regs.device        = f->device;
    g_regs.bar0_size     = f->bar0_size;
    g_regs.bar0_base_lo  = (u32)(f->bar0_base & 0xFFFFFFFFull);
    g_regs.bar0_base_hi  = (u32)(f->bar0_base >> 32);

    if (f->bar0_base == 0ull || f->bar0_size == 0u) {
        return;                          /* BAR 未分配：不猜地址去摸内存 */
    }
    /*
     * BAR 窗口必须真正可访问才能去读寄存器。恒等映射只铺到 4GiB，而固件——
     * UEFI/OVMF 尤其——会把 64 位 BAR 放到 4GiB 以上的 64 位 MMIO 窗口，
     * 所以先按同址补映射（低 4GiB 的窗口本就已覆盖）。
     * 补不上就如实报 reg_ok = 0，绝不闯未映射页。
     */
    if (vmm_map_mmio(f->bar0_base, (u64)f->bar0_size) != HYN_OK) {
        return;
    }

    /* 打开设备的内存空间译码：固件可能没替我们打开（UEFI 路径尤其如此） */
    hyn_pci_mem_enable(f->bus, f->dev, f->func);

    {
        volatile u32 *cap  = (volatile u32 *)(uptr)(f->bar0_base + NVME_REG_CAP);
        volatile u32 *vs   = (volatile u32 *)(uptr)(f->bar0_base + NVME_REG_VS);
        volatile u32 *csts = (volatile u32 *)(uptr)(f->bar0_base + NVME_REG_CSTS);

        g_regs.cap_lo  = cap[0];         /* CAP[31:0] */
        g_regs.cap_hi  = cap[1];         /* CAP[63:32]（紧邻的一个 dword） */
        g_regs.version = vs[0];
        g_regs.csts    = csts[0];
    }

    /* 全 1 / 全 0 都是"没真读到"的典型读数，不能当成功 */
    g_regs.reg_ok = ((g_regs.cap_lo != 0u) && (g_regs.cap_lo != 0xFFFFFFFFu) &&
                     (g_regs.version != 0u) && (g_regs.version != 0xFFFFFFFFu)) ? 1u : 0u;
}

/* ---- 生命周期 ---- */

hyn_status_t vfs_quiesce(void)
{
    memset(g_nodes, 0, (unsigned long)sizeof(g_nodes));
    memset(g_mounts, 0, (unsigned long)sizeof(g_mounts));
    memset(&g_regs, 0, (unsigned long)sizeof(g_regs));
    g_node_count  = 0u;
    g_mount_count = 0u;
    return HYN_OK;
}

hyn_status_t vfs_init(void)
{
    return vfs_reset();
}

hyn_status_t vfs_reset(void)
{
    u32 root, dev, sysn, proc;
    u32 blk, mnt;

    (void)vfs_quiesce();

    /* 命名空间骨架：根 / dev / sys / proc */
    root = node_add("/",    HYN_VFS_KIND_DIR, 0u,    0ull);
    dev  = node_add("dev",  HYN_VFS_KIND_DIR, root,  0ull);
    sysn = node_add("sys",  HYN_VFS_KIND_DIR, root,  0ull);
    proc = node_add("proc", HYN_VFS_KIND_DIR, root,  0ull);
    if (root == 0xFFFFFFFFu || dev == 0xFFFFFFFFu ||
        sysn == 0xFFFFFFFFu || proc == 0xFFFFFFFFu) {
        (void)vfs_quiesce();
        return HYN_ENOMEM;
    }

    /* 真后端：只有认领到 NVMe 控制器才建块节点与挂载项 */
    nvme_claim();
    if (g_regs.found != 0u) {
        blk = node_add("nvme0n1", HYN_VFS_KIND_BLOCK, dev, 0ull);
        if (blk != 0xFFFFFFFFu) {
            mnt = mount_add(blk, "hyn-nvme", "nvme0n1");
            if (mnt != 0xFFFFFFFFu) {
                g_mounts[mnt].backend_ok      = g_regs.reg_ok;
                g_mounts[mnt].backend_bar_size = g_regs.bar0_size;
                g_mounts[mnt].backend_vendor  = g_regs.vendor;
                g_mounts[mnt].backend_device  = g_regs.device;
            }
        }
    }
    return HYN_OK;
}

/* ---- 取证访问器 ---- */

u32 vfs_node_count(void)        { return g_node_count; }
u32 vfs_mount_count(void)       { return g_mount_count; }

u32 vfs_ready_mount_count(void)
{
    u32 i, n = 0u;

    for (i = 0u; i < HYN_VFS_MAX_MOUNTS; i++) {
        if (g_mounts[i].used != 0u && g_mounts[i].backend_ok != 0u) {
            n++;
        }
    }
    return n;
}

u32 vfs_dir_count(void)
{
    u32 i, n = 0u;

    for (i = 0u; i < HYN_VFS_MAX_NODES; i++) {
        if (g_nodes[i].used != 0u && g_nodes[i].kind == (u32)HYN_VFS_KIND_DIR) {
            n++;
        }
    }
    return n;
}

const hyn_vfs_node_t *vfs_node(u32 idx)
{
    if (idx >= HYN_VFS_MAX_NODES || g_nodes[idx].used == 0u) {
        return NULL;
    }
    return &g_nodes[idx];
}

const hyn_vfs_mount_t *vfs_mount(u32 idx)
{
    if (idx >= HYN_VFS_MAX_MOUNTS || g_mounts[idx].used == 0u) {
        return NULL;
    }
    return &g_mounts[idx];
}

const hyn_nvme_regs_t *vfs_nvme_regs(void)
{
    return &g_regs;
}