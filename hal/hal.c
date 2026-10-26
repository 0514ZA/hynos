/*
 * HynOS —— HAL 设备注册中心与控制台汇聚
 *
 * 职责边界：HAL 只做"最简底层外设驱动 + 设备登记"，不承载文件系统/网络协议栈/业务逻辑。
 * 故障域（存储组/网络组/硬件总线组）通过 hal_device_probe_domain() 认领各自设备，
 * 单组重置时对组内设备调用 hal_device_quiesce_domain() / hal_device_reset_domain()，
 * 不影响内核底座（底座设备归属 CORE，HAL 拒绝静默/重置它们）。
 *
 * 探测是"真探测"：PCI 设备靠 arch 层 0xCF8/0xCFC 配置空间枚举得出，找不到就如实记
 * ABSENT（结论），绝不用"声明就绪"代替"确实看到硬件"。
 */
#include <hyn/hal.h>
#include <hyn/kprintf.h>
#include <hyn/kstring.h>
#include <hyn/arch.h>

static hal_device_t *g_devices;
static u32 g_device_count;

static u64 g_ticks;
static u64 g_ticks_per_sec = 100;   /* 时钟中断频率，T2 接入定时器后据此换算 */

/* PCI 枚举缓存：HAL 与硬件总线组共用同一份"最近一次扫描结果"，避免重复扫总线 */
static hyn_pci_dev_t g_pci_table[HYN_PCI_MAX_DEVICES];
static u32 g_pci_count;
static u32 g_pci_scans;

/* 各设备的探测事实（静态事实结构，避免为此引入动态分配） */
static hal_pci_fact_t g_fact_usb;
static hal_pci_fact_t g_fact_hda;
static hal_pci_fact_t g_fact_nvme;
static hal_pci_fact_t g_fact_net;

/* ---------------- 控制台 ---------------- */

hyn_status_t hal_console_init(void)
{
    hyn_status_t st = hal_fb_init();
    if (st != HYN_OK) {
        return st;
    }
    return hal_serial_init();
}

void hal_console_putc(char c)
{
    if (hal_serial_ready()) {
        hal_serial_putc(c);
    }
    hal_fb_putc(c);
}

void hal_console_write(const char *s, u64 n)
{
    for (u64 i = 0; i < n; i++) {
        hal_console_putc(s[i]);
    }
}

void hal_console_sink(char c, void *ctx)
{
    (void)ctx;
    hal_console_putc(c);
}

/* ---------------- 时间基准 ---------------- */

u64 hal_ticks(void) { return g_ticks; }
u64 hal_ticks_per_sec(void) { return g_ticks_per_sec; }

void hal_timer_tick(void)
{
    g_ticks++;
}

/* ---------------- 设备注册中心 ---------------- */

hyn_status_t hal_register_device(hal_device_t *dev)
{
    if (dev == NULL || dev->name == NULL) {
        return HYN_EINVAL;
    }
    if (hal_find_device(dev->name) != NULL) {
        return HYN_EBUSY;
    }
    dev->next = g_devices;
    g_devices = dev;
    dev->state = HAL_DEV_STATE_REGISTERED;
    g_device_count++;
    return HYN_OK;
}

hal_device_t *hal_find_device(const char *name)
{
    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (strcmp(d->name, name) == 0) {
            return d;
        }
    }
    return NULL;
}

hal_device_t *hal_device_by_kind(hal_dev_kind_t kind)
{
    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (d->kind == kind) {
            return d;
        }
    }
    return NULL;
}

const hal_pci_fact_t *hal_device_fact(const hal_device_t *dev)
{
    return (dev != NULL) ? (const hal_pci_fact_t *)dev->priv : NULL;
}

void hal_foreach_device(void (*cb)(hal_device_t *dev, void *ctx), void *ctx)
{
    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        cb(d, ctx);
    }
}

u32 hal_device_count(void) { return g_device_count; }

