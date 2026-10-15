#include "vsmx.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STACK_MAX    1024
#define FRAMES_MAX   128
#define STEP_LIMIT   5000000          /* instructions per vs_call: a stuck script is stopped */
#define GC_EVERY     4096             /* allocations between collections */
#define HOLD_MAX     256

enum { OBJ_PLAIN, OBJ_ARRAY, OBJ_FUNC, OBJ_NATIVE, OBJ_HOST };

/* Every collected allocation starts with this. */
typedef struct gc_head {
    struct gc_head *next;
    uint8_t marked, is_string;
} gc_head;

struct vs_str {
    gc_head gc;
    int len;
    char text[];
};

typedef struct prop {
    const char *key;          /* interned */
    vs_value value;
} prop;

struct vs_obj {
    gc_head gc;
    int kind;
    prop *props;
    int prop_count, prop_cap;
    vs_value *items;          /* arrays */
    int length, item_cap;
    uint32_t start;           /* functions: first instruction */
    int params, locals;
    vs_native native;         /* native functions */
    void *user;
    const vs_class *cls;      /* host objects */
    void *host;
    vs_obj *proto;            /* objects made by "new F": F.prototype */
};

/* A stack entry: a value, or a reference that an instruction reads or writes. */
enum { SLOT_VALUE, SLOT_GLOBAL, SLOT_LOCAL, SLOT_PROP, SLOT_ITEM };
typedef struct slot {
    int kind;
    vs_value v;               /* the value, or the base object of a property or item */
    vs_value index;           /* SLOT_ITEM */
    const char *key;          /* SLOT_GLOBAL, SLOT_PROP */
    int local;                /* SLOT_LOCAL */
} slot;

typedef struct frame {
    uint32_t ret_pc;
    int stack_base;           /* the caller's stack depth, without the call's operands */
    vs_value *locals;
    int local_count;
    vs_value self;
    int construct;            /* "new": return this unless the function returns an object */
} frame;

typedef struct symbol {
    struct symbol *next;
    char text[];
} symbol;

struct vsmx {
    uint32_t *code;           /* opcode, operand pairs */
    uint32_t code_len;
    const char **names, **props;
    vs_str **texts;
    uint32_t name_count, prop_count, text_count;
    vs_obj *globals;
    slot stack[STACK_MAX];
    int sp;
    frame frames[FRAMES_MAX];
    int fp;
    gc_head *heap;
    int allocs;
    vs_value held[HOLD_MAX];
    uint8_t held_used[HOLD_MAX];
    symbol *symbols[256];
    vs_log_fn log;
    void *log_user;
    int error;
    /* interned keys used by the interpreter */
    const char *k_length, *k_call, *k_slice, *k_push, *k_prototype;
};

