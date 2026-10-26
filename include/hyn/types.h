/*
 * HynOS —— 基础类型与状态码（架构无关层使用，纯 C，不依赖任何 CPU 头文件）
 */
#ifndef HYN_TYPES_H
#define HYN_TYPES_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef signed char        i8;
typedef signed short       i16;
typedef signed int         i32;
typedef signed long long   i64;

typedef unsigned long      uptr;   /* 与指针同宽 */
typedef long               iptr;
typedef u64                size_t_hyn;

#ifndef NULL
#define NULL ((void *)0)
#endif

typedef int bool_t;
#define HYN_TRUE  1
#define HYN_FALSE 0

#define HYN_ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define HYN_ALIGN_UP(x, a)   (((x) + ((a) - 1)) & ~((u64)(a) - 1))
#define HYN_ALIGN_DOWN(x, a) ((x) & ~((u64)(a) - 1))
#define HYN_UNUSED(x) ((void)(x))

#define HYN_CONTAINER_OF(ptr, type, member) \
    ((type *)((u8 *)(ptr) - (uptr) & ((type *)0)->member))

/* 状态码：0 表示成功，其余为内核统一错误码 */
typedef u32 hyn_status_t;
#define HYN_OK           0u
#define HYN_EINVAL       1u   /* 参数非法 */
#define HYN_ENOMEM       2u   /* 内存不足 */
#define HYN_ENOENT       3u   /* 对象不存在 */
#define HYN_EPERM        4u   /* 权限不足 */
#define HYN_ENOTSUP      5u   /* 尚未实现（骨架接口默认返回） */
#define HYN_EBUSY        6u   /* 忙碌 */
#define HYN_EFAULT       7u   /* 地址非法 */
#define HYN_EIO          8u   /* 设备/IO 错误 */
#define HYN_EDOMAIN      9u   /* 违反故障域规则（跨域直调等） */
#define HYN_ETIMEDOUT    10u  /* 超时 */
#define HYN_EFAULT_FAULT 11u  /* 被故障域异常捕获接管 */

/* 各层通用上限 */
#define HYN_NAME_MAX     32
#define HYN_PATH_MAX     128
#define HYN_MSG_MAX      256

#endif /* HYN_TYPES_H */