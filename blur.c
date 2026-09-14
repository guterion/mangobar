#include "blur.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// One pass of a box filter with half-width k, along the rows or along the
// columns. Pixels outside the buffer count as zero, which keeps the spot
// symmetric at the edges.
static void box_pass(const uint32_t *in, uint32_t *out, int w, int h, int k,
                     bool rows) {
  int n = rows ? w : h;
  int lines = rows ? h : w;
  uint32_t span = (uint32_t)(2 * k + 1);
  for (int l = 0; l < lines; l++) {
    uint32_t sum = 0;
    // At step i the window holds i - k through i + k: the pixel at i + k
    // enters, and the one at i - k - 1 leaves.
    for (int i = -k - 1; i < n; i++) {
      int enter = i + k;
      int leave = i - k - 1;
      if (enter >= 0 && enter < n)
        sum += rows ? in[l * w + enter] : in[enter * w + l];
      if (leave >= 0 && leave < n)
        sum -= rows ? in[l * w + leave] : in[leave * w + l];
      if (i >= 0) {
        uint32_t v = (sum + span / 2) / span;
        if (rows)
          out[l * w + i] = v;
        else
          out[i * w + l] = v;
      }
    }
  }
}

void blur_a8(const uint8_t *src, int w, int h, int stride, int r,
             uint8_t *dst) {
  if (w <= 0 || h <= 0 || r < 0)
    return;
  int dw = w + 2 * r;
  int dh = h + 2 * r;
  size_t size = (size_t)dw * (size_t)dh;

  // Two passes of half-width k reach 2k pixels, which stays inside r.
  int k = r / 2;
  if (k == 0) {
    memset(dst, 0, size);
    for (int y = 0; y < h; y++)
      memcpy(dst + (y + r) * dw + r, src + y * stride, (size_t)w);
    return;
  }

  uint32_t *a = calloc(size, sizeof(*a));
  uint32_t *b = calloc(size, sizeof(*b));
  if (!a || !b) {
    free(a);
    free(b);
    memset(dst, 0, size);
    return;
  }

  // The values carry eight bits of fraction through the passes, so the
  // rounding of each pass costs little of the mass.
  for (int y = 0; y < h; y++)
    for (int x = 0; x < w; x++)
      a[(y + r) * dw + x + r] = (uint32_t)src[y * stride + x] << 8;

  box_pass(a, b, dw, dh, k, true);
  box_pass(b, a, dw, dh, k, false);
  box_pass(a, b, dw, dh, k, true);
  box_pass(b, a, dw, dh, k, false);

  for (size_t i = 0; i < size; i++) {
    uint32_t v = (a[i] + 128) >> 8;
    dst[i] = v > 255 ? 255 : (uint8_t)v;
  }
  free(a);
  free(b);
}
