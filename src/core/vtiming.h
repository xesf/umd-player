#ifndef VTIMING_H
#define VTIMING_H

#include <stdint.h>
#include "h264.h"

/* Display timestamps for H.264 access units. Only some access units carry a PES PTS, so the
   others get one from their display-order predecessor (the next lower picture order count):
     t = t(predecessor) + fields(predecessor) * field duration
   where fields comes from the SEI pic_struct (3:2 pulldown gives 2 or 3, a field picture 1).
   Every PES PTS re-anchors the chain, except that about 1 PES PTS in 2000 on the test discs is
   tied to the wrong access unit by the muxer. So an anchor that disagrees with the chain is used
   only when the next anchor disagrees by the same amount (a real jump); otherwise the chain wins.
   Access units go in in decode order. An access unit's
   predecessor is chosen once VTIMING_SETTLE later access units have arrived, because reordering
   is never deeper; a timestamp is known at the latest 3 * VTIMING_SETTLE access units later. */

#define VTIMING_WINDOW 32
#define VTIMING_SETTLE 4
#define VTIMING_UNKNOWN INT64_MIN

typedef struct vtiming_entry {
    uint32_t id;              /* decode index */
    uint32_t epoch;           /* increments at every IDR, because the POC restarts there */
    int poc;
    uint8_t fields;
    uint8_t anchored;         /* has a PES PTS */
    uint8_t lost;             /* can never be timed: no PTS and nothing before it in display order */
    int64_t anchor;           /* the PES PTS in 180 kHz ticks */
    int64_t t;                /* 180 kHz ticks, VTIMING_UNKNOWN until resolved */
    int64_t predicted;        /* what the chain predicted, VTIMING_UNKNOWN if it had no predecessor */
} vtiming_entry;

typedef struct vtiming {
    vtiming_entry e[VTIMING_WINDOW];
    uint32_t count;           /* entries in the window, oldest first at e[0] */
    uint32_t next_id;
    uint32_t epoch;
    uint32_t field_ticks;     /* one field in 180 kHz ticks; 3003 for 59.94 fields per second */
    int64_t pending_jump;     /* offset of the last disagreeing anchor, 0 if none */
    uint32_t outliers;        /* anchors overridden by the chain */
    uint32_t jumps;           /* anchors accepted after the next one confirmed them */
} vtiming;

void vtiming_reset(vtiming *v, const h264_sps *sps);

/* Adds an access unit in decode order. pes_pts is in 90 kHz or PS_NO_TS. Returns its id. */
uint32_t vtiming_push(vtiming *v, const h264_au_info *info, uint64_t pes_pts);

/* 1: *pts90 is the display time (90 kHz). 0: not known yet, ask again after more pushes.
   -1: will never be known (for example a leading picture after a seek, reported as soon as it
   is settled); do not display it. */
int vtiming_get(const vtiming *v, uint32_t id, uint64_t *pts90);

/* For tests: the predicted time of an entry (90 kHz), or -1 if it had no prediction. */
int64_t vtiming_predicted(const vtiming *v, uint32_t id);
/* For tests: the PES PTS of an entry (90 kHz), or -1 if it had none. */
int64_t vtiming_anchor(const vtiming *v, uint32_t id);

#endif
