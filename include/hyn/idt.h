/*
 * 混元 OS —— 中断描述符表（IDT）接口
 */
#ifndef HYN_IDT_H
#define HYN_IDT_H

#include <hyn/types.h>

#define HYN_IDT_ENTRIES 256

/* 中断门类型属性：存在 + DPL0 + 64 位中断门 */
#define HYN_IDT_ATTR_INT_GATE  0x8E
/* 陷阱门（异常默认）：不关中断 */
#define HYN_IDT_ATTR_TRAP_GATE 0x8F

/*
 * 用户可进入的中断门（DPL=3）：全内核只给 syscall 一个向量用这个属性，
 * 于是"syscall 是 Ring3 进入内核的唯一入口"在硬件层就能被核对——
 * 自检直接遍历 IDT 统计 DPL=3 的门数，必须是 1，且落在 0x80。
 */
#define HYN_IDT_ATTR_USER_INT_GATE 0xEE

typedef struct {
    u16 limit;
    u64 base;
} __attribute__((packed)) hyn_idtr_t;

void hyn_idt_set_gate(u32 vector, u64 handler, u16 selector, u8 attr, u8 ist);
void hyn_idt_init(void);
u64  hyn_idt_get_handler(u32 vector);
u8   hyn_idt_get_attr(u32 vector);      /* 门属性字节；P=0 的门返回 0 */
u32  hyn_idt_gate_dpl(u32 vector);      /* 门的 DPL；P=0 的门按 0 处理 */

#endif /* HYN_IDT_H */