hyn_status_t hal_device_bind_domain(hal_device_t *dev, hyn_domain_t domain)
{
    if (dev == NULL || (u32)domain >= (u32)HYN_DOMAIN_MAX) {
        return HYN_EINVAL;
    }
    dev->domain = (u8)domain;
    return HYN_OK;
}

u8 hal_device_domain(const hal_device_t *dev)
{
    return (dev != NULL) ? dev->domain : (u8)HYN_DOMAIN_CORE;
}

u32 hal_device_count_domain(hyn_domain_t domain)
{
    u32 n = 0u;

    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (d->domain == (u8)domain) {
            n++;
        }
    }
    return n;
}

u32 hal_device_ready_count(hyn_domain_t domain)
{
    u32 n = 0u;

    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (d->domain == (u8)domain && d->state == HAL_DEV_STATE_READY) {
            n++;
        }
    }
    return n;
}

u32 hal_device_probe_domain(hyn_domain_t domain)
{
    u32 ready = 0u;

    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (d->domain != (u8)domain) {
            continue;
        }
        d->probe_count++;
        if (d->probe != NULL) {
            (void)d->probe(d);
        }
        if (d->state == HAL_DEV_STATE_READY) {
            ready++;
        }
    }
    return ready;
}

/*
 * 静默/重置底座设备是被明确禁止的：控制台与帧缓冲要是也被"重置"，内核连日志都没了，
 * 这不是隔离而是自毁。因此 HAL 对 CORE 域直接拒绝，并如实返回 0。
 */
u32 hal_device_quiesce_domain(hyn_domain_t domain)
{
    u32 n = 0u;

    if ((u32)domain >= (u32)HYN_DOMAIN_MAX || domain == HYN_DOMAIN_CORE) {
        return 0u;
    }
    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (d->domain != (u8)domain) {
            continue;
        }
        if (d->shutdown != NULL) {
            (void)d->shutdown(d);
        } else {
            d->state = HAL_DEV_STATE_QUIESCED;
        }
        n++;
    }
    return n;
}

u32 hal_device_reset_domain(hyn_domain_t domain)
{
    u32 ready = 0u;

    if ((u32)domain >= (u32)HYN_DOMAIN_MAX || domain == HYN_DOMAIN_CORE) {
        return 0u;
    }
    for (hal_device_t *d = g_devices; d != NULL; d = d->next) {
        if (d->domain != (u8)domain) {
            continue;
        }
        d->reset_count++;
        d->probe_count++;
        if (d->reset != NULL) {
            (void)d->reset(d);
        } else if (d->probe != NULL) {
            (void)d->probe(d);
        }
        if (d->state == HAL_DEV_STATE_READY) {
            ready++;
        }
    }
    return ready;
}

const char *hal_device_state_name(hal_dev_state_t s)
{
    switch (s) {
    case HAL_DEV_STATE_EMPTY:      return "空";
    case HAL_DEV_STATE_REGISTERED: return "已登记(待所属分组探测)";
    case HAL_DEV_STATE_READY:      return "就绪";
    case HAL_DEV_STATE_FAILED:     return "失败";
    case HAL_DEV_STATE_ABSENT:     return "本机无此硬件";
    case HAL_DEV_STATE_QUIESCED:   return "静默(所属分组已重置)";
    default:                       return "未知";
    }
}

const char *hal_device_kind_name(hal_dev_kind_t k)
{
    switch (k) {
    case HAL_DEV_FRAMEBUFFER: return "帧缓冲";
    case HAL_DEV_USB:         return "USB";
    case HAL_DEV_HDA:         return "HDA 音频";
    case HAL_DEV_NVME:        return "NVMe";
    case HAL_DEV_NET:         return "网卡";
    default:                  return "未知";
    }
}

/* ---------------- PCI 枚举共享 ---------------- */

u32 hal_pci_rescan(void)
{
    g_pci_count = hyn_pci_enumerate(g_pci_table, HYN_PCI_MAX_DEVICES);
    g_pci_scans++;
    return g_pci_count;
}