static void log_msg(vsmx *vm, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void log_msg(vsmx *vm, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (!vm->log)
        return;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    vm->log(vm->log_user, buf);
}

/* ---- values ---- */

vs_value vs_undef(void) { vs_value v; v.type = VS_UNDEF; v.u.n = 0; return v; }
vs_value vs_null(void) { vs_value v; v.type = VS_NULL; v.u.n = 0; return v; }
vs_value vs_bool(int b) { vs_value v; v.type = VS_BOOL; v.u.b = b != 0; return v; }
vs_value vs_num(double n) { vs_value v; v.type = VS_NUM; v.u.n = n; return v; }
static vs_value obj_value(vs_obj *o) { vs_value v; v.type = VS_OBJ; v.u.o = o; return v; }
static vs_value str_value(vs_str *s) { vs_value v; v.type = VS_STR; v.u.s = s; return v; }

const char *vs_intern(vsmx *vm, const char *s)
{
    unsigned h = 5381;
    symbol *sym;
    size_t n = strlen(s);

    for (const char *p = s; *p; p++)
        h = h * 33 + (unsigned char)*p;
    for (sym = vm->symbols[h & 255]; sym; sym = sym->next)
        if (strcmp(sym->text, s) == 0)
            return sym->text;
    if ((sym = malloc(sizeof(symbol) + n + 1)) == NULL)
        return "";
    memcpy(sym->text, s, n + 1);
    sym->next = vm->symbols[h & 255];
    vm->symbols[h & 255] = sym;
    return sym->text;
}

static void *gc_alloc(vsmx *vm, size_t size, int is_string)
{
    gc_head *h = calloc(1, size);

    if (!h) {
        vm->error = 1;
        return NULL;
    }
    h->is_string = (uint8_t)is_string;
    h->next = vm->heap;
    vm->heap = h;
    vm->allocs++;
    return h;
}

static vs_str *new_str(vsmx *vm, const char *s, int len)
{
    vs_str *str = gc_alloc(vm, sizeof(vs_str) + (size_t)len + 1, 1);

    if (!str)
        return NULL;
    str->len = len;
    memcpy(str->text, s, (size_t)len);
    str->text[len] = 0;
    return str;
}

vs_value vs_string(vsmx *vm, const char *s)
{
    vs_str *str = new_str(vm, s, (int)strlen(s));
    return str ? str_value(str) : vs_undef();
}

static vs_obj *new_obj(vsmx *vm, int kind)
{
    vs_obj *o = gc_alloc(vm, sizeof(vs_obj), 0);
    if (o)
        o->kind = kind;
    return o;
}

vs_value vs_new_object(vsmx *vm)
{
    vs_obj *o = new_obj(vm, OBJ_PLAIN);
    return o ? obj_value(o) : vs_undef();
}

static int set_length(vs_obj *a, int length)
{
    if (length < 0 || length > 1000000)
        return -1;
    if (length > a->item_cap) {
        int cap = a->item_cap ? a->item_cap : 8;
        vs_value *n;
        while (cap < length)
            cap *= 2;
        if ((n = realloc(a->items, (size_t)cap * sizeof(vs_value))) == NULL)
            return -1;
        a->items = n;
        a->item_cap = cap;
    }
    for (int i = a->length; i < length; i++)
        a->items[i] = vs_undef();
    a->length = length;
    return 0;
}

vs_value vs_new_array(vsmx *vm, int length)
{
    vs_obj *o = new_obj(vm, OBJ_ARRAY);
    if (!o || set_length(o, length) < 0)
        return vs_undef();
    return obj_value(o);
}

vs_value vs_new_native(vsmx *vm, vs_native fn, void *user)
{
    vs_obj *o = new_obj(vm, OBJ_NATIVE);
    if (!o)
        return vs_undef();
    o->native = fn;
    o->user = user;
    return obj_value(o);
}

vs_value vs_new_host(vsmx *vm, const vs_class *cls, void *host)
{
    vs_obj *o = new_obj(vm, OBJ_HOST);
    if (!o)
        return vs_undef();
    o->cls = cls;
    o->host = host;
    return obj_value(o);
}

int vs_truthy(vs_value v)
{
    switch (v.type) {
    case VS_BOOL: return v.u.b;
    case VS_NUM: return v.u.n != 0 && !isnan(v.u.n);
    case VS_STR: return v.u.s->len > 0;
    case VS_OBJ: return 1;
    default: return 0;
    }
}

double vs_number(vs_value v)
{
    switch (v.type) {
    case VS_BOOL: return v.u.b;
    case VS_NUM: return v.u.n;
    case VS_NULL: return 0;
    case VS_STR: {
        char *end;
        double d = strtod(v.u.s->text, &end);
        return end == v.u.s->text && v.u.s->len ? NAN : d;
    }
    default: return NAN;
    }
}

const char *vs_text(vs_value v, char *buf, int size)
{
    switch (v.type) {
    case VS_UNDEF: return "undefined";
    case VS_NULL: return "null";
    case VS_BOOL: return v.u.b ? "true" : "false";
    case VS_STR: return v.u.s->text;
    case VS_NUM:
        if (isnan(v.u.n))
            return "NaN";
        if (v.u.n == floor(v.u.n) && fabs(v.u.n) < 1e15)
            snprintf(buf, (size_t)size, "%.0f", v.u.n);
        else
            snprintf(buf, (size_t)size, "%.15g", v.u.n);
        return buf;
    default:
        return v.u.o->kind == OBJ_ARRAY ? "" : "[object Object]";
    }
}

int vs_is_function(vs_value v)
{
    return v.type == VS_OBJ && (v.u.o->kind == OBJ_FUNC || v.u.o->kind == OBJ_NATIVE ||
                                (v.u.o->kind == OBJ_HOST && v.u.o->cls->call));
}

void *vs_host(vs_value v, const vs_class *cls)
{
    return v.type == VS_OBJ && v.u.o->kind == OBJ_HOST && v.u.o->cls == cls ? v.u.o->host : NULL;
}

int vs_length(vs_value a)
{
    return a.type == VS_OBJ && a.u.o->kind == OBJ_ARRAY ? a.u.o->length : 0;
}

/* ---- properties ---- */

static prop *find_prop(vs_obj *o, const char *key)
{
    for (int i = 0; i < o->prop_count; i++)
        if (o->props[i].key == key)
            return &o->props[i];
    return NULL;
}

static void put_prop(vsmx *vm, vs_obj *o, const char *key, vs_value value)
{
    prop *p = find_prop(o, key);

    if (p) {
        p->value = value;
        return;
    }
    if (o->prop_count == o->prop_cap) {
        int cap = o->prop_cap ? o->prop_cap * 2 : 8;
        prop *n = realloc(o->props, (size_t)cap * sizeof(prop));
        if (!n) {
            vm->error = 1;
            return;
        }
        o->props = n;
        o->prop_cap = cap;
    }
    o->props[o->prop_count].key = key;
    o->props[o->prop_count].value = value;
    o->prop_count++;
}

static void native_call(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret);
static void native_slice(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret);
static void native_push(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret);

vs_value vs_get(vsmx *vm, vs_value obj, const char *key)
{
    vs_value out = vs_undef();
    prop *p;
    int depth = 0;

    if (obj.type == VS_STR) {
        if (key == vm->k_length)
            return vs_num(obj.u.s->len);
        return vs_undef();
    }
    if (obj.type != VS_OBJ)
        return vs_undef();
    if ((p = find_prop(obj.u.o, key)) != NULL)
        return p->value;
    if (obj.u.o->kind == OBJ_HOST && obj.u.o->cls->get && obj.u.o->cls->get(vm, obj.u.o->host, key, &out))
        return out;
    if (obj.u.o->kind == OBJ_ARRAY) {
        if (key == vm->k_length)
            return vs_num(obj.u.o->length);
        if (key == vm->k_slice)
            return vs_new_native(vm, native_slice, NULL);
        if (key == vm->k_push)
            return vs_new_native(vm, native_push, NULL);
        /* "3" as a key */
        if (key[0] >= '0' && key[0] <= '9')
            return vs_get_index(vm, obj, atoi(key));
    }
    if ((obj.u.o->kind == OBJ_FUNC || obj.u.o->kind == OBJ_NATIVE) && key == vm->k_call)
        return vs_new_native(vm, native_call, NULL);
    if (obj.u.o->kind == OBJ_FUNC && key == vm->k_prototype) {
        out = vs_new_object(vm);            /* made on first use */
        put_prop(vm, obj.u.o, key, out);
        return out;
    }
    /* Up the prototype chain: methods set with F.prototype.init = function ... */
    for (vs_obj *o = obj.u.o->proto; o && depth < 16; o = o->proto, depth++)
        if ((p = find_prop(o, key)) != NULL)
            return p->value;
    return vs_undef();
}

void vs_set(vsmx *vm, vs_value obj, const char *key, vs_value value)
{
    if (obj.type != VS_OBJ)
        return;
    if (obj.u.o->kind == OBJ_HOST && obj.u.o->cls->set && obj.u.o->cls->set(vm, obj.u.o->host, key, value))
        return;
    if (obj.u.o->kind == OBJ_ARRAY && key == vm->k_length) {
        set_length(obj.u.o, (int)vs_number(value));
        return;
    }
    put_prop(vm, obj.u.o, key, value);
}

vs_value vs_get_index(vsmx *vm, vs_value a, int index)
{
    char buf[16];

    if (a.type == VS_OBJ && a.u.o->kind == OBJ_ARRAY)
        return index >= 0 && index < a.u.o->length ? a.u.o->items[index] : vs_undef();
    snprintf(buf, sizeof(buf), "%d", index);
    return vs_get(vm, a, vs_intern(vm, buf));
}

void vs_set_index(vsmx *vm, vs_value a, int index, vs_value value)
{
    char buf[16];

    if (a.type == VS_OBJ && a.u.o->kind == OBJ_ARRAY) {
        if (index >= a.u.o->length && set_length(a.u.o, index + 1) < 0)
            return;
        if (index >= 0)
            a.u.o->items[index] = value;
        return;
    }
    snprintf(buf, sizeof(buf), "%d", index);
    vs_set(vm, a, vs_intern(vm, buf), value);
}

/* obj[index] where index may be a number or a string. */
static vs_value get_item(vsmx *vm, vs_value obj, vs_value index)
{
    char buf[32];

    if (index.type == VS_NUM && index.u.n == floor(index.u.n))
        return vs_get_index(vm, obj, (int)index.u.n);
    return vs_get(vm, obj, vs_intern(vm, vs_text(index, buf, sizeof(buf))));
}

static void set_item(vsmx *vm, vs_value obj, vs_value index, vs_value value)
{
    char buf[32];

    if (index.type == VS_NUM && index.u.n == floor(index.u.n)) {
        vs_set_index(vm, obj, (int)index.u.n, value);
        return;
    }
    vs_set(vm, obj, vs_intern(vm, vs_text(index, buf, sizeof(buf))), value);
}

vs_value vs_global(vsmx *vm, const char *name)
{
    prop *p = find_prop(vm->globals, vs_intern(vm, name));
    return p ? p->value : vs_undef();
}

void vs_set_global(vsmx *vm, const char *name, vs_value value)
{
    put_prop(vm, vm->globals, vs_intern(vm, name), value);
}

/* ---- built-ins ---- */

static void native_call(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    (void)user;
    /* f.call(thisArg, ...): self is f. */
    vs_call(vm, self, argc > 0 ? args[0] : vs_undef(), args + (argc > 0), argc > 0 ? argc - 1 : 0, ret);
}

static void native_slice(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    int n = vs_length(self), from = argc > 0 ? (int)vs_number(args[0]) : 0, to = argc > 1 ? (int)vs_number(args[1]) : n;

    (void)user;
    if (from < 0)
        from += n;
    if (to < 0)
        to += n;
    from = from < 0 ? 0 : from > n ? n : from;
    to = to < from ? from : to > n ? n : to;
    *ret = vs_new_array(vm, to - from);
    for (int i = from; i < to; i++)
        vs_set_index(vm, *ret, i - from, vs_get_index(vm, self, i));
}

static void native_push(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    (void)user;
    for (int i = 0; i < argc; i++)
        vs_set_index(vm, self, vs_length(self), args[i]);
    *ret = vs_num(vs_length(self));
}

static void native_array(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    (void)user;
    (void)self;
    if (argc == 1 && args[0].type == VS_NUM) {
        *ret = vs_new_array(vm, (int)args[0].u.n);
        return;
    }
    *ret = vs_new_array(vm, argc);
    for (int i = 0; i < argc; i++)
        vs_set_index(vm, *ret, i, args[i]);
}

static void native_object(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    (void)user;
    (void)self;
    (void)args;
    (void)argc;
    *ret = vs_new_object(vm);
}

static void native_math(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    double a = argc > 0 ? vs_number(args[0]) : NAN, b = argc > 1 ? vs_number(args[1]) : NAN;
    const char *fn = (const char *)user;

    (void)vm;
    (void)self;
    if (strcmp(fn, "abs") == 0) *ret = vs_num(fabs(a));
    else if (strcmp(fn, "ceil") == 0) *ret = vs_num(ceil(a));
    else if (strcmp(fn, "floor") == 0) *ret = vs_num(floor(a));
    else if (strcmp(fn, "round") == 0) *ret = vs_num(floor(a + 0.5));
    else if (strcmp(fn, "min") == 0) *ret = vs_num(a < b ? a : b);
    else if (strcmp(fn, "max") == 0) *ret = vs_num(a > b ? a : b);
    else if (strcmp(fn, "sin") == 0) *ret = vs_num(sin(a));
    else if (strcmp(fn, "cos") == 0) *ret = vs_num(cos(a));
    else if (strcmp(fn, "sqrt") == 0) *ret = vs_num(sqrt(a));
    else if (strcmp(fn, "pow") == 0) *ret = vs_num(pow(a, b));
    else if (strcmp(fn, "atan2") == 0) *ret = vs_num(atan2(a, b));
    else if (strcmp(fn, "random") == 0) *ret = vs_num((double)rand() / ((double)RAND_MAX + 1));
}

/* Int, Float, Number, String, parseInt, parseFloat, isNaN: user is the name. */
static void native_convert(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    const char *fn = (const char *)user;
    vs_value a = argc > 0 ? args[0] : vs_undef();
    char buf[64];

    (void)self;
    if (strcmp(fn, "Int") == 0) {
        double d = vs_number(a);
        *ret = vs_num(isnan(d) ? 0 : d < 0 ? ceil(d) : floor(d));
    } else if (strcmp(fn, "Float") == 0 || strcmp(fn, "Number") == 0 || strcmp(fn, "parseFloat") == 0) {
        *ret = vs_num(vs_number(a));
    } else if (strcmp(fn, "parseInt") == 0) {
        int radix = argc > 1 ? (int)vs_number(args[1]) : 10;
        const char *t = vs_text(a, buf, sizeof(buf));
        char *end;
        long v = strtol(t, &end, radix >= 2 && radix <= 36 ? radix : 10);
        *ret = end == t ? vs_num(NAN) : vs_num((double)v);
    } else if (strcmp(fn, "isNaN") == 0) {
        *ret = vs_bool(isnan(vs_number(a)));
    } else if (strcmp(fn, "String") == 0) {
        *ret = vs_string(vm, vs_text(a, buf, sizeof(buf)));
    }
}

static void add_builtins(vsmx *vm)
{
    static const char *const convert[] = {"Int", "Float", "Number", "String", "parseInt", "parseFloat", "isNaN"};
    static const char *const math[] = {"abs", "ceil", "floor", "round", "min", "max", "sin", "cos", "sqrt", "pow",
                                       "atan2", "random"};
    vs_value m = vs_new_object(vm);

    vs_set_global(vm, "Array", vs_new_native(vm, native_array, NULL));
    vs_set_global(vm, "Object", vs_new_native(vm, native_object, NULL));
    for (unsigned i = 0; i < sizeof(math) / sizeof(math[0]); i++)
        vs_set(vm, m, vs_intern(vm, math[i]), vs_new_native(vm, native_math, (void *)math[i]));
    vs_set(vm, m, vs_intern(vm, "PI"), vs_num(3.14159265358979323846));
    vs_set_global(vm, "Math", m);
    for (unsigned i = 0; i < sizeof(convert) / sizeof(convert[0]); i++)
        vs_set_global(vm, convert[i], vs_new_native(vm, native_convert, (void *)convert[i]));
    vs_set_global(vm, "undefined", vs_undef());
    vs_set_global(vm, "NULL", vs_null());
    vs_set_global(vm, "NaN", vs_num(NAN));
}

/* ---- collector ---- */

static void mark_value(vs_value v);

static void mark_obj(vs_obj *o)
{
    if (o->gc.marked)
        return;
    o->gc.marked = 1;
    if (o->proto)
        mark_obj(o->proto);
    for (int i = 0; i < o->prop_count; i++)
        mark_value(o->props[i].value);
    for (int i = 0; i < o->length; i++)
        mark_value(o->items[i]);
}

static void mark_value(vs_value v)
{
    if (v.type == VS_STR)
        v.u.s->gc.marked = 1;
    else if (v.type == VS_OBJ)
        mark_obj(v.u.o);
}

static void collect(vsmx *vm)
{
    gc_head **link = &vm->heap;

    mark_obj(vm->globals);
    for (uint32_t i = 0; i < vm->text_count; i++)
        if (vm->texts[i])
            vm->texts[i]->gc.marked = 1;
    for (int i = 0; i < vm->sp; i++) {
        mark_value(vm->stack[i].v);
        mark_value(vm->stack[i].index);
    }
    for (int f = 0; f < vm->fp; f++) {
        for (int i = 0; i < vm->frames[f].local_count; i++)
            mark_value(vm->frames[f].locals[i]);
        mark_value(vm->frames[f].self);
    }
    for (int i = 0; i < HOLD_MAX; i++)
        if (vm->held_used[i])
            mark_value(vm->held[i]);
    while (*link) {
        gc_head *h = *link;
        if (h->marked) {
            h->marked = 0;
            link = &h->next;
            continue;
        }
        *link = h->next;
        if (!h->is_string) {
            vs_obj *o = (vs_obj *)h;
            free(o->props);
            free(o->items);
        }
        free(h);
    }
    vm->allocs = 0;
}

int vs_hold(vsmx *vm, vs_value v)
{
    for (int i = 0; i < HOLD_MAX; i++)
        if (!vm->held_used[i]) {
            vm->held_used[i] = 1;
            vm->held[i] = v;
            return i;
        }
    log_msg(vm, "vsmx: too many held values");
    return -1;
}

void vs_release(vsmx *vm, int h)
{
    if (h >= 0 && h < HOLD_MAX)
        vm->held_used[h] = 0;
}

vs_value vs_held(vsmx *vm, int h)
{
    return h >= 0 && h < HOLD_MAX && vm->held_used[h] ? vm->held[h] : vs_undef();
}

/* ---- loading ---- */

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* Splits a table of NUL-separated strings. wide: UTF-16LE (converted to UTF-8). */
static char **split(const uint8_t *data, uint32_t len, uint32_t count, int wide)
{
    char **out = calloc(count ? count : 1, sizeof(char *));
    uint32_t pos = 0;

    if (!out)
        return NULL;
    for (uint32_t i = 0; i < count; i++) {
        char buf[1024];
        int n = 0;
        while (pos + (wide ? 1u : 0u) < len) {
            unsigned c = wide ? (unsigned)(data[pos] | data[pos + 1] << 8) : data[pos];
            pos += wide ? 2 : 1;
            if (c == 0)
                break;
            if (n < (int)sizeof(buf) - 4) {
                if (c < 0x80) {
                    buf[n++] = (char)c;
                } else if (c < 0x800) {
                    buf[n++] = (char)(0xC0 | c >> 6);
                    buf[n++] = (char)(0x80 | (c & 0x3F));
                } else {
                    buf[n++] = (char)(0xE0 | c >> 12);
                    buf[n++] = (char)(0x80 | ((c >> 6) & 0x3F));
                    buf[n++] = (char)(0x80 | (c & 0x3F));
                }
            }
        }
        buf[n] = 0;
        out[i] = malloc((size_t)n + 1);
        if (!out[i])
            return out;
        memcpy(out[i], buf, (size_t)n + 1);
    }
    return out;
}

vsmx *vs_create(const uint8_t *s, uint32_t size, vs_log_fn log, void *log_user)
{
    vsmx *vm;
    uint32_t co, cl, to, tl, tn, po, pl, pn, no, nl, nn;
    char **texts, **props, **names;

    if (size < 0x34 || memcmp(s, "VSMX", 4) != 0)
        return NULL;
    co = le32(s + 8); cl = le32(s + 12);
    to = le32(s + 16); tl = le32(s + 20); tn = le32(s + 24);
    po = le32(s + 28); pl = le32(s + 32); pn = le32(s + 36);
    no = le32(s + 40); nl = le32(s + 44); nn = le32(s + 48);
    if (co + cl > size || to + tl > size || po + pl > size || no + nl > size || cl % 8)
        return NULL;
    if ((vm = calloc(1, sizeof(vsmx))) == NULL)
        return NULL;
    vm->log = log;
    vm->log_user = log_user;
    vm->code_len = cl / 8;
    vm->code = malloc(cl ? cl : 8);
    texts = split(s + to, tl, tn, 1);
    props = split(s + po, pl, pn, 1);
    names = split(s + no, nl, nn, 0);
    vm->texts = calloc(tn ? tn : 1, sizeof(vs_str *));
    vm->props = calloc(pn ? pn : 1, sizeof(char *));
    vm->names = calloc(nn ? nn : 1, sizeof(char *));
    vm->globals = new_obj(vm, OBJ_PLAIN);
    if (!vm->code || !texts || !props || !names || !vm->texts || !vm->props || !vm->names || !vm->globals) {
        vs_destroy(vm);
        vm = NULL;
        goto out;
    }
    for (uint32_t i = 0; i < cl / 4; i++)
        vm->code[i] = le32(s + co + i * 4);
    vm->text_count = tn;
    vm->prop_count = pn;
    vm->name_count = nn;
    for (uint32_t i = 0; i < tn; i++)
        vm->texts[i] = texts[i] ? new_str(vm, texts[i], (int)strlen(texts[i])) : NULL;
    for (uint32_t i = 0; i < pn; i++)
        vm->props[i] = vs_intern(vm, props[i] ? props[i] : "");
    for (uint32_t i = 0; i < nn; i++)
        vm->names[i] = vs_intern(vm, names[i] ? names[i] : "");
    vm->k_length = vs_intern(vm, "length");
    vm->k_call = vs_intern(vm, "call");
    vm->k_slice = vs_intern(vm, "slice");
    vm->k_push = vs_intern(vm, "push");
    vm->k_prototype = vs_intern(vm, "prototype");
    add_builtins(vm);
out:
    for (uint32_t i = 0; texts && i < tn; i++)
        free(texts[i]);
    for (uint32_t i = 0; props && i < pn; i++)
        free(props[i]);
    for (uint32_t i = 0; names && i < nn; i++)
        free(names[i]);
    free(texts);
    free(props);
    free(names);
    return vm;
}

void vs_destroy(vsmx *vm)
{
    gc_head *h;

    if (!vm)
        return;
    for (int f = 0; f < vm->fp; f++)
        free(vm->frames[f].locals);
    while ((h = vm->heap) != NULL) {
        vm->heap = h->next;
        if (!h->is_string) {
            free(((vs_obj *)h)->props);
            free(((vs_obj *)h)->items);
        }
        free(h);
    }
    for (int i = 0; i < 256; i++)
        while (vm->symbols[i]) {
            symbol *s = vm->symbols[i];
            vm->symbols[i] = s->next;
            free(s);
        }
    free(vm->code);
    free(vm->texts);
    free(vm->props);
    free(vm->names);
    free(vm);
}

/* ---- the interpreter ---- */

static vs_value read_slot(vsmx *vm, const slot *s)
{
    switch (s->kind) {
    case SLOT_GLOBAL: {
        prop *p = find_prop(vm->globals, s->key);
        return p ? p->value : vs_undef();
    }
    case SLOT_LOCAL: {
        frame *f = &vm->frames[vm->fp - 1];
        return vm->fp > 0 && s->local < f->local_count ? f->locals[s->local] : vs_undef();
    }
    case SLOT_PROP:
        return vs_get(vm, s->v, s->key);
    case SLOT_ITEM:
        return get_item(vm, s->v, s->index);
    default:
        return s->v;
    }
}

static void write_slot(vsmx *vm, const slot *s, vs_value v)
{
    switch (s->kind) {
    case SLOT_GLOBAL:
        put_prop(vm, vm->globals, s->key, v);
        break;
    case SLOT_LOCAL: {
        frame *f = &vm->frames[vm->fp - 1];
        if (vm->fp > 0 && s->local < f->local_count)
            f->locals[s->local] = v;
        break;
    }
    case SLOT_PROP:
        vs_set(vm, s->v, s->key, v);
        break;
    case SLOT_ITEM:
        set_item(vm, s->v, s->index, v);
        break;
    default:
        log_msg(vm, "vsmx: assignment to a value");
        break;
    }
}

static int push(vsmx *vm, slot s)
{
    if (vm->sp >= STACK_MAX) {
        log_msg(vm, "vsmx: stack overflow");
        vm->error = 1;
        return -1;
    }
    vm->stack[vm->sp++] = s;
    return 0;
}

static void push_value(vsmx *vm, vs_value v)
{
    slot s;
    memset(&s, 0, sizeof(s));
    s.kind = SLOT_VALUE;
    s.v = v;
    push(vm, s);
}

static slot pop(vsmx *vm)
{
    slot s;
    if (vm->sp > 0)
        return vm->stack[--vm->sp];
    memset(&s, 0, sizeof(s));
    s.kind = SLOT_VALUE;
    return s;
}

static vs_value pop_value(vsmx *vm)
{
    slot s = pop(vm);
    return read_slot(vm, &s);
}

static int loose_equal(vs_value a, vs_value b)
{
    if ((a.type == VS_UNDEF || a.type == VS_NULL) && (b.type == VS_UNDEF || b.type == VS_NULL))
        return 1;
    if (a.type == VS_UNDEF || a.type == VS_NULL || b.type == VS_UNDEF || b.type == VS_NULL)
        return 0;
    if (a.type == VS_STR && b.type == VS_STR)
        return strcmp(a.u.s->text, b.u.s->text) == 0;
    if (a.type == VS_OBJ || b.type == VS_OBJ)
        return a.type == b.type && a.u.o == b.u.o;
    return vs_number(a) == vs_number(b);
}

static int strict_equal(vs_value a, vs_value b)
{
    if (a.type != b.type)
        return 0;
    switch (a.type) {
    case VS_UNDEF:
    case VS_NULL: return 1;
    case VS_BOOL: return a.u.b == b.u.b;
    case VS_NUM: return a.u.n == b.u.n;
    case VS_STR: return strcmp(a.u.s->text, b.u.s->text) == 0;
    default: return a.u.o == b.u.o;
    }
}

static const char *type_name(vs_value v)
{
    switch (v.type) {
    case VS_UNDEF: return "undefined";
    case VS_BOOL: return "boolean";
    case VS_NUM: return "number";
    case VS_STR: return "string";
    case VS_OBJ: return vs_is_function(v) ? "function" : "object";
    default: return "object";
    }
}

static vs_value add(vsmx *vm, vs_value a, vs_value b)
{
    if (a.type == VS_STR || b.type == VS_STR) {
        char ba[64], bb[64], *buf;
        const char *ta = vs_text(a, ba, sizeof(ba)), *tb = vs_text(b, bb, sizeof(bb));
        size_t la = strlen(ta), lb = strlen(tb);
        vs_str *s;
        if ((buf = malloc(la + lb + 1)) == NULL)
            return vs_undef();
        memcpy(buf, ta, la);
        memcpy(buf + la, tb, lb + 1);
        s = new_str(vm, buf, (int)(la + lb));
        free(buf);
        return s ? str_value(s) : vs_undef();
    }
    return vs_num(vs_number(a) + vs_number(b));
}

static int compare(vs_value a, vs_value b, int op)
{
    double x, y;

    if (a.type == VS_STR && b.type == VS_STR) {
        int c = strcmp(a.u.s->text, b.u.s->text);
        x = c;
        y = 0;
    } else {
        x = vs_number(a);
        y = vs_number(b);
    }
    switch (op) {
    case 0x12: return x < y;
    case 0x13: return x <= y;
    case 0x14: return x >= y;
    default: return x > y;
    }
}

static int32_t to_int32(vs_value v)
{
    double d = vs_number(v);
    if (isnan(d) || isinf(d))
        return 0;
    return (int32_t)(uint32_t)(int64_t)d;
}

static int enter(vsmx *vm, vs_obj *fn, vs_value self, const vs_value *args, int argc, uint32_t ret_pc)
{
    frame *f;
    int count = fn->locals > fn->params + 1 ? fn->locals : fn->params + 1;

    if (vm->fp >= FRAMES_MAX) {
        log_msg(vm, "vsmx: calls nested too deeply");
        return -1;
    }
    f = &vm->frames[vm->fp];
    f->locals = calloc((size_t)count, sizeof(vs_value));
    if (!f->locals)
        return -1;
    f->local_count = count;
    for (int i = 0; i < argc && i < fn->params; i++)
        f->locals[i + 1] = args[i];        /* slot 0 is not a parameter */
    f->ret_pc = ret_pc;
    f->stack_base = vm->sp;
    f->self = self;
    f->construct = 0;
    vm->fp++;
    return 0;
}

/* A call instruction. The stack holds [this (method calls only)] function args...; all of it
   is popped. A native's result is pushed at once; a script function gets a frame, *pc moves
   to its first instruction, and its return pushes the result. */
static void invoke(vsmx *vm, int argc, int method, int construct, uint32_t *pc)
{
    vs_value args[32], ret = vs_undef(), fn, self = vs_undef();
    slot fs;
    int n = argc < 32 ? argc : 32;

    for (int i = argc - 1; i >= 0; i--) {
        vs_value v = pop_value(vm);
        if (i < 32)
            args[i] = v;
    }
    fs = pop(vm);
    fn = read_slot(vm, &fs);
    if (method)
        self = pop_value(vm);
    if (!vs_is_function(fn)) {
        log_msg(vm, "vsmx: %s is not a function (at %u)", fs.key ? fs.key : "value", *pc - 1);
        push_value(vm, vs_undef());
        return;
    }
    if (construct && fn.u.o->kind == OBJ_FUNC) {
        vs_value proto = vs_get(vm, fn, vm->k_prototype);
        self = vs_new_object(vm);
        if (self.type == VS_OBJ && proto.type == VS_OBJ)
            self.u.o->proto = proto.u.o;
    }
    if (fn.u.o->kind == OBJ_NATIVE) {
        fn.u.o->native(vm, fn.u.o->user, self, args, n, &ret);
        push_value(vm, ret);
        return;
    }
    if (fn.u.o->kind == OBJ_HOST) {
        fn.u.o->cls->call(vm, fn.u.o->host, self, args, n, &ret);
        push_value(vm, ret);
        return;
    }
    if (enter(vm, fn.u.o, self, args, n, *pc) < 0) {
        push_value(vm, vs_undef());
        return;
    }
    vm->frames[vm->fp - 1].construct = construct;
    *pc = fn.u.o->start;
}

/* Runs until the frame count drops back to stop_fp (a return from the entry frame) or the
   script ends. Returns 0, or -1 on an error. */
static int run(vsmx *vm, uint32_t pc, int stop_fp, vs_value *result)
{
    long steps = 0;

    *result = vs_undef();
    while (!vm->error) {
        uint32_t op, val;
        slot s;
        vs_value a, b;

        if (pc >= vm->code_len) {
            log_msg(vm, "vsmx: ran off the end of the code");
            return -1;
        }
        if (++steps > STEP_LIMIT) {
            log_msg(vm, "vsmx: stopped a script that ran too long");
            return -1;
        }
        if (vm->allocs > GC_EVERY)
            collect(vm);
        op = vm->code[pc * 2];
        val = vm->code[pc * 2 + 1];
        pc++;
        memset(&s, 0, sizeof(s));
        if ((op & 0xFF) == 0x2A) {
            vs_obj *fn = new_obj(vm, OBJ_FUNC);
            if (!fn)
                return -1;
            fn->start = val;
            fn->params = (int)((op >> 8) & 0xFF);
            fn->locals = (int)(op >> 24);
            push_value(vm, obj_value(fn));
            continue;
        }
        switch (op) {
        case 0x00: break;
        case 0x01:            /* assign */
            a = pop_value(vm);
            s = pop(vm);
            write_slot(vm, &s, a);
            push_value(vm, a);
            break;
        case 0x02: b = pop_value(vm); a = pop_value(vm); push_value(vm, add(vm, a, b)); break;
        case 0x03: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(vs_number(a) - vs_number(b))); break;
        case 0x04: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(vs_number(a) * vs_number(b))); break;
        case 0x05: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(vs_number(a) / vs_number(b))); break;
        case 0x06: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(fmod(vs_number(a), vs_number(b)))); break;
        case 0x07: a = pop_value(vm); push_value(vm, vs_num(vs_number(a))); break;
        case 0x08: a = pop_value(vm); push_value(vm, vs_num(-vs_number(a))); break;
        case 0x09: a = pop_value(vm); push_value(vm, vs_bool(!vs_truthy(a))); break;
        case 0x0A: case 0x0B: case 0x0C: case 0x0D: {   /* ++x --x x++ x-- */
            double old, now;
            s = pop(vm);
            old = vs_number(read_slot(vm, &s));
            now = old + ((op == 0x0A || op == 0x0C) ? 1 : -1);
            write_slot(vm, &s, vs_num(now));
            push_value(vm, vs_num(op <= 0x0B ? now : old));
            break;
        }
        case 0x0E: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_bool(loose_equal(a, b))); break;
        case 0x0F: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_bool(!loose_equal(a, b))); break;
        case 0x10: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_bool(strict_equal(a, b))); break;
        case 0x11: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_bool(!strict_equal(a, b))); break;
        case 0x12: case 0x13: case 0x14: case 0x15:
            b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_bool(compare(a, b, (int)op))); break;
        case 0x18: a = pop_value(vm); push_value(vm, vs_string(vm, type_name(a))); break;
        case 0x19: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(to_int32(a) & to_int32(b))); break;
        case 0x1A: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(to_int32(a) ^ to_int32(b))); break;
        case 0x1B: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(to_int32(a) | to_int32(b))); break;
        case 0x1C: a = pop_value(vm); push_value(vm, vs_num(~to_int32(a))); break;
        case 0x1D: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num((int32_t)((uint32_t)to_int32(a) << (to_int32(b) & 31)))); break;
        case 0x1E: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num(to_int32(a) >> (to_int32(b) & 31))); break;
        case 0x1F: b = pop_value(vm); a = pop_value(vm); push_value(vm, vs_num((uint32_t)to_int32(a) >> (to_int32(b) & 31))); break;
        case 0x20:            /* duplicate */
            if (vm->sp > 0)
                push(vm, vm->stack[vm->sp - 1]);
            break;
        case 0x21:            /* swap */
            if (vm->sp > 1) {
                slot t = vm->stack[vm->sp - 1];
                vm->stack[vm->sp - 1] = vm->stack[vm->sp - 2];
                vm->stack[vm->sp - 2] = t;
            }
            break;
        case 0x22: pop(vm); break;
        case 0x23: push_value(vm, vs_undef()); break;
        case 0x24: push_value(vm, vs_null()); break;
        case 0x25: push_value(vm, vs_bool((int)val)); break;
        case 0x26: push_value(vm, vs_num((int32_t)val)); break;
        case 0x27: {
            float f;
            memcpy(&f, &val, 4);
            push_value(vm, vs_num(f));
            break;
        }
        case 0x28:
            push_value(vm, val < vm->text_count && vm->texts[val] ? str_value(vm->texts[val]) : vs_undef());
            break;
        case 0x2B: push_value(vm, vs_new_array(vm, 0)); break;
        case 0x2C: push_value(vm, vm->fp > 0 ? vm->frames[vm->fp - 1].self : vs_undef()); break;
        case 0x2D: s.kind = SLOT_LOCAL; s.local = (int)val; push(vm, s); break;
        case 0x2E:
            s.kind = SLOT_GLOBAL;
            s.key = val < vm->name_count ? vm->names[val] : "";
            push(vm, s);
            break;
        case 0x2F:            /* property reference */
            s.kind = SLOT_PROP;
            s.v = pop_value(vm);
            s.key = val < vm->prop_count ? vm->props[val] : "";
            push(vm, s);
            break;
        case 0x30:            /* method: the object stays as "this" for the call */
            a = pop_value(vm);
            push_value(vm, a);
            s.kind = SLOT_PROP;
            s.v = a;
            s.key = val < vm->prop_count ? vm->props[val] : "";
            push(vm, s);
            break;
        case 0x31:            /* set property */
            b = pop_value(vm);
            a = pop_value(vm);
            vs_set(vm, a, val < vm->prop_count ? vm->props[val] : "", b);
            push_value(vm, b);
            break;
        case 0x34:            /* item reference */
            b = pop_value(vm);
            a = pop_value(vm);
            s.kind = SLOT_ITEM;
            s.v = a;
            s.index = b;
            push(vm, s);
            break;
        case 0x35:            /* item as a method */
            b = pop_value(vm);
            a = pop_value(vm);
            push_value(vm, a);
            s.kind = SLOT_ITEM;
            s.v = a;
            s.index = b;
            push(vm, s);
            break;
        case 0x36: {          /* set item */
            vs_value v = pop_value(vm);
            b = pop_value(vm);
            a = pop_value(vm);
            set_item(vm, a, b, v);
            push_value(vm, v);
            break;
        }
        case 0x38:            /* array literal: append */
            a = pop_value(vm);
            if (vm->sp > 0) {
                vs_value arr = read_slot(vm, &vm->stack[vm->sp - 1]);
                vs_set_index(vm, arr, vs_length(arr), a);
            }
            break;
        case 0x39: pc = val; break;
        case 0x3A: if (vs_truthy(pop_value(vm))) pc = val; break;
        case 0x3B: if (!vs_truthy(pop_value(vm))) pc = val; break;
        case 0x3C: case 0x3D: case 0x3E:   /* call function, call method, new */
            if (vm->sp < (int)val + 1 + (op == 0x3D)) {
                log_msg(vm, "vsmx: stack underflow in a call at %u", pc - 1);
                return -1;
            }
            invoke(vm, (int)val, op == 0x3D, op == 0x3E, &pc);
            break;
        case 0x3F: {          /* return */
            frame *f;
            a = pop_value(vm);
            if (vm->fp <= stop_fp) {
                *result = a;
                return 0;
            }
            f = &vm->frames[--vm->fp];
            if (f->construct && a.type != VS_OBJ)
                a = f->self;
            free(f->locals);
            vm->sp = f->stack_base;
            if (vm->fp == stop_fp) {
                *result = a;
                return 0;
            }
            pc = f->ret_pc;
            push_value(vm, a);
            break;
        }
        case 0x45:            /* end of the script */
            return 0;
        default:
            log_msg(vm, "vsmx: unknown opcode %#x at %u", op, pc - 1);
            return -1;
        }
    }
    return -1;
}

