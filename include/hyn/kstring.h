/*
 * HynOS —— 内核基础字符串/内存操作（架构无关层）
 *
 * 这里以标准名字实现 memcpy/memset/memmove/memcmp/strlen 等，
 * 因为 freestanding 编译时编译器会自行生成对这些符号的调用。
 */
#ifndef HYN_KSTRING_H
#define HYN_KSTRING_H

#include <hyn/types.h>

void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);
void *memmove(void *dst, const void *src, unsigned long n);
int   memcmp(const void *a, const void *b, unsigned long n);

unsigned long strlen(const char *s);
int   strcmp(const char *a, const char *b);
int   strncmp(const char *a, const char *b, unsigned long n);
char *strcpy(char *dst, const char *src);

/* 带容量约束的拷贝，始终以 '\0' 结尾 */
char *strncpy_safe(char *dst, const char *src, unsigned long cap);

/* 对象名哈希（对象管理器 / ALPC 端口名索引用） */
u64 kstr_hash(const char *s);

#endif /* HYN_KSTRING_H */