const hyn_pci_dev_t *hal_pci_table(u32 *out_count)
{
    if (out_count != NULL) {
        *out_count = g_pci_count;
    }
    return g_pci_table;
}

u32 hal_pci_scan_count(void) { return g_pci_scans; }

static const hyn_pci_dev_t *pci_first_of_class(u8 cls, u8 sub)
{
    u32 from = 0u;

    return hyn_pci_find_class(g_pci_table, g_pci_count, cls, sub, &from);
}

static void fact_fill(hal_pci_fact_t *fact, const hyn_pci_dev_t *d)
{
    memset(fact, 0, (unsigned long)sizeof(*fact));
    fact->found     = 1u;
    fact->vendor    = d->vendor;
    fact->device    = d->device;
    fact->bus       = d->bus;
    fact->dev       = d->dev;
    fact->func      = d->func;
    fact->bar0_size = d->bar0_size;
    fact->bar0_raw  = d->bar0_raw;
    fact->bar0_base = d->bar0_base;
}

/*
 * 类别设备探测：在共享的枚举结果里找该类别设备。找到 -> READY（身份与 BAR 已知，
 * 这才算 HAL 这一层的最简初始化完成）；找不到 -> ABSENT（如实结论，不是失败）。
 */
static hyn_status_t class_probe(hal_device_t *dev, u8 cls, u8 sub, hal_pci_fact_t *fact)
{
    const hyn_pci_dev_t *d = pci_first_of_class(cls, sub);

    if (d == NULL) {
        memset(fact, 0, (unsigned long)sizeof(*fact));
        dev->priv  = fact;
        dev->state = HAL_DEV_STATE_ABSENT;
        return HYN_ENOENT;
    }
    fact_fill(fact, d);
    dev->priv  = fact;
    dev->state = HAL_DEV_STATE_READY;
    return HYN_OK;
}

/* ---------------- 设备实例（骨架 + PCI 类别探测） ---------------- */

static hyn_status_t usb_probe(hal_device_t *dev)  { return class_probe(dev, HYN_PCI_CLASS_SERIAL, HYN_PCI_SUB_SERIAL_USB, &g_fact_usb); }
static hyn_status_t hda_probe(hal_device_t *dev)  { return class_probe(dev, HYN_PCI_CLASS_MULTIMEDIA, HYN_PCI_SUB_MM_AUDIO, &g_fact_hda); }
static hyn_status_t nvme_probe(hal_device_t *dev) { return class_probe(dev, HYN_PCI_CLASS_STORAGE, HYN_PCI_SUB_STORAGE_NVM, &g_fact_nvme); }
static hyn_status_t net_probe(hal_device_t *dev)  { return class_probe(dev, HYN_PCI_CLASS_NETWORK, HYN_PCI_SUB_NET_ETHER, &g_fact_net); }

/* 重置 = 在静默之后重新走一遍探测（硬件可能换了，重新扫总线再找一次） */
static hyn_status_t usb_reset(hal_device_t *dev)
{
    (void)hal_pci_rescan();
    return usb_probe(dev);
}

static hyn_status_t hda_reset(hal_device_t *dev)
{
    (void)hal_pci_rescan();
    return hda_probe(dev);
}

static hyn_status_t nvme_reset(hal_device_t *dev)
{
    (void)hal_pci_rescan();
    return nvme_probe(dev);
}

static hyn_status_t net_reset(hal_device_t *dev)
{
    (void)hal_pci_rescan();
    return net_probe(dev);
}

/* 静默：设备停止对外服务（本轮没有收发环可停，故只改状态并如实说明） */
static hyn_status_t class_shutdown(hal_device_t *dev)
{
    dev->state = HAL_DEV_STATE_QUIESCED;
    return HYN_OK;
}

