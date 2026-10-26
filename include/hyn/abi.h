/*
 * HynOS —— Ring3 <-> Ring0 稳定 ABI（用户态库与内核之间唯一的契约）
 *
 * 这一层只放"两侧都必须遵守"的东西：
 *   · syscall 调用号与错误码；   · ALPC 消息的二进制布局；
 *   · 组间 op 常量与随消息回传的取证结构；· 用户程序的装载约定与记录页布局。
 *
 * 纪律：
 *   1) 用户态程序（libhyn 与业务代码）只 include 本文件，不 include 任何内核内部头——
 *      内核内部结构调整不会传染用户态；用户态也不需要知道端口、线程、页表这些内部概念。
 *   2) 内核侧在 include/hyn/userprog.h 里用 _Static_assert 逐字段核对本文件的结构体布局，
 *      两侧一旦对不上，编译期直接失败（不靠"跑起来才发现"）。
 *   3) 本文件必须自包含：不 include <hyn/types.h>，用自己的定宽类型，避免把用户态拖进内核头。
 */
#ifndef HYN_ABI_H
#define HYN_ABI_H

/* ---- 定宽类型：与内核 u8/u16/u32/u64 等价的用户态别名 ---- */
typedef unsigned char      hyn_abi_u8;
typedef unsigned short     hyn_abi_u16;
typedef unsigned int       hyn_abi_u32;
typedef unsigned long long hyn_abi_u64;

/* ---- ABI 版本：用户程序把它写进记录页，内核据此核对"跑的是不是同一代契约" ---- */
#define HYN_ABI_VERSION 1u

/* ---- 错误码（值与内核 hyn_status_t 完全一致） ---- */
#define HYN_ABI_OK           0u
#define HYN_ABI_EINVAL       1u
#define HYN_ABI_ENOMEM       2u
#define HYN_ABI_ENOENT       3u
#define HYN_ABI_EPERM        4u
#define HYN_ABI_ENOTSUP      5u
#define HYN_ABI_EBUSY        6u
#define HYN_ABI_EFAULT       7u
#define HYN_ABI_EIO          8u
#define HYN_ABI_EDOMAIN      9u
#define HYN_ABI_ETIMEDOUT    10u
#define HYN_ABI_EFAULT_FAULT 11u

/* ---- syscall 调用号（与内核 include/hyn/syscall.h 一致） ---- */
#define HYN_ABI_SYS_PING      0x0001u   /* (magic) -> rax=0, rdx=magic 回显 */
#define HYN_ABI_SYS_GET_TID   0x0002u   /* () -> rax=0, rdx=线程 id */
#define HYN_ABI_SYS_CHECK_CPL 0x0003u   /* (cs) -> rax=0 表示与内核帧 CS 一致 */
#define HYN_ABI_SYS_YIELD     0x0004u   /* () -> rax=0 */
#define HYN_ABI_SYS_ALPC_SEND 0x0010u   /* (目标端口 id, 消息*) -> rax=0 */
#define HYN_ABI_SYS_ALPC_RECV 0x0011u   /* (本端口 id, 消息*, 超时节拍) -> rax=0 */
#define HYN_ABI_SYS_EXIT      0x0020u   /* (退出码) -> 不返回 */

#define HYN_ABI_PING_MAGIC    0x48594E53u   /* 'HYNS'：核对参数真的到了内核 */

/* ---- ALPC 消息（二进制布局必须与内核 hyn_alpc_msg_t 逐字段一致） ---- */
#define HYN_ABI_MSG_MAGIC     0x504E5948u   /* 内存中读作 "HYNP" */
#define HYN_ABI_MSG_REQUEST   0x00000001u
#define HYN_ABI_MSG_REPLY     0x00000002u
#define HYN_ABI_MSG_NOTIFY    0x00000003u
#define HYN_ABI_MSG_MAX       256u

typedef struct hyn_abi_msg {
    hyn_abi_u32 magic;
    hyn_abi_u32 msg_id;
    hyn_abi_u32 type;
    hyn_abi_u32 flags;
    hyn_abi_u32 sender_id;
    hyn_abi_u8  sender_domain;
    hyn_abi_u8  target_domain;
    hyn_abi_u8  reserved0[2];
    hyn_abi_u32 len;
    hyn_abi_u32 reserved1;
    hyn_abi_u64 reply_to;
    hyn_abi_u8  data[HYN_ABI_MSG_MAX];
} hyn_abi_msg_t;

