/*
 * HynOS —— 网络组故障域（网卡驱动 + TCP/IP 骨架）的 ops 适配层
 *
 * 职责边界：
 *   · 本文件只做"分组协议"该做的事：认领网卡、静默协议栈、重建连接表骨架、上报账目；
 *   · 网卡寄存器取证与 IPv4 校验和计算在 drivers/net.c；连接表只登记 LISTEN，不伪造已建立连接。
 */
#include <hyn/fault.h>
#include <hyn/net.h>
#include <hyn/hal.h>
#include <hyn/kstring.h>

static hyn_status_t ne_probe(void)
{
    u32 ready = hal_device_probe_domain(HYN_DOMAIN_NET);

    if (net_conn_count() == 0u) {
        (void)net_init();
    } else {
        (void)net_reset();
    }

    hyn_domain_report(HYN_DOMAIN_NET,
                      net_conn_count(),
                      hal_device_count_domain(HYN_DOMAIN_NET),
                      ready);
    return HYN_OK;
}

static hyn_status_t ne_quiesce(void)
{
    (void)hal_device_quiesce_domain(HYN_DOMAIN_NET);
    (void)net_quiesce();
    hyn_domain_report(HYN_DOMAIN_NET, 0u,
                      hal_device_count_domain(HYN_DOMAIN_NET), 0u);
    return HYN_OK;
}

static hyn_status_t ne_reset(void)
{
    (void)hal_device_reset_domain(HYN_DOMAIN_NET);
    return HYN_OK;
}

static hyn_status_t ne_dispatch(u32 op, const u8 *in, u32 in_len, u8 *out, u32 *out_len)
{
    HYN_UNUSED(in);
    HYN_UNUSED(in_len);

    if (out_len == NULL) {
        return HYN_EINVAL;
    }

    if (op == HYN_GROUP_OP_NET_PROBE) {
        const hyn_net_nic_regs_t *r = net_nic_regs();
        hyn_net_probe_t           p;
        u8                        hdr[20];

        memset(&p, 0, (unsigned long)sizeof(p));
        p.nic_found = r->found;
        p.reg_ok    = r->reg_ok;
        p.bar0_size = r->bar0_size;
        p.pci_vendor = r->vendor;
        p.pci_device = r->device;
        p.mac_hi    = r->mac_hi;
        p.mac_lo    = r->mac_lo;
        p.link_up   = r->link_up;
        p.ctrl_status_raw = r->ctrl ^ r->status;   /* 只为"确实读到了两个寄存器"留痕 */
        p.conns     = net_conn_count();

        /*
         * 造一个标准 IPv4 首部（192.168.1.10 -> 10.0.0.1，TCP，20 字节），
         * 真算校验和并回填，再算一次：结果必须是 0 —— 这才叫"校验和是对的"。
         */
        memset(hdr, 0, (unsigned long)sizeof(hdr));
        hdr[0]  = 0x45u;                  /* 版本 4 / 首部长 20 */
        hdr[1]  = 0x00u;
        hdr[2]  = 0x00u; hdr[3]  = 0x14u; /* 总长 20 */
        hdr[4]  = 0x1Cu; hdr[5]  = 0x46u; /* 标识 */
        hdr[6]  = 0x40u; hdr[7]  = 0x00u; /* 不分片 */
        hdr[8]  = 64u;                    /* TTL */
        hdr[9]  = 6u;                     /* TCP */
        hdr[10] = 0x00u; hdr[11] = 0x00u; /* 校验和先置 0 */
        hdr[12] = 192u; hdr[13] = 168u; hdr[14] = 1u;   hdr[15] = 10u;
        hdr[16] = 10u;  hdr[17] = 0u;   hdr[18] = 0u;   hdr[19] = 1u;

        p.cksum_expected = net_ip_checksum(hdr, 20u);
        hdr[10] = (u8)((p.cksum_expected >> 8) & 0xFFu);
        hdr[11] = (u8)(p.cksum_expected & 0xFFu);
        p.cksum_result = net_ip_checksum(hdr, 20u);   /* 应为 0 */

        if (out == NULL || *out_len < (u32)sizeof(p)) {
            return HYN_EBUSY;
        }
        memcpy(out, &p, (unsigned long)sizeof(p));
        *out_len = (u32)sizeof(p);
        return HYN_OK;
    }

    return HYN_ENOENT;
}

static const hyn_domain_ops_t g_net_ops = {
    .name     = "网络组",
    .probe    = ne_probe,
    .quiesce  = ne_quiesce,
    .reset    = ne_reset,
    .dispatch = ne_dispatch,
};

const hyn_domain_ops_t *hyn_group_net_ops(void)
{
    return &g_net_ops;
}