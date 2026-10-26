/*
 * HynOS —— 硬件总线组故障域（ACPI、PCIe、基础硬件管理）的 ops 适配层
 *
 * 职责边界：
 *   · 本文件只做"分组协议"该做的事：枚举总线、静默本组、重新盘点、上报账目；
 *   · ACPI 表发现与 PCI 清点在 drivers/bus.c；USB/HDA 的设备事实由 HAL 按域探测。
 */
#include <hyn/fault.h>
#include <hyn/bus.h>
#include <hyn/hal.h>
#include <hyn/kstring.h>

static hyn_status_t bs_probe(void)
{
    u32 ready = hal_device_probe_domain(HYN_DOMAIN_BUS);

    if (bus_pci_count() == 0u) {
        (void)bus_init();
    } else {
        (void)bus_reset();
    }

    hyn_domain_report(HYN_DOMAIN_BUS,
                      bus_pci_count() + bus_acpi()->tables,
                      hal_device_count_domain(HYN_DOMAIN_BUS),
                      ready);
    return HYN_OK;
}

static hyn_status_t bs_quiesce(void)
{
    (void)hal_device_quiesce_domain(HYN_DOMAIN_BUS);
    (void)bus_quiesce();
    hyn_domain_report(HYN_DOMAIN_BUS, 0u,
                      hal_device_count_domain(HYN_DOMAIN_BUS), 0u);
    return HYN_OK;
}

static hyn_status_t bs_reset(void)
{
    (void)hal_device_reset_domain(HYN_DOMAIN_BUS);
    return HYN_OK;
}

static hyn_status_t bs_dispatch(u32 op, const u8 *in, u32 in_len, u8 *out, u32 *out_len)
{
    HYN_UNUSED(in);
    HYN_UNUSED(in_len);

    if (out_len == NULL) {
        return HYN_EINVAL;
    }

    if (op == HYN_GROUP_OP_BUS_ENUM) {
        const hyn_bus_acpi_t *a = bus_acpi();
        hyn_bus_probe_t       p;
        u32                   i;

        memset(&p, 0, (unsigned long)sizeof(p));
        p.pci_devices     = bus_pci_count();
        p.pci_bridges     = bus_pci_bridge_count();
        p.acpi_rsdp_found = a->rsdp_found;
        p.acpi_checksum_ok = a->checksum_ok;
        p.acpi_ext_ok     = a->ext_ok;
        p.acpi_revision   = a->revision;
        p.acpi_tables     = a->tables;
        p.acpi_tables_ok  = a->tables_ok;
        p.rsdt_address    = a->rsdt_address;
        for (i = 0u; i < 6u; i++) {
            p.oem_id[i] = a->oem_id[i];
            p.first_table[i] = a->first_table[i];
        }

        if (out == NULL || *out_len < (u32)sizeof(p)) {
            return HYN_EBUSY;
        }
        memcpy(out, &p, (unsigned long)sizeof(p));
        *out_len = (u32)sizeof(p);
        return HYN_OK;
    }

    return HYN_ENOENT;
}

static const hyn_domain_ops_t g_bus_ops = {
    .name     = "硬件总线组",
    .probe    = bs_probe,
    .quiesce  = bs_quiesce,
    .reset    = bs_reset,
    .dispatch = bs_dispatch,
};

const hyn_domain_ops_t *hyn_group_bus_ops(void)
{
    return &g_bus_ops;
}