/* ---- 故障域常量：域编号与状态（与内核 hyn_domain_t / hyn_domain_state_t 一致） ---- */
#define HYN_ABI_DOMAIN_CORE    0u
#define HYN_ABI_DOMAIN_STORAGE 1u
#define HYN_ABI_DOMAIN_NET     2u
#define HYN_ABI_DOMAIN_BUS     3u

#define HYN_ABI_DOMAIN_OFFLINE   0u
#define HYN_ABI_DOMAIN_ONLINE    1u
#define HYN_ABI_DOMAIN_FAULTED   2u
#define HYN_ABI_DOMAIN_RESETTING 3u

/* ---- 组间调用号（当前消息放在 msg.flags） ---- */
#define HYN_ABI_OP_PING       0x0001u
#define HYN_ABI_OP_INFO       0x0002u
#define HYN_ABI_OP_CRASH      0x00FFu
#define HYN_ABI_OP_STORAGE_PROBE 0x0101u
#define HYN_ABI_OP_NET_PROBE  0x0201u
#define HYN_ABI_OP_BUS_ENUM   0x0301u

/* ---- PING 应答载荷（与内核 hyn_group_ping_t 一致） ---- */
typedef struct hyn_abi_group_ping {
    hyn_abi_u32 op_echo;
    hyn_abi_u32 domain;
    hyn_abi_u32 state;
    hyn_abi_u32 epoch;
    hyn_abi_u32 resources;
    hyn_abi_u32 devices_ready;
} hyn_abi_group_ping_t;

/* ---- INFO 应答载荷（与内核 hyn_group_info_t 一致） ---- */
typedef struct hyn_abi_group_info {
    hyn_abi_u32 domain;
    hyn_abi_u32 state;
    hyn_abi_u32 epoch;
    hyn_abi_u32 devices_total;
    hyn_abi_u32 devices_ready;
    hyn_abi_u32 resources;
    hyn_abi_u32 probe_count;
    hyn_abi_u32 quiesce_count;
    hyn_abi_u32 reset_count;
    hyn_abi_u32 fault_count;
    hyn_abi_u32 msg_served;
    hyn_abi_u32 msg_discarded;
    hyn_abi_u32 last_fault_vector;
    hyn_abi_u32 last_fault_error;
    hyn_abi_u64 last_fault_thread;
    hyn_abi_u64 svc_port;
    hyn_abi_u64 svc_thread;
    char        name[16];
    char        last_fault_thread_name[16];
} hyn_abi_group_info_t;

/* ---- NET_PROBE 应答载荷（与内核 hyn_net_probe_t 一致） ---- */
typedef struct hyn_abi_net_probe {
    hyn_abi_u32 nic_found;
    hyn_abi_u16 pci_vendor;
    hyn_abi_u16 pci_device;
    hyn_abi_u32 bar0_size;
    hyn_abi_u32 reg_ok;
    hyn_abi_u32 mac_hi;
    hyn_abi_u32 mac_lo;
    hyn_abi_u32 link_up;
    hyn_abi_u32 ctrl_status_raw;
    hyn_abi_u32 cksum_result;
    hyn_abi_u32 cksum_expected;
    hyn_abi_u32 conns;
} hyn_abi_net_probe_t;

/*
 * ---- 用户程序装载约定 ----
 * 内核把入口 RIP、用户栈顶布置好，并在栈顶放一个 hyn_abi_user_args_t；
 * 用户入口桩把 rsp 当参数指针（"栈顶即参数块"），再给 C 代码腾出栈空间。
 */
typedef struct hyn_abi_user_args {
    hyn_abi_u64 target_port;   /* 本次要请求的内核服务端口 id */
    hyn_abi_u64 record_va;     /* 记录页 VA：用户写、内核读 */
    hyn_abi_u64 self_port;     /* 本用户线程的身份端口 id（内核绑定，用户不能改） */
    hyn_abi_u64 arena_va;      /* 预留：用户消息缓冲区页（当前为 0） */
} hyn_abi_user_args_t;

