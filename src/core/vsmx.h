#ifndef VSMX_H
#define VSMX_H

#include <stdint.h>

/* An interpreter for VSMX, the compiled JavaScript-like scripts in UMD Video menus (the script
   entry of an RCO file). See docs/research/umd-menus.md for the instruction set.

   Values are undefined, null, booleans, numbers (double), strings and objects. Objects are
   plain objects, arrays, script functions, native functions, and host objects whose
   properties the embedder serves (the player, widgets, animations). Memory is managed by a
   mark-and-sweep collector that runs between instructions. */

typedef struct vsmx vsmx;
typedef struct vs_obj vs_obj;
typedef struct vs_str vs_str;

enum { VS_UNDEF, VS_NULL, VS_BOOL, VS_NUM, VS_STR, VS_OBJ };

typedef struct vs_value {
    int type;
    union {
        int b;
        double n;
        vs_str *s;
        vs_obj *o;
    } u;
} vs_value;

/* A native function. args are already values; set *ret (it starts as undefined). */
typedef void (*vs_native)(vsmx *vm, void *user, vs_value self, const vs_value *args, int argc, vs_value *ret);

/* Host objects: properties the script stored on the object come first; then get returns 1
   and sets *out if it serves key. set returns 1 if it took the value; otherwise the object
   stores it. Keys are interned (compare pointers with vs_intern results). */
typedef struct vs_class {
    const char *name;
    int (*get)(vsmx *vm, void *host, const char *key, vs_value *out);
    int (*set)(vsmx *vm, void *host, const char *key, vs_value value);
    /* Non-NULL makes the object callable. */
    void (*call)(vsmx *vm, void *host, vs_value self, const vs_value *args, int argc, vs_value *ret);
    /* The object's type in its text form, "[object Plane]"; NULL for "Object". */
    const char *(*type)(void *host);
} vs_class;

typedef void (*vs_log_fn)(void *user, const char *msg);

/* Loads a script (it is copied). Returns NULL on bad data or no memory. */
vsmx *vs_create(const uint8_t *script, uint32_t size, vs_log_fn log, void *log_user);
void vs_destroy(vsmx *vm);

/* Runs the script's top level, which defines its globals and functions. Returns 0 on success. */
int vs_run_main(vsmx *vm);

/* Calls a function value. Returns 0 on success; *ret may be NULL. */
int vs_call(vsmx *vm, vs_value fn, vs_value self, const vs_value *args, int argc, vs_value *ret);
/* Calls the global function with this name, if there is one. Returns 0 if it ran. */
int vs_call_global(vsmx *vm, const char *name, const vs_value *args, int argc, vs_value *ret);

const char *vs_intern(vsmx *vm, const char *s);
vs_value vs_undef(void);
vs_value vs_null(void);
vs_value vs_bool(int b);
vs_value vs_num(double n);
vs_value vs_string(vsmx *vm, const char *s);
vs_value vs_new_object(vsmx *vm);
vs_value vs_new_array(vsmx *vm, int length);
vs_value vs_new_native(vsmx *vm, vs_native fn, void *user);
vs_value vs_new_host(vsmx *vm, const vs_class *cls, void *host);

vs_value vs_get(vsmx *vm, vs_value obj, const char *key);
void vs_set(vsmx *vm, vs_value obj, const char *key, vs_value value);
vs_value vs_get_index(vsmx *vm, vs_value array, int index);
void vs_set_index(vsmx *vm, vs_value array, int index, vs_value value);
int vs_length(vs_value array);
vs_value vs_global(vsmx *vm, const char *name);
void vs_set_global(vsmx *vm, const char *name, vs_value value);

int vs_truthy(vs_value v);
double vs_number(vs_value v);
/* The value as text in buf (or a pointer to the string's own text). */
const char *vs_text(vs_value v, char *buf, int size);
int vs_is_function(vs_value v);
void *vs_host(vs_value v, const vs_class *cls);   /* the host pointer, if v is of that class */

/* Values the embedder holds outside the script (timers, handlers): keep them alive. */
int vs_hold(vsmx *vm, vs_value v);
void vs_release(vsmx *vm, int handle);
vs_value vs_held(vsmx *vm, int handle);

#endif