hal_device_t hal_dev_usb = {
    .name = "usb.xhci", .kind = HAL_DEV_USB, .state = HAL_DEV_STATE_EMPTY,
    .domain = (u8)HYN_DOMAIN_BUS,
    .probe = usb_probe, .reset = usb_reset, .shutdown = class_shutdown,
    .priv = NULL, .next = NULL,
};

hal_device_t hal_dev_hda = {
    .name = "hda.codec", .kind = HAL_DEV_HDA, .state = HAL_DEV_STATE_EMPTY,
    .domain = (u8)HYN_DOMAIN_BUS,
    .probe = hda_probe, .reset = hda_reset, .shutdown = class_shutdown,
    .priv = NULL, .next = NULL,
};

hal_device_t hal_dev_nvme = {
    .name = "nvme.0", .kind = HAL_DEV_NVME, .state = HAL_DEV_STATE_EMPTY,
    .domain = (u8)HYN_DOMAIN_STORAGE,
    .probe = nvme_probe, .reset = nvme_reset, .shutdown = class_shutdown,
    .priv = NULL, .next = NULL,
};

hal_device_t hal_dev_net = {
    .name = "net.e1000", .kind = HAL_DEV_NET, .state = HAL_DEV_STATE_EMPTY,
    .domain = (u8)HYN_DOMAIN_NET,
    .probe = net_probe, .reset = net_reset, .shutdown = class_shutdown,
    .priv = NULL, .next = NULL,
};

/* ---------------- HAL 初始化 ---------------- */

static void hal_log_device(hal_device_t *dev, void *ctx)
{
    int *count = (int *)ctx;
    (*count)++;
    kprintf("      · HAL 设备 %-15s %-8s 域=%u 状态=%s 探测 %u 次\n",
            dev->name, hal_device_kind_name(dev->kind), (u32)dev->domain,
            hal_device_state_name(dev->state), dev->probe_count);
}

hyn_status_t hal_init(void)
{
    /* 先把显示与串口控制台拉起来，保证后续任何失败都有日志可查 */
    (void)hal_console_init();
    kprintf_add_sink(hal_console_sink, NULL);

    hal_device_t *skeletons[] = {
        &hal_dev_framebuffer,
        &hal_dev_usb,
        &hal_dev_hda,
        &hal_dev_nvme,
        &hal_dev_net,
    };
    for (u32 i = 0; i < (u32)HYN_ARRAY_SIZE(skeletons); i++) {
        (void)hal_register_device(skeletons[i]);
    }

    /*
     * 设备 -> 故障域归属：NVMe 归存储组、网卡归网络组、USB/HDA 归硬件总线组，
     * 帧缓冲归硬核底座（底座设备不可被重置）。
     */
    (void)hal_device_bind_domain(&hal_dev_framebuffer, HYN_DOMAIN_CORE);
    (void)hal_device_bind_domain(&hal_dev_usb,         HYN_DOMAIN_BUS);
    (void)hal_device_bind_domain(&hal_dev_hda,         HYN_DOMAIN_BUS);
    (void)hal_device_bind_domain(&hal_dev_nvme,        HYN_DOMAIN_STORAGE);
    (void)hal_device_bind_domain(&hal_dev_net,         HYN_DOMAIN_NET);

    /* 先扫一次 PCI：此后 HAL 与硬件总线组共用这份结果 */
    (void)hal_pci_rescan();
    kprintf("[HAL ] PCI 总线扫描完成：发现 %u 个设备（%u 次扫描）\n",
            g_pci_count, g_pci_scans);

    /*
     * 只探测底座自身的设备（帧缓冲）：其余设备由所属分组在自己的 probe 里认领，
     * 这样"设备就绪"这件事与分组生命周期绑定，分组重置后设备状态也跟着重建。
     */
    (void)hal_device_probe_domain(HYN_DOMAIN_CORE);

    int logged = 0;
    hal_foreach_device(hal_log_device, &logged);
    return HYN_OK;
}