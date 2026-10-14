#ifndef MAC_FRAME_H
#define MAC_FRAME_H

#include <CoreVideo/CoreVideo.h>

/* A decoded frame on macOS: an IOSurface-backed BGRA pixel buffer. */
struct plat_frame {
    CVPixelBufferRef image;
    int deinterlaced;   /* the field blend has been applied in place */
};

#endif
