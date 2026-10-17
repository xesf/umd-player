#ifndef APP_MANUAL_H
#define APP_MANUAL_H

#include <stdint.h>

/* The manual inside the app: F1 on a computer, Start in the library on the Vita. Its pages are
   the Vita manual's (tools/make_manual.py, from docs/manual.md), built into the binary, with
   a page of keyboard and gamepad keys first on computers. */

#define MANUAL_PAGES 11       /* the keys page, then the 10 pages of the Vita manual */

typedef struct manual_page {
    const unsigned char *png;
    unsigned int size;
} manual_page;

extern const manual_page manual_pages[MANUAL_PAGES];

/* Shows the manual, from its keys page when keys is set. */
void manual_open(int keys);
/* Turns pages, or closes the manual. Returns 1 while it is open; it then has the screen and
   the buttons. */
int manual_update(uint32_t pressed);

#endif
