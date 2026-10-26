/*
 * 混元 OS —— 时钟基准接口（架构无关形态）
 *
 * arch 层提供硬件计时（x86_64 用 PIT），core/hal 通过 attach 挂载自己的节拍逻辑，
 * 保持"上层不直接触碰硬件、底层不反向依赖上层"的分层纪律。
 */
#ifndef HYN_TIMER_H
#define HYN_TIMER_H

#include <hyn/types.h>

hyn_status_t hyn_timer_init(u32 hz);
u32          hyn_timer_hz(void);
u64          hyn_timer_ticks(void);
void         hyn_timer_attach_tick(void (*cb)(void));   /* 每个时钟中断回调一次 */

/* 由时钟中断服务例程调用 */
void hyn_timer_tick(void);

#endif /* HYN_TIMER_H */