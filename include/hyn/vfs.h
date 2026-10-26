/*
 * HynOS —— 存储组 VFS（虚拟文件系统）接口
 *
 * 归属：快通道分组故障域 · 存储组（NVMe 驱动 + VFS）。
 * 分层纪律：VFS 不进硬核底座（core/ 只放调度、PML4 内存、ALPC、中断异常、对象管理器）。
 * 生命周期纪律：本组状态全部经 vfs_init/vfs_reset/vfs_quiesce 管理，
 *   quiesce 之后本组不留任何活着的资源账目；reset 必须能在 quiesce 之后独立重建。
 *
 * 诚实性说明：本里程碑实现的是"接口骨架 + 真实设备认领"。
 *   · 节点表描述的是 VFS 的命名空间骨架（根 / dev / sys / proc），不是伪造的磁盘内容；
 *   · 挂载项只在真的认领到 NVMe 控制器时才建立，backend_ok 表示控制器寄存器确实读到了；
 *   · 没有实现任何块读写、目录项落盘或缓存，故不声称"文件系统可用"。
 */
#ifndef HYN_VFS_H
#define HYN_VFS_H

#include <hyn/types.h>

#define HYN_VFS_MAX_NODES  24u
#define HYN_VFS_MAX_MOUNTS 4u
#define HYN_VFS_NAME_MAX   32

typedef enum hyn_vfs_kind {
    HYN_VFS_KIND_FREE  = 0u,
    HYN_VFS_KIND_DIR   = 1u,
    HYN_VFS_KIND_FILE  = 2u,
    HYN_VFS_KIND_BLOCK = 3u
} hyn_vfs_kind_t;

typedef struct hyn_vfs_node {
    u32  used;
    u32  kind;                       /* hyn_vfs_kind_t */
    u32  parent;                     /* 父节点下标；根节点指向自身 */
    u32  refs;                       /* 被引用次数（打开数由上层记账，此处只做骨架） */
    u64  size;                       /* 目录为 0；块设备为 0（尚未实现块读写） */
    char name[HYN_VFS_NAME_MAX];
} hyn_vfs_node_t;

typedef struct hyn_vfs_mount {
    u32  used;
    u32  node;                       /* 挂载点节点下标 */
    u32  backend_ok;                 /* 1 = 后端块设备已认领且寄存器可读 */
    u32  backend_bar_size;
    u16  backend_vendor;
    u16  backend_device;
    char fs[16];                     /* 文件系统类型标识（骨架阶段为标识，非真实格式） */
    char backend[24];                /* 后端块设备名 */
} hyn_vfs_mount_t;

/* 生命周期 */
hyn_status_t vfs_init(void);         /* 首次建立命名空间骨架 + 认领真后端 */
hyn_status_t vfs_reset(void);        /* 重建：先清空再按当前硬件重认领 */
hyn_status_t vfs_quiesce(void);      /* 静默：清空全部节点与挂载（组内资源归零） */

/* 取证访问器 */
u32 vfs_node_count(void);
u32 vfs_mount_count(void);
u32 vfs_ready_mount_count(void);     /* backend_ok == 1 的挂载数 */
u32 vfs_dir_count(void);
const hyn_vfs_node_t  *vfs_node(u32 idx);
const hyn_vfs_mount_t *vfs_mount(u32 idx);

/*
 * NVMe 控制器寄存器取证：真实 MMIO 读数，不是常量。
 *   reg_ok = 0 只有两种诚实含义：本机未认领到控制器，或 BAR 落在恒等映射窗口（0..4GiB）之外。
 */
typedef struct hyn_nvme_regs {
    u32 found;
    u32 reg_ok;
    u32 bar0_size;
    u32 bar0_base_lo;
    u32 bar0_base_hi;
    u16 vendor;
    u16 device;
    u32 cap_lo;                      /* NVMe CAP[31:0] */
    u32 cap_hi;                      /* NVMe CAP[63:32] */
    u32 version;                     /* NVMe VS */
    u32 csts;                        /* NVMe CSTS */
} hyn_nvme_regs_t;

const hyn_nvme_regs_t *vfs_nvme_regs(void);

#endif /* HYN_VFS_H */