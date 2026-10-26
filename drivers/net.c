/*
 * HynOS —— 网络组协议栈实现（网卡驱动骨架 + 连接表骨架 + 真实 IP 校验和）
 *
 * 只做三件事，且都要求"证据可核对"：
 *   1) 认领真实网卡：走 HAL 的 PCI 事实 + 真读 e1000 的 MMIO 寄存器（CTRL/STATUS/RAL0/RAH0）；
 *   2) 建立协议栈连接表骨架：只登记 LISTEN 槽位——没有收发路径就不登记任何"已建立连接"；
 *   3) 真算 IPv4 首部校验和（RFC 791 的 16 位反码求和），供上层核对，不是常量。
 *
 * 不实现：收发描述符环、DMA、ARP、IP 分片/转发、TCP 状态机与重传。故本文件不声称"能联网"。
 * 故障域纪律：本组状态可由 net_quiesce 一次清空，net_reset 之后必须能独立重建。
 */
#include <hyn/net.h>
#include <hyn/hal.h>
#include <hyn/pci.h>
#include <hyn/vmm.h>
#include <hyn/kstring.h>

/* e1000 寄存器（QEMU 的 e1000 模型按这套偏移暴露 MMIO） */
#define E1000_REG_CTRL   0x0000u
#define E1000_REG_STATUS 0x0008u
#define E1000_REG_RAL0   0x5400u   /* 接收地址低 32 位 */
#define E1000_REG_RAH0   0x5404u   /* 接收地址高 16 位 + AV */

static hyn_net_conn_t      g_conns[HYN_NET_MAX_CONNS];
static hyn_net_nic_regs_t  g_nic;
static u32                 g_conn_count;

/* 协议栈骨架的 LISTEN 槽位：诚实说明——这只是登记了"本组打算监听的入口"，不是已连接 */
typedef struct { const char *proto; u16 port; const char *name; } net_listen_seed_t;
static const net_listen_seed_t g_listen_seeds[] = {
    { "tcp", 8080u, "net.mgmt"  },
    { "tcp", 8443u, "net.agent" },
    { "udp", 5353u, "net.disc"  }
};

/* ---- 真实网卡认领：只有硬件真的在读，才置 reg_ok ---- */

static void nic_claim(void)
{
    const hal_device_t   *dev = hal_device_by_kind(HAL_DEV_NET);
    const hal_pci_fact_t *f   = hal_device_fact(dev);

    memset(&g_nic, 0, (unsigned long)sizeof(g_nic));
    if (f == NULL || f->found == 0u) {
        return;                          /* 本机没有网卡：found=0 就是诚实结论 */
    }
    g_nic.found        = 1u;
    g_nic.vendor       = f->vendor;
    g_nic.device       = f->device;
    g_nic.bar0_size    = f->bar0_size;
    g_nic.bar0_base_lo = (u32)(f->bar0_base & 0xFFFFFFFFull);
    g_nic.bar0_base_hi = (u32)(f->bar0_base >> 32);

    if (f->bar0_base == 0ull || f->bar0_size == 0u) {
        return;                          /* BAR 未分配：不猜地址去摸内存 */
    }
    /*
     * BAR 窗口必须真正可访问才能去读寄存器。恒等映射只铺到 4GiB，而 64 位 BAR
     * 可能被固件放到 4GiB 以上，所以先按同址补映射（低 4GiB 的窗口本就已覆盖）。
     * 补不上就如实报 reg_ok = 0，绝不闯未映射页。
     */
    if (vmm_map_mmio(f->bar0_base, (u64)f->bar0_size) != HYN_OK) {
        return;
    }

    /* 打开设备的内存空间译码：e1000 的 MMIO 前必做 */
    hyn_pci_mem_enable(f->bus, f->dev, f->func);

    {
        volatile u32 *ctrl   = (volatile u32 *)(uptr)(f->bar0_base + E1000_REG_CTRL);
        volatile u32 *status = (volatile u32 *)(uptr)(f->bar0_base + E1000_REG_STATUS);
        volatile u32 *ral0   = (volatile u32 *)(uptr)(f->bar0_base + E1000_REG_RAL0);
        volatile u32 *rah0   = (volatile u32 *)(uptr)(f->bar0_base + E1000_REG_RAH0);

        g_nic.ctrl     = ctrl[0];
        g_nic.status   = status[0];
        g_nic.mac_lo   = ral0[0];
        g_nic.mac_hi   = rah0[0];
        g_nic.link_up  = (g_nic.status >> 1) & 1u;   /* STATUS.LU */
    }

    /* 全 1 是"没真读到"的典型读数，不能当成功 */
    g_nic.reg_ok = ((g_nic.ctrl != 0xFFFFFFFFu) && (g_nic.status != 0xFFFFFFFFu)) ? 1u : 0u;
}

