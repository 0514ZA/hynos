/*
 * 混元 OS —— 内核对象管理器（纯 C，架构无关，硬核底座）
 *
 * 两条不变量：
 *   1) 句柄/对象代次自增 —— 槽位复用不会让旧句柄指向新对象（结构上消除悬挂引用）；
 *   2) 销毁即清零对象体 —— 上一个使用者的数据不会被下一个使用者读到。
 *
 * 未引入动态分配器：对象体由调用方用静态池提供，等对象专用分配器接入后再切换。
 * 这一点在模块汇报里如实标注，不假装已经用了分配器。
 */
#include <hyn/object.h>
#include <hyn/kstring.h>

typedef struct obj_slot {
    u8  used;
    u8  reserved0[3];
    u32 refs;                    /* 当前打开的句柄数 */
    u32 body_size;
    u32 type;
    u32 generation;              /* 对象代次：创建从 1 起，销毁时 +1 */
    u8  domain;
    u8  reserved1[3];
    void *body;
    u64  object_id;              /* 完整 id，便于 O(1) 校验代次 */
    u64  name_hash;
    char name[HYN_OBJ_NAME_MAX];
} obj_slot_t;

typedef struct handle_slot {
    u8  used;
    u8  reserved0[3];
    u32 generation;              /* 句柄代次：每次分配/关闭都 +1 */
    u32 index;                   /* 指向对象槽位 */
} handle_slot_t;

typedef struct obj_class {
    u8  used;
    u8  reserved0[3];
    u32 type;
    const char *name;
    hyn_object_destroy_fn destroy;
    void *ctx;
} obj_class_t;

static obj_slot_t    g_objects[HYN_OBJ_MAX];
static handle_slot_t g_handles[HYN_HANDLE_MAX];
static obj_class_t   g_classes[HYN_CLASS_MAX];

static u32 g_object_count;
static u32 g_handle_count;
static u32 g_object_total;       /* 累计创建过的对象数（含已销毁） */
static int g_ready;

#define OBJ_INDEX(id)  ((u32)((id) >> 32))
#define HANDLE_SLOT(h) ((u32)((h) >> 32))

hyn_status_t hyn_obj_init(void)
{
    memset(g_objects, 0, (unsigned long)sizeof(g_objects));
    memset(g_handles, 0, (unsigned long)sizeof(g_handles));
    memset(g_classes, 0, (unsigned long)sizeof(g_classes));
    g_object_count = 0u;
    g_handle_count = 0u;
    g_object_total = 0u;
    g_ready = 1;
    return HYN_OK;
}

hyn_status_t hyn_class_register(u32 type, const char *name,
                                hyn_object_destroy_fn destroy, void *ctx)
{
    if (type == HYN_OBJ_TYPE_INVALID || type >= HYN_CLASS_MAX || name == NULL || name[0] == '\0') {
        return HYN_EINVAL;
    }
    g_classes[type].used    = 1u;
    g_classes[type].type    = type;
    g_classes[type].name    = name;
    g_classes[type].destroy = destroy;
    g_classes[type].ctx     = ctx;
    return HYN_OK;
}

const char *hyn_object_class_name(u32 type)
{
    if (type >= HYN_CLASS_MAX || !g_classes[type].used) {
        return "未注册";
    }
    return g_classes[type].name;
}

static obj_slot_t *object_by_name(const char *name)
{
    u64 h = kstr_hash(name);
    u32 i;

    for (i = 0; i < HYN_OBJ_MAX; i++) {
        if (g_objects[i].used && g_objects[i].name_hash == h && strcmp(g_objects[i].name, name) == 0) {
            return &g_objects[i];
        }
    }
    return NULL;
}

static obj_slot_t *object_by_id(hyn_object_id_t id)
{
    u32 idx = OBJ_INDEX(id);

    if (id == 0ull || idx >= HYN_OBJ_MAX) {
        return NULL;
    }
    if (!g_objects[idx].used || g_objects[idx].object_id != id) {
        return NULL;                     /* 代次不符 → 陈旧 id */
    }
    return &g_objects[idx];
}

