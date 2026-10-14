#ifndef HOST_SOURCE_H
#define HOST_SOURCE_H

#include "../src/core/umd_source.h"

/* stdio-backed umd_source for host tools. */
int host_source_open(umd_source *src, const char *path);
void host_source_close(umd_source *src);

#endif
