/*
 * HynOS —— 内核对象管理器接口（硬核底座）
 *
 * 硬核底座不放驱动、文件系统、网络代码；对象管理器只提供"命名对象 + 句柄"这套
 * 通用骨架，具体对象（端口、事件、线程…）由各功能域注册类型并把对象体交进来。
 *
 * 句柄设计：
 *   句柄值 = (句柄槽位下标 + 1) << 32 | 代次
 *   对象 id  = (对象槽位下标)     << 32 | 代次
 *   槽位复用时代次自增，因此"早已关闭/销毁的旧句柄"永远不会指向新对象——这是
 *   内核最容易踩的悬挂引用坑，用代次在结构上回避，而不是靠调用方自觉。
 */
#ifndef HYN_OBJECT_H
#define HYN_OBJECT_H

#include <hyn/types.h>

#define HYN_OBJ_MAX      128u                /* 对象池容量 */
#define HYN_HANDLE_MAX   64u                 /* 句柄表容量 */
/*
 * 类型注册表容量 —— 类型号是"稳定编号"（0x10=ALPC 端口、0x20=自检对象…），类型表按
 * 类型号直接下标索引，所以容量必须大于最大类型号；这里留足余量，后续新增类型只追加
 * 新编号，不改动已定编号（编号即 ABI）。
 */
#define HYN_CLASS_MAX    64u                 /* 类型注册表容量（须 > 最大类型号） */
#define HYN_OBJ_NAME_MAX HYN_NAME_MAX

/* 对象类型：类型必须先注册才能创建（避免出现"无名字、无清理逻辑"的野对象） */
#define HYN_OBJ_TYPE_INVALID   0u
#define HYN_OBJ_TYPE_ALPC_PORT 0x10u

/*
 * 故障域标签 —— 对应架构里的"快通道分组故障域"。
 * 硬核底座自身是 CORE；存储组/网络组/硬件总线组各自独立，任一组重置不影响其它。
 */
typedef enum hyn_domain {
    HYN_DOMAIN_CORE    = 0,
    HYN_DOMAIN_STORAGE = 1,
    HYN_DOMAIN_NET     = 2,
    HYN_DOMAIN_BUS     = 3,
    HYN_DOMAIN_MAX     = 4
} hyn_domain_t;

typedef u64 hyn_handle_t;        /* 0 表示无效句柄 */
typedef u64 hyn_object_id_t;     /* 0 表示无效对象 */

/* 类型清理回调：对象销毁时由对象管理器调用（如 ALPC 端口注销连接） */
typedef void (*hyn_object_destroy_fn)(void *body, void *ctx);

typedef struct hyn_handle_info {
    hyn_object_id_t object_id;
    u32 type;
    u32 handle_gen;              /* 句柄代次 */
    u32 object_gen;              /* 对象代次 */
    u32 refs;                    /* 当前打开句柄数 */
    u8  domain;
    u8  valid;
    u16 reserved;
} hyn_handle_info_t;

hyn_status_t hyn_obj_init(void);

/* 注册对象类型（含名称与清理回调）；同一 type 重复注册以最后一次为准 */
hyn_status_t hyn_class_register(u32 type, const char *name,
                                hyn_object_destroy_fn destroy, void *ctx);
const char  *hyn_object_class_name(u32 type);

/*
 * 创建对象：对象体由调用方提供（本里程碑用静态池，避免为对象单独引入分配器，
 * 动态分配器待对象专用分配器接入后替换）。创建成功会同时产出一个"内核持有的句柄"。
 */
hyn_status_t hyn_object_create(u32 type, const char *name, hyn_domain_t domain,
                               void *body, u32 body_size,
                               hyn_object_id_t *out_id, hyn_handle_t *out_handle);

hyn_status_t hyn_object_open(const char *name, hyn_handle_t *out_handle);
hyn_status_t hyn_object_lookup(hyn_handle_t h, hyn_object_id_t *out_object_id);
hyn_status_t hyn_object_body(hyn_handle_t h, void **out_body, u32 *out_size);
hyn_status_t hyn_handle_info(hyn_handle_t h, hyn_handle_info_t *out);
hyn_status_t hyn_object_close(hyn_handle_t h);

/* 销毁对象：仍有打开的句柄时返回 HYN_EBUSY；销毁后对象体被清零（不泄漏给下一个使用者） */
hyn_status_t hyn_object_destroy(hyn_object_id_t object_id);

u32 hyn_object_count(void);
u32 hyn_handle_count(void);
u32 hyn_object_total(void);

#endif /* HYN_OBJECT_H */