/* 句柄 → 对象槽位；任何一步不成立都返回 NULL（陈旧/伪造句柄一律拒绝） */
static obj_slot_t *object_by_handle(hyn_handle_t h)
{
    u32 hs;
    u32 gen = (u32)h;

    hs = HANDLE_SLOT(h);

    if (h == 0ull || hs == 0u || hs > HYN_HANDLE_MAX) {
        return NULL;
    }
    hs -= 1u;
    if (!g_handles[hs].used || g_handles[hs].generation != gen) {
        return NULL;
    }
    if (g_handles[hs].index >= HYN_OBJ_MAX || !g_objects[g_handles[hs].index].used) {
        return NULL;
    }
    return &g_objects[g_handles[hs].index];
}

hyn_status_t hyn_object_create(u32 type, const char *name, hyn_domain_t domain,
                               void *body, u32 body_size,
                               hyn_object_id_t *out_id, hyn_handle_t *out_handle)
{
    obj_slot_t *o;
    handle_slot_t *hs;
    u32 i, obj_idx = HYN_OBJ_MAX, h_idx = HYN_HANDLE_MAX;

    if (!g_ready || name == NULL || name[0] == '\0' || body == NULL || body_size == 0u) {
        return HYN_EINVAL;
    }
    if (type == HYN_OBJ_TYPE_INVALID || type >= HYN_CLASS_MAX || !g_classes[type].used) {
        return HYN_EINVAL;               /* 未注册的类型不允许创建 */
    }
    if ((u32)domain >= HYN_DOMAIN_MAX || out_id == NULL || out_handle == NULL) {
        return HYN_EINVAL;
    }
    if (strlen(name) >= HYN_OBJ_NAME_MAX) {
        return HYN_EINVAL;
    }
    if (object_by_name(name) != NULL) {
        return HYN_EBUSY;                /* 同名对象已存在 */
    }
    for (i = 0; i < HYN_OBJ_MAX; i++) {
        if (!g_objects[i].used) {
            obj_idx = i;
            break;
        }
    }
    for (i = 0; i < HYN_HANDLE_MAX; i++) {
        if (!g_handles[i].used) {
            h_idx = i;
            break;
        }
    }
    if (obj_idx == HYN_OBJ_MAX || h_idx == HYN_HANDLE_MAX) {
        return HYN_ENOMEM;
    }

    o = &g_objects[obj_idx];
    o->used      = 1u;
    o->refs      = 0u;
    o->body      = body;
    o->body_size = body_size;
    o->type      = type;
    o->domain    = (u8)domain;
    o->name_hash = kstr_hash(name);
    strncpy_safe(o->name, name, HYN_OBJ_NAME_MAX);
    o->generation++;
    if (o->generation == 0u) {
        o->generation = 1u;              /* 代次从 1 起，保证 id 非 0 */
    }
    o->object_id = ((u64)obj_idx << 32) | (u64)o->generation;

    hs = &g_handles[h_idx];
    hs->used    = 1u;
    hs->index   = obj_idx;
    hs->generation++;
    if (hs->generation == 0u) {
        hs->generation = 1u;
    }
    o->refs++;
    g_object_count++;
    g_handle_count++;
    g_object_total++;

    *out_id     = o->object_id;
    *out_handle = ((u64)(h_idx + 1u) << 32) | (u64)hs->generation;
    return HYN_OK;
}

