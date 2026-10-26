/*
 * 混元 OS —— 8259A 可编程中断控制器接口（x86_64）
 *
 * 说明：APIC 将在后续里程碑接入；当前阶段用 8259 完成中断投递与 EOI 闭环。
 */
#ifndef HYN_PIC_H
#define HYN_PIC_H

#include <hyn/types.h>

#define HYN_PIC_IRQ_COUNT 16

void hyn_pic_init(void);
void hyn_pic_mask(u32 irq);
void hyn_pic_unmask(u32 irq);
void hyn_pic_eoi(u32 irq);

#endif /* HYN_PIC_H */