int vs_run_main(vsmx *vm)
{
    vs_value r;
    int rc;

    vm->sp = 0;
    rc = run(vm, 0, 0, &r);
    vm->sp = 0;
    return rc;
}

int vs_call(vsmx *vm, vs_value fn, vs_value self, const vs_value *args, int argc, vs_value *ret)
{
    vs_value r = vs_undef();
    int base_sp = vm->sp, base_fp = vm->fp, rc = 0;

    if (!vs_is_function(fn)) {
        if (ret)
            *ret = vs_undef();
        return -1;
    }
    if (fn.u.o->kind == OBJ_NATIVE) {
        fn.u.o->native(vm, fn.u.o->user, self, args, argc, &r);
    } else if (fn.u.o->kind == OBJ_HOST) {
        fn.u.o->cls->call(vm, fn.u.o->host, self, args, argc, &r);
    } else if (enter(vm, fn.u.o, self, args, argc, 0) < 0) {
        rc = -1;
    } else {
        rc = run(vm, fn.u.o->start, base_fp, &r);
        /* Unwind whatever an error left behind. */
        while (vm->fp > base_fp)
            free(vm->frames[--vm->fp].locals);
    }
    vm->sp = base_sp;
    vm->error = 0;
    if (ret)
        *ret = r;
    return rc;
}

int vs_call_global(vsmx *vm, const char *name, const vs_value *args, int argc, vs_value *ret)
{
    vs_value fn = vs_global(vm, name);

    if (!vs_is_function(fn))
        return -1;
    return vs_call(vm, fn, vs_undef(), args, argc, ret);
}
