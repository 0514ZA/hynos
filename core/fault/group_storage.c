/*
 * HynOS —— 存储组故障域（NVMe 驱动 + VFS）的 ops 适配层
 *
 * 职责边界：
 *   · 本文件只做"分组协议"该做的事：认领本组硬件、静默本组资源、重建本组结构、上报账目；
 *   · 真正的设备与文件系统逻辑在 hal/（NVMe 事实）与 drivers/vfs.c（VFS 骨架）里；
 *   · 组内不直接调别的组：内层跨组一律走 hyn_domain_call（ALPC 消息往返）。
 */
#include <hyn/fault.h>
#include <hyn/vfs.h>
#include <hyn/hal.h>
#include <hyn/pci.h>
#include <hyn/alpc.h>
#include <hyn/kstring.h>

/* 认领真实硬件 + 按当前事实重建 VFS 骨架 + 上报账目 */
static hyn_status_t st_probe(void)
{
    u32 ready = hal_device_probe_domain(HYN_DOMAIN_STORAGE);

    /*
     * 静默（或首次启动）之后本组节点表是空的：此时走 vfs_init 重建骨架并重新认领后端；
     * 已有骨架时走 vfs_reset，先清空再按当前硬件重认领。两条路径都不伪造节点内容。
     */
    if (vfs_node_count() == 0u) {
        (void)vfs_init();
    } else {
        (void)vfs_reset();
    }

    hyn_domain_report(HYN_DOMAIN_STORAGE,
                      vfs_node_count() + vfs_mount_count(),
                      hal_device_count_domain(HYN_DOMAIN_STORAGE),
                      ready);
    return HYN_OK;
}

/* 静默：停本组设备、清空本组资源账目（quiesce 之后本组不留活着的资源） */
static hyn_status_t st_quiesce(void)
{
    (void)hal_device_quiesce_domain(HYN_DOMAIN_STORAGE);
    (void)vfs_quiesce();
    hyn_domain_report(HYN_DOMAIN_STORAGE, 0u,
                      hal_device_count_domain(HYN_DOMAIN_STORAGE), 0u);
    return HYN_OK;
}

/* 重置：重新扫总线、重新认领本组设备（软件结构重建在随后的 probe 里做） */
static hyn_status_t st_reset(void)
{
    (void)hal_device_reset_domain(HYN_DOMAIN_STORAGE);
    return HYN_OK;
}

static hyn_status_t st_dispatch(u32 op, const u8 *in, u32 in_len, u8 *out, u32 *out_len)
{
    HYN_UNUSED(in);
    HYN_UNUSED(in_len);

    if (out_len == NULL) {
        return HYN_EINVAL;
    }

    if (op == HYN_GROUP_OP_STORAGE_PROBE) {
        const hyn_nvme_regs_t *r = vfs_nvme_regs();
        hyn_storage_probe_t    p;

        memset(&p, 0, (unsigned long)sizeof(p));
        p.nvme_found = r->found;
        p.reg_ok     = r->reg_ok;
        p.bar0_size  = r->bar0_size;
        p.pci_vendor = r->vendor;
        p.pci_device = r->device;
        p.cap_lo     = r->cap_lo;
        p.cap_hi     = r->cap_hi;
        p.version    = r->version;
        p.csts       = r->csts;
        p.vfs_nodes  = vfs_node_count();
        p.vfs_mounts = vfs_mount_count();

        /* 类别号从共享枚举结果里真读（0x0108 = 存储/NVMe），不做推导 */
        {
            u32 cnt = 0u, from = 0u;
            const hyn_pci_dev_t *t = hal_pci_table(&cnt);
            const hyn_pci_dev_t *d = hyn_pci_find_class(t, cnt, HYN_PCI_CLASS_STORAGE,
                                                        HYN_PCI_SUB_STORAGE_NVM, &from);
            if (d != NULL) {
                p.nvme_class = ((u32)d->class_code << 8) | (u32)d->subclass;
            }
        }

        if (out == NULL || *out_len < (u32)sizeof(p)) {
            return HYN_EBUSY;
        }
        memcpy(out, &p, (unsigned long)sizeof(p));
        *out_len = (u32)sizeof(p);
        return HYN_OK;
    }

    if (op == HYN_GROUP_OP_STORAGE_CHAIN) {
        hyn_storage_chain_t c;
        hyn_group_ping_t    ping;
        u32                 echo = 0x53544F52u;      /* 'STOR'：核对参数真的过了消息总线 */
        u32                 plen = 0u;
        hyn_status_t        st;

        memset(&c, 0, (unsigned long)sizeof(c));
        memset(&ping, 0, (unsigned long)sizeof(ping));

        /* 调用前先记下"被拒绝的跨域调用"累计值，供自检核对这一次有没有被拒 */
        c.chain_denied_before = hyn_domain_call_denied();

        /*
         * 组到组也必须走 ALPC：这里用 hyn_domain_call（消息往返），
         * 绝不用 hyn_group_local_call —— 后者对跨域直接返回 HYN_EDOMAIN。
         */
        st = hyn_domain_call(HYN_DOMAIN_STORAGE, HYN_DOMAIN_NET, HYN_GROUP_OP_PING,
                             &echo, 4u, &ping, (u32)sizeof(ping), &plen);
        c.chain_ok = (u32)st;
        if (st == HYN_OK && plen == (u32)sizeof(ping)) {
            c.net_ping = ping;
        }

        if (out == NULL || *out_len < (u32)sizeof(c)) {
            return HYN_EBUSY;
        }
        memcpy(out, &c, (unsigned long)sizeof(c));
        *out_len = (u32)sizeof(c);
        return HYN_OK;
    }

    return HYN_ENOENT;
}

static const hyn_domain_ops_t g_storage_ops = {
    .name      = "存储组",
    .probe     = st_probe,
    .quiesce   = st_quiesce,
    .reset     = st_reset,
    .dispatch  = st_dispatch,
};

const hyn_domain_ops_t *hyn_group_storage_ops(void)
{
    return &g_storage_ops;
}