/* ---- 记录页布局：用户程序跑完留下的"自证材料"，内核据此核对 ---- */
#define HYN_ABI_REC_MAGIC  0x374E4952ull   /* 内存中读作 "RIN7" */
#define HYN_ABI_REC_DONE   0x00005A5Aull
#define HYN_ABI_REC_MSG_OFF 0x100u         /* 消息缓冲区偏移：用户在这里拼请求/收应答 */

/* 用户程序类别：内核据此解释 k0..k3 四个关键读数 */
#define HYN_ABI_KIND_SHELL 1u              /* INFO 读数：分组规模 */
#define HYN_ABI_KIND_AGENT 2u              /* NET_PROBE 读数：网卡硬件事实 */

#define HYN_ABI_USER_CS    0x2Bull         /* 探针/用户程序自读的 %cs（RPL3） */

typedef struct hyn_abi_record {
    hyn_abi_u64 magic;        /* 0x00  "RIN7"：说明程序至少写到了第一条记录 */
    hyn_abi_u64 abi_version;  /* 0x08  契约版本 */
    hyn_abi_u64 kind;         /* 0x10  HYN_ABI_KIND_* */
    hyn_abi_u64 tid;          /* 0x18  SYS_GET_TID 返回值 */
    hyn_abi_u64 cs;           /* 0x20  程序自读的 %cs（交给内核核对） */
    hyn_abi_u64 cpl_ret;      /* 0x28  SYS_CHECK_CPL(cs) 返回值（0 = 与内核帧一致） */
    hyn_abi_u64 self_port;    /* 0x30  内核参数里的身份端口 id */
    hyn_abi_u64 target_port;  /* 0x38  内核参数里的目标服务端口 id */
    hyn_abi_u64 ping_ret;     /* 0x40  第一次请求（PING）往返返回码 */
    hyn_abi_u64 ping_echo;    /* 0x48  应答里回声的参数（应与程序发出的常量一致） */
    hyn_abi_u64 ping_domain;  /* 0x50  应答来源域 */
    hyn_abi_u64 ping_msg_id;  /* 0x58  应答原样回填的序号 */
    hyn_abi_u64 ping_state;   /* 0x60  应答里的组状态 */
    hyn_abi_u64 ping_epoch;   /* 0x68  应答里的组纪元 */
    hyn_abi_u64 req2_ret;     /* 0x70  第二次请求（INFO / NET_PROBE）往返返回码 */
    hyn_abi_u64 req2_len;     /* 0x78  第二次应答载荷字节数 */
    hyn_abi_u64 req2_domain;  /* 0x80  第二次应答来源域 */
    hyn_abi_u64 k0;           /* 0x88  关键读数 0（按 kind 解释） */
    hyn_abi_u64 k1;           /* 0x90  关键读数 1 */
    hyn_abi_u64 k2;           /* 0x98  关键读数 2 */
    hyn_abi_u64 k3;           /* 0xA0  关键读数 3 */
    hyn_abi_u64 recv_to;      /* 0xA8  一次"故意超时收取"的返回码（期望 ETIMEDOUT） */
    hyn_abi_u64 badptr_ret;   /* 0xB0  一次"非法用户指针"发送的返回码（期望 EFAULT） */
    hyn_abi_u64 done;         /* 0xB8  收尾标记 0x5A5A：只有跑完整条流程才会写 */
    hyn_abi_u64 sys_ping_echo;/* 0xC0  SYS_PING 的回声：核对 syscall 参数真的到了内核 */
} hyn_abi_record_t;

/*
 * 记录页里 k0..k3 与两个负向返回码的解释（按 kind 分）：
 *   Shell(kind=1)：k0=devices_total k1=devices_ready k2=resources k3=probe_count
 *                  recv_to=超时收取（期望 ETIMEDOUT）  badptr_ret=非法指针**发送**（期望 EFAULT）
 *   Agent(kind=2)：k0=nic_found k1=(vendor<<16|device) k2=cksum_expected k3=cksum_result
 *                  recv_to=收他人端口（期望 EPERM）     badptr_ret=非法指针**收取**（期望 EFAULT）
 */

#endif /* HYN_ABI_H */