static u32 conn_add(u32 state, const char *proto, u16 port, const char *name)
{
    u32 i;

    for (i = 0u; i < HYN_NET_MAX_CONNS; i++) {
        if (g_conns[i].used == 0u) {
            g_conns[i].used        = 1u;
            g_conns[i].state       = state;
            g_conns[i].local_port  = port;
            g_conns[i].remote_port = 0u;
            g_conns[i].remote_addr = 0u;
            g_conns[i].rx_bytes    = 0u;
            g_conns[i].tx_bytes    = 0u;      /* 没有收发路径：绝不编造字节数 */
            (void)strncpy_safe(g_conns[i].proto, proto, HYN_NET_PROTO_MAX);
            (void)strncpy_safe(g_conns[i].name,  name,  HYN_NET_ADDR_MAX);
            g_conn_count++;
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

/* ---- 生命周期 ---- */

hyn_status_t net_quiesce(void)
{
    memset(g_conns, 0, (unsigned long)sizeof(g_conns));
    memset(&g_nic, 0, (unsigned long)sizeof(g_nic));
    g_conn_count = 0u;
    return HYN_OK;
}

hyn_status_t net_reset(void)
{
    u32 i;

    (void)net_quiesce();
    nic_claim();

    /* 协议栈骨架：登记 LISTEN 入口。状态一律 LISTEN——不伪造 ESTABLISHED。 */
    for (i = 0u; i < (u32)HYN_ARRAY_SIZE(g_listen_seeds); i++) {
        (void)conn_add((u32)HYN_NET_CONN_LISTEN, g_listen_seeds[i].proto,
                       g_listen_seeds[i].port, g_listen_seeds[i].name);
    }
    return HYN_OK;
}

hyn_status_t net_init(void)
{
    return net_reset();
}

/* ---- 取证访问器 ---- */

u32 net_conn_count(void) { return g_conn_count; }

u32 net_listen_count(void)
{
    u32 i, n = 0u;

    for (i = 0u; i < HYN_NET_MAX_CONNS; i++) {
        if (g_conns[i].used != 0u && g_conns[i].state == (u32)HYN_NET_CONN_LISTEN) {
            n++;
        }
    }
    return n;
}

/*
 * 骨架阶段恒为 0：连接表里没有任何 ESTABLISHED 槽位，也没有实现握手/收发路径，
 * 因此这里如实返回 0，而不是拿 LISTEN 数冒充"已建立的连接"。
 */
u32 net_established_count(void)
{
    u32 i, n = 0u;

    for (i = 0u; i < HYN_NET_MAX_CONNS; i++) {
        if (g_conns[i].used != 0u && g_conns[i].state == (u32)HYN_NET_CONN_ESTABLISHED) {
            n++;
        }
    }
    return n;
}

const hyn_net_conn_t *net_conn(u32 idx)
{
    if (idx >= HYN_NET_MAX_CONNS || g_conns[idx].used == 0u) {
        return NULL;
    }
    return &g_conns[idx];
}

const hyn_net_nic_regs_t *net_nic_regs(void)
{
    return &g_nic;
}

/* ---- IPv4 首部校验和：按 RFC 791 的 16 位反码求和真算 ---- */

u32 net_ip_checksum(const u8 *hdr, u32 len)
{
    u32 sum = 0u;
    u32 i;

    if (hdr == NULL || len < 2u) {
        return 0xFFFFu;                  /* 无法计算：返回全 1（调用方据此判不可用） */
    }
    for (i = 0u; i + 1u < len; i += 2u) {
        sum += ((u32)hdr[i] << 8) | (u32)hdr[i + 1u];
    }
    if (i < len) {
        sum += ((u32)hdr[i] << 8);       /* 奇数长度：末字节按高位对齐补齐 */
    }
    while ((sum >> 16) != 0u) {
        sum = (sum & 0xFFFFu) + (sum >> 16);
    }
    return (~sum) & 0xFFFFu;
}