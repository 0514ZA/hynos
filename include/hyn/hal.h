/*
 * 混元 OS —— HAL 硬件抽象层接口
 *
 * 分层要求：本层只提供"最简底层外设驱动"，不放文件系统、网络协议栈等逻辑
 * （那些属于故障域分组）。设备以统一的 hal_device 骨架注册，便于故障域按组重置。
 *
 * 与故障域的关系（本轮实装）：
 *   · 每个设备带"所属故障域"标签：NVMe -> 存储组、网卡 -> 网络组、USB/HDA -> 硬件总线组、
 *     帧缓冲 -> 硬核底座（底座设备不可被重置，否则连日志都没了）；
 *   · 设备的探测由所属分组驱动：分组 probe 时调用 hal_device_probe_domain()，
 *     分组被重置时调用 hal_device_quiesce_domain() / hal_device_reset_domain()；
 *   · HAL 只回答"看到了什么、能不能用"（探测事实结构 hal_pci_fact_t），
 *     设备功能初始化（NVMe 队列、e1000 收发环等）留给后续里程碑，不在此虚报。
 */
#ifndef HYN_HAL_H
#define HYN_HAL_H

#include <hyn/types.h>
#include <hyn/object.h>
#include <hyn/pci.h>

/* ================= 控制台 ================= */
hyn_status_t hal_console_init(void);
void         hal_console_putc(char c);
void         hal_console_write(const char *s, u64 n);
void         hal_console_sink(char c, void *ctx);   /* 供 kprintf 注册的输出汇 */

/* ================= 时间基准 ================= */
u64 hal_ticks(void);
u64 hal_ticks_per_sec(void);
void hal_timer_tick(void);          /* 由时钟中断调用 */

/* ================= 设备模型 ================= */
typedef enum {
    HAL_DEV_UNKNOWN = 0,
    HAL_DEV_FRAMEBUFFER,
    HAL_DEV_USB,
    HAL_DEV_HDA,
    HAL_DEV_NVME,
    HAL_DEV_NET,
} hal_dev_kind_t;

typedef enum {
    HAL_DEV_STATE_EMPTY = 0,
    HAL_DEV_STATE_REGISTERED,   /* 已登记，等待所属故障域探测 */
    HAL_DEV_STATE_READY,        /* 探测通过：硬件确实在，且已完成最简初始化 */
    HAL_DEV_STATE_FAILED,       /* 探测到了但初始化失败 */
    HAL_DEV_STATE_ABSENT,       /* 本机不存在该硬件：这是探测结论，不是错误 */
    HAL_DEV_STATE_QUIESCED,     /* 所属故障域被重置期间：设备已静默 */
} hal_dev_state_t;

/* 探测事实：HAL 填、上层只读。只给"看到了什么"，不给任何"能做什么"的承诺 */
typedef struct hal_pci_fact {
    u32 found;                  /* 1 = 在本机找到该类别设备 */
    u16 vendor;
    u16 device;
    u8  bus, dev, func;
    u8  reserved;
    u32 bar0_size;
    u32 bar0_raw;
    u64 bar0_base;
} hal_pci_fact_t;

typedef struct hal_device hal_device_t;
struct hal_device {
    const char        *name;
    hal_dev_kind_t     kind;
    hal_dev_state_t    state;
    u8                 domain;      /* 所属故障域（hyn_domain_t） */
    u8                 reserved[3];
    u32                probe_count; /* 探测次数：分组重置后重新探测会自增 */
    u32                reset_count; /* 被所属分组重置的次数 */
    hyn_status_t     (*probe)(hal_device_t *dev);
    hyn_status_t     (*reset)(hal_device_t *dev);
    hyn_status_t     (*shutdown)(hal_device_t *dev);
    void              *priv;        /* 探测事实（hal_pci_fact_t，见 hal_device_fact） */
    hal_device_t      *next;
};

hyn_status_t   hal_init(void);
hyn_status_t   hal_register_device(hal_device_t *dev);
hal_device_t  *hal_find_device(const char *name);
hal_device_t  *hal_device_by_kind(hal_dev_kind_t kind);
const hal_pci_fact_t *hal_device_fact(const hal_device_t *dev);
void           hal_foreach_device(void (*cb)(hal_device_t *dev, void *ctx), void *ctx);
u32            hal_device_count(void);

/* ---- 故障域绑定与按域操作 ---- */
hyn_status_t   hal_device_bind_domain(hal_device_t *dev, hyn_domain_t domain);
u8             hal_device_domain(const hal_device_t *dev);
u32            hal_device_probe_domain(hyn_domain_t domain);   /* 返回探测后就绪的设备数 */
u32            hal_device_quiesce_domain(hyn_domain_t domain); /* 让该域设备静默 */
u32            hal_device_reset_domain(hyn_domain_t domain);   /* 重新探测该域设备 */
u32            hal_device_ready_count(hyn_domain_t domain);
u32            hal_device_count_domain(hyn_domain_t domain);
const char    *hal_device_state_name(hal_dev_state_t s);
const char    *hal_device_kind_name(hal_dev_kind_t k);

/* ---- PCI 枚举共享（HAL 与硬件总线组共用一次扫描结果） ---- */
u32                  hal_pci_rescan(void);                   /* 重新枚举，返回设备数 */
const hyn_pci_dev_t *hal_pci_table(u32 *out_count);          /* 只读的最近一次枚举结果 */
u32                  hal_pci_scan_count(void);

/* 设备骨架实例 */
extern hal_device_t hal_dev_framebuffer;
extern hal_device_t hal_dev_usb;
extern hal_device_t hal_dev_hda;
extern hal_device_t hal_dev_nvme;
extern hal_device_t hal_dev_net;

/* ================= 后端（HAL 内部实现） ================= */
hyn_status_t hal_serial_init(void);
int          hal_serial_ready(void);
void         hal_serial_putc(char c);

hyn_status_t hal_fb_init(void);
void         hal_fb_putc(char c);
void         hal_fb_clear(void);
u32          hal_fb_width(void);
u32          hal_fb_height(void);

#endif /* HYN_HAL_H */