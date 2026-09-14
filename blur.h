#ifndef MANGOBAR_BLUR_H
#define MANGOBAR_BLUR_H

#include <stdint.h>

// Blur an 8-bit mask with two passes of a box filter in each direction.
// The source is w by h pixels in rows of stride bytes. dst holds
// (w + 2r) by (h + 2r) bytes, and the source lands at (r, r), so the blur
// has room to reach r pixels past each edge.
void blur_a8(const uint8_t *src, int w, int h, int stride, int r,
             uint8_t *dst);

#endif
