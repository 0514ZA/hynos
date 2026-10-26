/*
 * HynOS —— 快通道分组故障域接口（硬核底座之上的一层纪律）
 *
 * 架构对应关系：
 *   · 硬核底座（CORE）：调度、PML4 虚拟内存、ALPC、中断异常、对象管理器 —— 不承载驱动/FS/协议栈；
 *   · 快通道分组故障域：存储组（NVMe 驱动 + VFS）、网络组（网卡驱动 + TCP/IP）、
 *     硬件总线组（ACPI、PCIe、基础硬件管理）；
 *   · 每个分组自带异常捕获、资源回收与单独重置逻辑：单组崩溃/被重置，硬核底座与其余两组继续运行。
 *
 * 四条纪律（都在代码里强制，且有自检项）：
 *   1) syscall 是 Ring3 进入内核的唯一入口（在 syscall/IDT 层强制）；
 *   2) 跨故障域通信强制走 ALPC 消息：hyn_domain_call 走消息往返，
 *      hyn_group_local_call 在同域内直调、跨域一律 HYN_EDOMAIN；
 *   3) 组内异常由本组自己捕获（hyn_domain_fault_hook）：只结束故障线程 + 标记本组 FAULTED，
 *      底座与其余两组不受影响；CORE 自身的异常仍如实停机，绝不吞掉；
 *   4) 重置协议固定为 quiesce -> reset -> init：先让本组静默、回收资源，再重建。
 *
 * 消息约定：REQUEST 的 op 放在 hyn_alpc_msg_t.flags，参数放 data；REPLY 的 msg_id 与请求一致
 * （按 msg_id 配对）。同一域内并发跨域调用共用一条请求端口，配对不上的应答计入 msg_discarded；
 * 本里程碑的自检为串行调用，因此不引入"每调用一个端点"的额外机制。
 */
#ifndef HYN_FAULT_H
#define HYN_FAULT_H

#include <hyn/types.h>
#include <hyn/object.h>
#include <hyn/trap.h>
#include <hyn/thread.h>

/* 被故障域接管的内核线程退出码：0x2_0000 | 异常向量（与 Ring3 的 0x1_0000|向量 明确区分） */
#define HYN_DOMAIN_FAULT_EXIT_BASE 0x20000u

typedef enum hyn_domain_state {
    HYN_DOMAIN_STATE_OFFLINE   = 0u,     /* 未注册/未启动 */
    HYN_DOMAIN_STATE_ONLINE    = 1u,     /* 正常服务 */
    HYN_DOMAIN_STATE_FAULTED   = 2u,     /* 本组刚出过异常：服务已停，等重置 */
    HYN_DOMAIN_STATE_RESETTING = 3u,     /* 静默中/正在重建（服务已停） */
    HYN_DOMAIN_STATE_MAX       = 4u
} hyn_domain_state_t;

/* ---- 组间调用号 ---- */
#define HYN_GROUP_OP_PING   0x0001u      /* 回声：返回组纪元/状态/资源数（框架统一实现） */
#define HYN_GROUP_OP_INFO   0x0002u      /* 快照：返回 hyn_group_info_t（框架统一实现） */
#define HYN_GROUP_OP_CRASH  0x00FFu      /* 故意解引用未映射内核地址：验证本组异常捕获 */
#define HYN_GROUP_OP_STORAGE_PROBE 0x0101u
#define HYN_GROUP_OP_STORAGE_CHAIN 0x0102u  /* 存储组服务线程内部再调网络组：验证"组到组"也走 ALPC */
#define HYN_GROUP_OP_NET_PROBE     0x0201u
#define HYN_GROUP_OP_BUS_ENUM      0x0301u

typedef struct hyn_group_ping {
    u32 op_echo;                         /* 原样回填，用于核对参数真的过了消息总线 */
    u32 domain;
    u32 state;
    u32 epoch;                           /* 每次重置自增：调用方据此判断"是不是重启过的组" */
    u32 resources;
    u32 devices_ready;
} hyn_group_ping_t;

