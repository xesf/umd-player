#ifndef RCO_H
#define RCO_H

#include <stdint.h>

/* Reader for the RCO files in UMD_VIDEO/RESOURCE/: a UMD Video disc's menus. One file holds
   PNG images, a tree of widgets (pages, groups, planes, buttons), animations, and one VSMX
   script. See docs/research/umd-menus.md for the layout. The reader keeps pointers into the
   file data, which must stay alive. */

enum {
    RCO_PAGE = 0x01,
    RCO_PLANE = 0x02,
    RCO_GROUP = 0x12,
    RCO_BUTTON = 0x1A
};

/* A button's focus moves and a page's events point either to a script handler or, for focus
   moves, straight to another widget. */
typedef struct rco_action {
    const char *event;        /* "script:/main/onPush_btnRootMenu", or NULL */
    int object;               /* widget index (focus moves there), or -1 */
    int anim;                 /* animation index (played on this widget), or -1 */
} rco_action;

enum { RCO_BTN_PUSH, RCO_BTN_FOCUS_IN, RCO_BTN_FOCUS_OUT, RCO_BTN_LEFT, RCO_BTN_RIGHT, RCO_BTN_UP, RCO_BTN_DOWN,
       RCO_BTN_ACTIONS };
enum { RCO_PAGE_INIT, RCO_PAGE_CANCEL, RCO_PAGE_CONTEXT, RCO_PAGE_ACTIVATE, RCO_PAGE_ACTIONS };

typedef struct rco_object {
    int kind;                 /* RCO_PAGE, RCO_PLANE, RCO_GROUP, RCO_BUTTON */
    const char *name;
    int parent, first_child, next_sibling;   /* widget indexes, -1 for none */
    float pos[3];             /* units of an 854x480 screen from the parent's centre, y up */
    float color[4];           /* multiplies the image; [3] is alpha */
    float size[3];            /* 0: the image's own size */
    float scale[3];
    int anchor;               /* what pos is: low 4 bits x (0 the image's centre, 1 its left edge,
                                 2 its right edge), next 4 bits y (0 centre, 1 top, 2 bottom) */
    int image;                /* image index, or -1 */
    rco_action action[RCO_BTN_ACTIONS];      /* buttons: RCO_BTN_*; pages: RCO_PAGE_* */
    rco_action init;          /* planes, groups and buttons: run as their page opens (Underworld
                                 Evolution mirrors its ornaments with a turn of pi) */
} rco_object;

enum {
    RCO_STEP_MOVE = 2,
    RCO_STEP_COLOR = 3,
    RCO_STEP_ROTATE = 4,      /* x y z in radians: Transporter turns its menu dial */
    RCO_STEP_SCALE = 5,
    RCO_STEP_FADE = 6,
    RCO_STEP_DELAY = 7,
    RCO_STEP_EVENT = 8,
    RCO_STEP_LOCK = 9,
    RCO_STEP_UNLOCK = 10,
    RCO_STEP_CROP = 11        /* x y w h, fractions of the image: The Da Vinci Code scrolls its cryptex strips */
};

typedef struct rco_step {
    int kind;                 /* RCO_STEP_* */
    int object;               /* widget index, or -1 */
    float duration;           /* ms; for a delay, the wait */
    float accel;
    float value[4];           /* move, rotate and scale: x y z; colour: r g b a; fade: alpha */
    const char *event;        /* RCO_STEP_EVENT: a script handler to call, or NULL */
    int anim;                 /* RCO_STEP_EVENT: an animation to play (Alien chains its menu's
                                 fade-in this way), or -1 */
} rco_step;

typedef struct rco_anim {
    const char *name;
    int first_step, step_count;
} rco_anim;

typedef struct rco_image {
    const char *name;
    const uint8_t *data;      /* PNG */
    uint32_t size;
} rco_image;

/* A sound effect: one VAG file per channel (resource.soundtable.sndOne in Kill Bill Vol. 2). */
typedef struct rco_sound {
    const char *name;
    int channels;             /* 1 or 2 */
    const uint8_t *data[2];   /* VAG */
    uint32_t size[2];
} rco_sound;
typedef struct rco {
    const uint8_t *file;
    uint32_t file_size;
    const uint8_t *script;    /* VSMX, or NULL */
    uint32_t script_size;
    rco_image *images;
    int image_count;
    rco_object *objects;
    int object_count;
    rco_anim *anims;
    int anim_count;
    rco_step *steps;
    int step_count;
    rco_sound *sounds;
    int sound_count;
} rco;

/* Returns 0 on success. data must outlive r. */
int rco_parse(rco *r, const uint8_t *data, uint32_t size);
void rco_free(rco *r);

int rco_find_object(const rco *r, const char *name);
int rco_find_anim(const rco *r, const char *name);
int rco_find_sound(const rco *r, const char *name);

#endif
