/*
 * HynOS —— 内核格式化输出
 *
 * 设计：kprintf 维护一组"输出汇"（sink），HAL 在初始化时把串口与显示控制台注册进来，
 * 因此同一条日志会同时出现在 QEMU 串口与 VGA 文本屏上，便于自检抓取。
 */
#ifndef HYN_KPRINTF_H
#define HYN_KPRINTF_H

#include <hyn/types.h>

typedef void (*kputc_fn)(char c, void *ctx);

#define KPRINTF_MAX_SINKS 4

/* 注册/清空输出汇 */
int  kprintf_add_sink(kputc_fn fn, void *ctx);
void kprintf_clear_sinks(void);

/* 底层输出 */
void kputc(char c);
void kputs(const char *s);
void kwrite(const char *s, u64 n);

/* 格式化输出 */
int  ksnprintf(char *buf, u64 cap, const char *fmt, ...);
int  kvsnprintf(char *buf, u64 cap, const char *fmt, __builtin_va_list ap);
void kprintf(const char *fmt, ...);
void kvprintf(const char *fmt, __builtin_va_list ap);

/* 内核致命错误：打印并停机 */
void kpanic(const char *fmt, ...) __attribute__((noreturn));
void kpanic_status(const char *tag, hyn_status_t st) __attribute__((noreturn));

#endif /* HYN_KPRINTF_H */