/* 组快照：自检与汇报用的取证结构（也随 ALPC 消息回传，两条取数路径结果必须一致） */
typedef struct hyn_group_info {
    u32  domain;
    u32  state;
    u32  epoch;
    u32  devices_total;
    u32  devices_ready;
    u32  resources;
    u32  probe_count;
    u32  quiesce_count;
    u32  reset_count;
    u32  fault_count;
    u32  msg_served;
    u32  msg_discarded;
    u32  last_fault_vector;              /* 最近一次被接管的异常向量；无异常时为 0xFFFFFFFF */
    u32  last_fault_error;
    u64  last_fault_thread;              /* 故障线程 id */
    u64  svc_port;
    u64  svc_thread;
    char name[16];
    char last_fault_thread_name[16];
} hyn_group_info_t;

/* 存储组专属探测结果 */
typedef struct hyn_storage_probe {
    u32 nvme_found;
    u32 nvme_class;
    u16 pci_vendor;
    u16 pci_device;
    u32 bar0_size;
    u32 reg_ok;                          /* MMIO 寄存器是否真的读到了（0 = BAR 在映射窗口外） */
    u32 cap_lo;                          /* NVMe CAP[31:0] */
    u32 cap_hi;                          /* NVMe CAP[63:32] */
    u32 version;                         /* NVMe VS：QEMU 下应为非零版本号 */
    u32 csts;                            /* NVMe CSTS：bit0 = RDY */
    u32 vfs_nodes;
    u32 vfs_mounts;
} hyn_storage_probe_t;

/* 存储组的"组到组"链式调用结果：存储组线程内部再调网络组 */
typedef struct hyn_storage_chain {
    u32 chain_ok;                        /* 内层跨域调用返回码（0=成功） */
    u32 chain_denied_before;             /* 调用前累计的"跨域调用被拒"次数（供自检核对自增） */
    hyn_group_ping_t net_ping;           /* 网络组回给存储组的回声 */
} hyn_storage_chain_t;

/* 网络组专属探测结果 */
typedef struct hyn_net_probe {
    u32 nic_found;
    u16 pci_vendor;
    u16 pci_device;
    u32 bar0_size;
    u32 reg_ok;
    u32 mac_hi;                          /* e1000 RAL/RAH 里的 MAC（真实寄存器读数） */
    u32 mac_lo;
    u32 link_up;
    u32 ctrl_status_raw;
    u32 cksum_result;                    /* 真实计算的 IP 首部校验和结果（0 = 校验通过） */
    u32 cksum_expected;
    u32 conns;                           /* 协议栈连接表条目数（LISTEN 数，不含伪造的已建立连接） */
} hyn_net_probe_t;

/* 硬件总线组专属探测结果 */
typedef struct hyn_bus_probe {
    u32 pci_devices;
    u32 pci_bridges;
    u32 acpi_rsdp_found;
    u32 acpi_checksum_ok;                /* RSDP 校验和校验（20 字节）是否通过 */
    u32 acpi_ext_ok;                     /* 版本>=2 时扩展校验和是否通过 */
    u32 acpi_revision;
    u32 acpi_tables;
    u32 acpi_tables_ok;
    u32 rsdt_address;
    char oem_id[8];
    char first_table[6];
} hyn_bus_probe_t;

/* 组实现（每个分组一个）：probe 认领真实资源，quiesce 静默，reset 重建，dispatch 处理组专属 op */
typedef struct hyn_domain_ops {
    const char  *name;
    hyn_status_t (*probe)(void);
    hyn_status_t (*quiesce)(void);
    hyn_status_t (*reset)(void);
    hyn_status_t (*dispatch)(u32 op, const u8 *in, u32 in_len, u8 *out, u32 *out_len);
} hyn_domain_ops_t;

/* 三组把各自的 ops 交进来（实现在 core/fault/group_*.c），由 hyn_domain_init 统一注册 */
const hyn_domain_ops_t *hyn_group_storage_ops(void);
const hyn_domain_ops_t *hyn_group_net_ops(void);
const hyn_domain_ops_t *hyn_group_bus_ops(void);

