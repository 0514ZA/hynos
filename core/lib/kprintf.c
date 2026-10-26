/* 混元 OS —— 内核格式化输出实现（纯 C，架构无关） */
#include <hyn/kprintf.h>
#include <hyn/kstring.h>

typedef struct {
    kputc_fn fn;
    void *ctx;
} kprintf_sink_t;

static kprintf_sink_t g_sinks[KPRINTF_MAX_SINKS];
static int g_sink_count;

/* 架构相关的"直接停机"入口，由 arch 层提供 */
extern void arch_halt_forever(void) __attribute__((noreturn));

int kprintf_add_sink(kputc_fn fn, void *ctx)
{
    if (fn == NULL) {
        return -1;
    }
    /*
     * 幂等去重：同一 (fn, ctx) 若被注册两次（例如 kmain 早期注册一次、
     * hal_init 又注册一次），每个字符会被写出两份。这里直接忽略重复注册。
     */
    for (int i = 0; i < g_sink_count; i++) {
        if (g_sinks[i].fn == fn && g_sinks[i].ctx == ctx) {
            return 0;
        }
    }
    if (g_sink_count >= KPRINTF_MAX_SINKS) {
        return -1;
    }
    g_sinks[g_sink_count].fn = fn;
    g_sinks[g_sink_count].ctx = ctx;
    g_sink_count++;
    return 0;
}

void kprintf_clear_sinks(void)
{
    g_sink_count = 0;
}

void kputc(char c)
{
    for (int i = 0; i < g_sink_count; i++) {
        g_sinks[i].fn(c, g_sinks[i].ctx);
    }
}

void kwrite(const char *s, u64 n)
{
    for (u64 i = 0; i < n; i++) {
        kputc(s[i]);
    }
}

void kputs(const char *s)
{
    while (*s) {
        kputc(*s++);
    }
}

/* ---------------------------------------------------------------- */
/* 轻量格式化内核                                                     */
/* ---------------------------------------------------------------- */

typedef struct {
    char *buf;
    u64   cap;      /* 0 表示直接输出到 sink，不缓存 */
    u64   len;
    int   overflow;
} fmt_out_t;

static void fmt_emit(fmt_out_t *o, char c)
{
    if (o->cap == 0) {
        kputc(c);
        o->len++;
        return;
    }
    if ((o->len + 1) < o->cap) {
        o->buf[o->len] = c;
    } else {
        o->overflow = 1;
    }
    o->len++;
}

static void fmt_emit_str(fmt_out_t *o, const char *s, u64 n)
{
    for (u64 i = 0; i < n; i++) {
        fmt_emit(o, s[i]);
    }
}

static void fmt_emit_uint(fmt_out_t *o, u64 v, u32 base, int upper, int width, char pad)
{
    char tmp[24];
    int n = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v) {
        tmp[n++] = digits[v % base];
        v /= base;
    }
    for (int i = n; i < width; i++) {
        fmt_emit(o, pad);
    }
    while (n > 0) {
        fmt_emit(o, tmp[--n]);
    }
}

static void fmt_emit_int(fmt_out_t *o, i64 v, int width, char pad)
{
    if (v < 0) {
        fmt_emit(o, '-');
        /* 避免 i64 最小值取反溢出 */
        u64 uv = (u64)(-(v + 1)) + 1u;
        fmt_emit_uint(o, uv, 10, 0, width > 0 ? width - 1 : 0, pad);
    } else {
        fmt_emit_uint(o, (u64)v, 10, 0, width, pad);
    }
}

typedef enum { FMT_LEN_NONE, FMT_LEN_LONG, FMT_LEN_LONGLONG } fmt_len_t;

static void fmt_handle(fmt_out_t *o, const char *fmt, __builtin_va_list ap)
{
    while (*fmt) {
        if (*fmt != '%') {
            fmt_emit(o, *fmt++);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            fmt_emit(o, '%');
            fmt++;
            continue;
        }

        /* 标志与宽度 */
        char pad = ' ';
        int width = 0;
        int flags = 1;
        while (flags) {
            switch (*fmt) {
            case '0': pad = '0'; fmt++; break;
            case '-': fmt++; break;          /* 简化实现：忽略左对齐 */
            case '+': fmt++; break;
            case ' ': fmt++; break;
            case '#': fmt++; break;
            default: flags = 0; break;
            }
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* 长度修饰 */
        fmt_len_t len = FMT_LEN_NONE;
        if (*fmt == 'l') {
            fmt++;
            if (*fmt == 'l') {
                fmt++;
                len = FMT_LEN_LONGLONG;
            } else {
                len = FMT_LEN_LONG;
            }
        } else if (*fmt == 'z') {
            fmt++;
            len = FMT_LEN_LONGLONG;
        }

        switch (*fmt) {
        case 's': {
            const char *s = __builtin_va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            u64 n = strlen(s);
            for (int i = (int)n; i < width; i++) {
                fmt_emit(o, ' ');
            }
            fmt_emit_str(o, s, n);
            break;
        }
        case 'c':
            fmt_emit(o, (char)__builtin_va_arg(ap, int));
            break;
        case 'd':
        case 'i': {
            i64 v;
            if (len == FMT_LEN_LONGLONG) {
                v = __builtin_va_arg(ap, i64);
            } else if (len == FMT_LEN_LONG) {
                v = __builtin_va_arg(ap, long);
            } else {
                v = (i64)__builtin_va_arg(ap, int);
            }
            fmt_emit_int(o, v, width, pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            u64 v;
            if (len == FMT_LEN_LONGLONG) {
                v = __builtin_va_arg(ap, u64);
            } else if (len == FMT_LEN_LONG) {
                v = (u64)__builtin_va_arg(ap, unsigned long);
            } else {
                v = (u64)__builtin_va_arg(ap, unsigned int);
            }
            if (*fmt == 'u') {
                fmt_emit_uint(o, v, 10, 0, width, pad);
            } else {
                fmt_emit_uint(o, v, 16, (*fmt == 'X'), width, pad);
            }
            break;
        }
        case 'p': {
            u64 v = (u64)(uptr)__builtin_va_arg(ap, void *);
            fmt_emit(o, '0');
            fmt_emit(o, 'x');
            fmt_emit_uint(o, v, 16, 0, 16, '0');
            break;
        }
        case '\0':
            return;
        default:
            fmt_emit(o, '%');
            fmt_emit(o, *fmt);
            break;
        }
        fmt++;
    }
}

int kvsnprintf(char *buf, u64 cap, const char *fmt, __builtin_va_list ap)
{
    fmt_out_t o = { buf, cap, 0, 0 };
    fmt_handle(&o, fmt, ap);
    if (cap > 0) {
        u64 end = (o.len < (cap - 1)) ? o.len : (cap - 1);
        buf[end] = '\0';
    }
    return (int)o.len;
}

int ksnprintf(char *buf, u64 cap, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int n = kvsnprintf(buf, cap, fmt, ap);
    __builtin_va_end(ap);
    return n;
}

void kvprintf(const char *fmt, __builtin_va_list ap)
{
    fmt_out_t o = { NULL, 0, 0, 0 };
    fmt_handle(&o, fmt, ap);
}

void kprintf(const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
}

void kpanic(const char *fmt, ...)
{
    kputs("\n*** [混元 OS] 内核致命错误: ");
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    kvprintf(fmt, ap);
    __builtin_va_end(ap);
    kputs("\n系统停机。\n");
    arch_halt_forever();
}

void kpanic_status(const char *tag, hyn_status_t st)
{
    kpanic("%s 失败（状态码 %u）", tag, st);
}