hyn_status_t hyn_object_open(const char *name, hyn_handle_t *out_handle)
{
    obj_slot_t *o;
    handle_slot_t *hs;
    u32 i, h_idx = HYN_HANDLE_MAX;

    if (!g_ready || name == NULL || out_handle == NULL) {
        return HYN_EINVAL;
    }
    o = object_by_name(name);
    if (o == NULL) {
        return HYN_ENOENT;
    }
    for (i = 0; i < HYN_HANDLE_MAX; i++) {
        if (!g_handles[i].used) {
            h_idx = i;
            break;
        }
    }
    if (h_idx == HYN_HANDLE_MAX) {
        return HYN_ENOMEM;
    }
    hs = &g_handles[h_idx];
    hs->used = 1u;
    hs->index = (u32)(o - g_objects);
    hs->generation++;
    if (hs->generation == 0u) {
        hs->generation = 1u;
    }
    o->refs++;
    g_handle_count++;
    *out_handle = ((u64)(h_idx + 1u) << 32) | (u64)hs->generation;
    return HYN_OK;
}

hyn_status_t hyn_object_lookup(hyn_handle_t h, hyn_object_id_t *out_object_id)
{
    obj_slot_t *o = object_by_handle(h);

    if (o == NULL || out_object_id == NULL) {
        return HYN_ENOENT;
    }
    *out_object_id = o->object_id;
    return HYN_OK;
}

hyn_status_t hyn_object_body(hyn_handle_t h, void **out_body, u32 *out_size)
{
    obj_slot_t *o = object_by_handle(h);

    if (o == NULL || out_body == NULL || out_size == NULL) {
        return HYN_ENOENT;
    }
    *out_body = o->body;
    *out_size = o->body_size;
    return HYN_OK;
}

hyn_status_t hyn_handle_info(hyn_handle_t h, hyn_handle_info_t *out)
{
    obj_slot_t *o = object_by_handle(h);

    if (o == NULL || out == NULL) {
        return HYN_ENOENT;
    }
    memset(out, 0, (unsigned long)sizeof(*out));
    out->object_id  = o->object_id;
    out->type       = o->type;
    out->handle_gen = (u32)h;
    out->object_gen = o->generation;
    out->refs       = o->refs;
    out->domain     = o->domain;
    out->valid      = 1u;
    return HYN_OK;
}

hyn_status_t hyn_object_close(hyn_handle_t h)
{
    obj_slot_t *o = object_by_handle(h);
    u32 hs;

    if (o == NULL) {
        return HYN_ENOENT;
    }
    hs = HANDLE_SLOT(h) - 1u;
    if (o->refs > 0u) {
        o->refs--;
    }
    g_handles[hs].used = 0u;
    g_handles[hs].generation++;          /* 关闭即换代：旧句柄值不再有效 */
    g_handle_count--;
    return HYN_OK;
}

hyn_status_t hyn_object_destroy(hyn_object_id_t object_id)
{
    obj_slot_t *o = object_by_id(object_id);

    if (o == NULL) {
        return HYN_ENOENT;
    }
    if (o->refs != 0u) {
        return HYN_EBUSY;                /* 还有打开着的句柄：销毁会造成悬挂引用 */
    }

    /* 先让类型做自己的清理（此时对象体仍然有效），再清零对象体 */
    if (o->type < HYN_CLASS_MAX && g_classes[o->type].used && g_classes[o->type].destroy != NULL) {
        g_classes[o->type].destroy(o->body, g_classes[o->type].ctx);
    }
    if (o->body != NULL && o->body_size > 0u) {
        memset(o->body, 0, (unsigned long)o->body_size);
    }

    o->used = 0u;
    o->refs = 0u;
    o->body = NULL;
    o->body_size = 0u;
    o->object_id = 0ull;
    o->generation++;                     /* 代次自增：陈旧 id 永久失效 */
    if (o->generation == 0u) {
        o->generation = 1u;
    }
    g_object_count--;
    return HYN_OK;
}

u32 hyn_object_count(void)
{
    return g_object_count;
}

u32 hyn_handle_count(void)
{
    return g_handle_count;
}

u32 hyn_object_total(void)
{
    return g_object_total;
}