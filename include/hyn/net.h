/*
 * 混元 OS —— 网络组协议栈接口（网卡驱动 + TCP/IP 骨架）
 *
 * 归属：快通道分组故障域 · 网络组。
 * 分层纪律：协议栈不进硬核底座。本组崩溃只由本组捕获、本组重置。
 *
 * 诚实性说明：
 *   · 连接表里只登记本组协议栈骨架的 LISTEN 槽位，不伪造任何"已建立连接"；
 *   · 网卡寄存器（CTRL/STATUS/RAL0/RAH0）是真实 MMIO 读数；
 *   · IPv4 首部校验和是真算的（按 RFC 791 的 16 位反码求和），不是常量；
 *   · 没有实现任何收发路径、ARP、TCP 状态机与重传，故不声称"能联网"。
 */
#ifndef HYN_NET_H
#define HYN_NET_H

#include <hyn/types.h>

#define HYN_NET_MAX_CONNS   8u
#define HYN_NET_PROTO_MAX   8
#define HYN_NET_ADDR_MAX    16

typedef enum hyn_net_conn_state {
    HYN_NET_CONN_FREE        = 0u,
    HYN_NET_CONN_LISTEN      = 1u,
    HYN_NET_CONN_ESTABLISHED = 2u,
    HYN_NET_CONN_CLOSED      = 3u
} hyn_net_conn_state_t;

typedef struct hyn_net_conn {
    u32  used;
    u32  state;                      /* hyn_net_conn_state_t */
    u16  local_port;
    u16  remote_port;
    u32  remote_addr;
    u32  rx_bytes;
    u32  tx_bytes;                   /* 骨架阶段恒为 0：没有收发路径就绝不编造字节数 */
    char proto[HYN_NET_PROTO_MAX];
    char name[HYN_NET_ADDR_MAX];
} hyn_net_conn_t;

/* e1000 寄存器取证：真实 MMIO 读数 */
typedef struct hyn_net_nic_regs {
    u32 found;
    u32 reg_ok;
    u32 bar0_size;
    u32 bar0_base_lo;
    u32 bar0_base_hi;
    u16 vendor;
    u16 device;
    u32 ctrl;                        /* CTRL  (0x0000) */
    u32 status;                      /* STATUS(0x0008) */
    u32 link_up;                     /* STATUS.LU (bit1)：真实链路状态 */
    u32 mac_lo;                      /* RAL0  (0x5400) */
    u32 mac_hi;                      /* RAH0  (0x5404) */
} hyn_net_nic_regs_t;

/* 生命周期 */
hyn_status_t net_init(void);
hyn_status_t net_reset(void);
hyn_status_t net_quiesce(void);

/* 取证访问器 */
u32 net_conn_count(void);            /* 连接表在册条目数（= LISTEN 数） */
u32 net_listen_count(void);
u32 net_established_count(void);     /* 骨架阶段恒为 0 */
const hyn_net_conn_t   *net_conn(u32 idx);
const hyn_net_nic_regs_t *net_nic_regs(void);

/*
 * 真实计算：IPv4 首部 16 位反码校验和（RFC 791）。
 * 入参 hdr 为 IPv4 首部、len 为首部字节数（须为偶数或末字节按高位补齐）。
 * 返回校验和字段值：把它填进首部后再算一次，结果必须为 0。
 */
u32 net_ip_checksum(const u8 *hdr, u32 len);

#endif /* HYN_NET_H */