/* ---- 生命周期 ---- */
hyn_status_t hyn_domain_register(hyn_domain_t domain, const hyn_domain_ops_t *ops);
hyn_status_t hyn_domain_init(void);                  /* 注册三组 + 建端口与线程 + 逐组 probe */
hyn_status_t hyn_domain_quiesce(hyn_domain_t domain); /* 静默本组：停服务线程、停设备、回收资源 */
hyn_status_t hyn_domain_reset(hyn_domain_t domain);   /* 重置本组：quiesce -> 重探 -> 重建服务线程 */

/*
 * 组自行上报资源账目（由各组的 probe/quiesce 在框架允许的时机调用）：
 * resources = 本组掌握的资源条目数（VFS 节点/连接表/枚举设备…），devices_* = 组内设备就绪情况。
 */
void hyn_domain_report(hyn_domain_t domain, u32 resources, u32 devices_total, u32 devices_ready);

/* ---- 异常捕获 ---- */
/*
 * 陷阱分派在"未处理异常转储停机"之前调用：只有当前线程归属某个分组故障域（非 CORE）时才接管，
 * 返回 1 表示已接管（只结束该线程 + 标记该组 FAULTED）；返回 0 交回陷阱框架（CORE 异常仍停机）。
 */
int hyn_domain_fault_hook(const hyn_trap_frame_t *f);

/* 故障后置位的"待重置"标记：重置要在线程上下文的安全点做，不能在异常上下文里做 */
int hyn_domain_pending_reset(hyn_domain_t domain);
hyn_status_t hyn_domain_service_pending(void);       /* 安全点：处理所有待重置的组 */

/* ---- 组间调用 ---- */
/*
 * 跨故障域调用：必须经 ALPC 消息往返（请求 -> 本组服务线程处理 -> 应答）。
 * 返回 HYN_EDOMAIN：调用方与目标同域（同域应走 hyn_group_local_call 直调，不占消息总线）。
 * 目标组处于 FAULTED 返回 HYN_EFAULT_FAULT，RESETTING 返回 HYN_EBUSY —— 调用方拿到的是
 * "对方不可用"的明确结论，而不是被挂住等超时。
 */
hyn_status_t hyn_domain_call(hyn_domain_t caller, hyn_domain_t target, u32 op,
                             const void *in, u32 in_len,
                             void *out, u32 out_len, u32 *out_len_actual);

/* 同域直调：跨域一律拒绝（这是"禁止跨域直接函数调用"这条纪律的代码形态） */
hyn_status_t hyn_group_local_call(hyn_domain_t caller, hyn_domain_t target, u32 op,
                                  const void *in, u32 in_len,
                                  void *out, u32 out_len, u32 *out_len_actual);

/* ---- 取证访问器 ---- */
const char *hyn_domain_state_name(hyn_domain_state_t s);
const char *hyn_domain_name(hyn_domain_t d);
const hyn_group_info_t *hyn_domain_info(hyn_domain_t d);
int hyn_domain_registered(hyn_domain_t d);
hyn_thread_t *hyn_domain_svc_thread(hyn_domain_t d);
u64 hyn_domain_svc_port(hyn_domain_t d);
u64 hyn_domain_req_port(hyn_domain_t d);

/*
 * 底座侧安全点代服务：非阻塞地替某域处理一条落在其请求口上的跨域请求。
 * 供 Ring3 用户程序（经 syscall 投递到 core.req）在底座安全点被应答；无消息返回 HYN_ENOENT。
 */
hyn_status_t hyn_domain_serve_port(u64 port_id, u32 *out_op, u32 *out_msg_id,
                                   u8 *out_sender_domain);
u32 hyn_domain_fault_total(void);       /* 全内核被故障域接管的异常次数 */
u32 hyn_domain_call_total(void);        /* 跨域消息往返次数 */
u32 hyn_domain_call_denied(void);       /* 因目标不可用/超时被拒的跨域调用次数 */
u32 hyn_domain_msg_discarded(void);     /* 配对不上的应答累计丢弃数 */
const char *hyn_domain_last_msg(void);  /* 最近一次跨域调用的审计行（自检打印用） */

#endif /* HYN_FAULT_H */