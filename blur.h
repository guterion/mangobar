#ifndef MANGOBAR_BLUR_H
#define MANGOBAR_BLUR_H

#include <stdbool.h>
#include <stdint.h>

// Blur an 8-bit mask with two passes of a box filter in each direction.
// The source is w by h pixels in rows of stride bytes. dst holds
// (w + 2r) by (h + 2r) bytes, and the source lands at (r, r), so the blur
// has room to reach r pixels past each edge.
void blur_a8(const uint8_t *src, int w, int h, int stride, int r,
             uint8_t *dst);

// Make the alpha mask of one box-shadow layer for a box of w by h pixels.
// The mask starts at (*mx, *my) relative to the top left of the box, and it
// is *mw by *mh pixels. An outer mask is clear under the box, and an inset
// mask covers only the box. The caller frees the mask. The result is NULL
// when the layer has no area or when the allocation fails.
uint8_t *box_shadow_mask(int w, int h, int x, int y, int blur, int spread,
                         bool inset, int *mx, int *my, int *mw, int *mh);

#endif
