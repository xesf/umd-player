/* menutest: checks the UMD Video menu code on the host.
   menutest <file.rco>                print what src/core/rco.c reads */
#include <stdio.h>
#include <stdlib.h>

#include "../src/core/rco.h"

static uint8_t *load(const char *path, uint32_t *size)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *size = (uint32_t)n;
    return buf;
}

static const char *nm(const char *s) { return s ? s : "-"; }

int main(int argc, char **argv)
{
    uint32_t size;
    uint8_t *data;
    rco r;

    if (argc < 2 || !(data = load(argv[1], &size)) || rco_parse(&r, data, size) < 0) {
        fprintf(stderr, "usage: menutest <file.rco>\n");
        return 1;
    }
    printf("script %u bytes, %d images, %d widgets, %d animations, %d steps\n", r.script_size, r.image_count,
           r.object_count, r.anim_count, r.step_count);
    for (int i = 0; i < r.object_count; i++) {
        const rco_object *o = &r.objects[i];
        printf("widget %3d kind %02x %-22s parent %3d pos %g,%g scale %g alpha %g image %s", i, o->kind, nm(o->name),
               o->parent, o->pos[0], o->pos[1], o->scale[0], o->color[3], o->image >= 0 ? nm(r.images[o->image].name) : "-");
        for (int a = 0; a < RCO_BTN_ACTIONS; a++)
            if (o->action[a].event || o->action[a].object >= 0)
                printf(" [%d %s]", a, o->action[a].event ? o->action[a].event : nm(r.objects[o->action[a].object].name));
        printf("\n");
    }
    for (int i = 0; i < r.anim_count; i++) {
        const rco_anim *a = &r.anims[i];
        printf("anim %-26s %d steps:", nm(a->name), a->step_count);
        for (int k = 0; k < a->step_count && k < 6; k++) {
            const rco_step *s = &r.steps[a->first_step + k];
            printf(" %d(%s %g)", s->kind, s->object >= 0 ? nm(r.objects[s->object].name) : nm(s->event), s->duration);
        }
        printf("\n");
    }
    rco_free(&r);
    free(data);
    return 0;
}
