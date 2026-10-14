#ifndef VITA_SOURCE_H
#define VITA_SOURCE_H

#include "../../core/umd_source.h"

/* sceIo-backed umd_source for an ISO file on the memory card. */
int vita_source_open(umd_source *src, const char *path);
void vita_source_close(umd_source *src);

#endif
