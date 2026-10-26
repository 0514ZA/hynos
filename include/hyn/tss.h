/*
 * 混元 OS —— x86_64 任务状态段（TSS）接口
 *
 * 为什么 64 位下还需要 TSS：硬件要求从 Ring3 切入 Ring0（中断/异常/系统调用）时，
 * CPU 必须去 TSS.rsp0 取内核栈——这是"用户栈不可信"的硬件保障。
 * 本内核据此在每次切换线程时更新 rsp0，使 Ring3 的陷阱帧总是落在该线程自己的内核栈上。
 *
 * 本里程碑只用到 rsp0 与 I/O 位图基址（指向 TSS 末尾 = 无 I/O 位图，故 Ring3 的
 * in/out 指令一律 #GP）。IST 留待需要独立栈的异常（NMI/#MC）时再启用。
 */
#ifndef HYN_TSS_H
#define HYN_TSS_H

#include <hyn/types.h>

/* x86_64 硬件要求的 TSS 布局（大小 104 字节） */
typedef struct {
    u32 reserved0;
    u64 rsp0;                 /* Ring0 栈指针：从 Ring3 进入内核时 CPU 取这里 */
    u64 rsp1;
    u64 rsp2;
    u64 reserved1;
    u64 ist[7];               /* 中断栈表：本里程碑未启用 */
    u64 reserved2;
    u16 reserved3;
    u16 iomap_base;           /* I/O 位图偏移；设成 TSS 大小表示"没有位图" */
} __attribute__((packed)) hyn_tss64_t;

hyn_status_t hyn_tss_init(void);      /* 填 GDT 第 6/7 项并 ltr，必须在 gdt_load 之后调用 */
void         hyn_tss_set_rsp0(u64 rsp0);
u64          hyn_tss_rsp0(void);
int          hyn_tss_ready(void);

#endif /* HYN_TSS_H */