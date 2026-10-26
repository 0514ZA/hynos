/*
 * HynOS —— 虚拟内存管理器接口（PML4 四级页表的管理策略层，架构无关）
 *
 * 分工：
 *   arch/x86_64/mmu.c  负责"怎么摆页表"（CPU 专属）
 *   core/vm/vmm.c      负责"给谁映射、映射到哪"（本文件，纯策略）
 *
 * 地址空间布局（当前里程碑）：
 *   [0x0000000000000000, 0x100000000)   恒等映射 0..4GiB（内核代码/数据/MMIO 直接可达）
 *   [0xFFFF900000000000, +4MiB)         内核动态映射窗口（4K 粒度，窗口内 VA 与 PA 解耦）
 *
 * 窗口是本里程碑"虚拟内存管理"的核心：同一段 VA 可以先后映射到不同物理页，
 * 取消映射后可验证 translate 归零、且真实访问会触发 #PF。
 */
#ifndef HYN_VMM_H
#define HYN_VMM_H

#include <hyn/types.h>

#define HYN_VMM_IDENTITY_LIMIT 0x100000000ull        /* 恒等映射上限：4GiB */
#define HYN_VMM_WINDOW_BASE    0xFFFF900000000000ull /* 内核动态映射窗口基址 */
#define HYN_VMM_WINDOW_PAGES   1024u                 /* 窗口大小：1024 页 = 4MiB */

hyn_status_t vmm_init(void);
int  vmm_ready(void);
u64  vmm_kernel_root(void);          /* 内核 PML4 物理地址 */

/* 地址翻译：返回物理地址；0 表示未映射 */
u64  vmm_translate(u64 va);

/* 任意 4K 映射 / 取消映射（恒等映射区内的 VA 会被拒绝，需大页拆分，留待后续里程碑） */
hyn_status_t vmm_map_page(u64 va, u64 pa, u64 flags);
hyn_status_t vmm_unmap_page(u64 va);

/*
 * 设备 MMIO 窗口：确保 [pa, pa+size) 在"物理地址即虚拟地址"的意义下可访问。
 *
 * 存在的理由：内核恒等映射只铺到 4GiB，而固件（UEFI/OVMF 尤其）会把 64 位 BAR
 * 放进 4GiB 以上的 64 位 MMIO 窗口。驱动要取寄存器读数，就必须先把该窗口按同址
 * 补映射进来——低 4GiB 内的窗口本来就已覆盖，直接判定可用、不动页表。
 * 返回 HYN_OK 表示窗口真正可访问（已覆盖或本次补映射后逐页复核通过）。
 */
hyn_status_t vmm_map_mmio(u64 pa, u64 size);

/* 内核窗口：分配物理页并映射；或把"已有物理页"映射进来；释放时解除映射并归还物理页 */
hyn_status_t vmm_alloc_window(u64 *out_va);
hyn_status_t vmm_map_window(u64 *out_va, u64 pa);
hyn_status_t vmm_free_window(u64 va);

/*
 * 用户页：从 [HYN_USER_BASE, HYN_USER_TOP) 里逐页顺序分配，页表项带 U=1。
 * 只有带 U 位的映射才允许 Ring3 访问——内核恒等映射区（0..4GiB）一律不带 U，
 * 于是"用户态摸得到什么"完全由这里的映射决定，而不是由地址范围猜。
 */
hyn_status_t vmm_alloc_user_page(u64 *out_va);
hyn_status_t vmm_free_user_page(u64 va);
u32 vmm_user_page_count(void);

u32  vmm_window_used(void);
u32  vmm_mapping_count(void);

#endif /* HYN_VMM_H */