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

// Fill the part of a w by h mask that the rectangle (x, y, rw, rh) covers.
static void fill_rect(uint8_t *m, int w, int h, int x, int y, int rw, int rh,
                      uint8_t v) {
  int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
  int x1 = x + rw > w ? w : x + rw, y1 = y + rh > h ? h : y + rh;
  for (int j = y0; j < y1; j++)
    if (x1 > x0)
      memset(m + j * w + x0, v, (size_t)(x1 - x0));
}

// The corners of the mask are square: a box with a border-radius gets a
// shadow with square corners.
uint8_t *box_shadow_mask(int w, int h, int x, int y, int blur, int spread,
                         bool inset, int *mx, int *my, int *mw, int *mh) {
  if (w <= 0 || h <= 0 || blur < 0)
    return NULL;
  int r = blur;

  if (!inset) {
    int sw = w + 2 * spread, sh = h + 2 * spread;
    if (sw <= 0 || sh <= 0)
      return NULL;
    *mx = x - spread - r;
    *my = y - spread - r;
    *mw = sw + 2 * r;
    *mh = sh + 2 * r;
    uint8_t *solid = malloc((size_t)sw * (size_t)sh);
    uint8_t *m = malloc((size_t)*mw * (size_t)*mh);
    if (!solid || !m) {
      free(solid);
      free(m);
      return NULL;
    }
    memset(solid, 255, (size_t)sw * (size_t)sh);
    blur_a8(solid, sw, sh, sw, r, m);
    free(solid);
    // CSS paints an outer shadow only outside the border box.
    fill_rect(m, *mw, *mh, -*mx, -*my, w, h, 0);
    return m;
  }

  // An inset shadow is the area outside the shrunk and moved box. The
  // canvas reaches past the box far enough that its own edge stays out of
  // the blur that falls on the box.
  int pad = r + 1;
  int cw = w + 2 * pad, ch = h + 2 * pad;
  uint8_t *canvas = malloc((size_t)cw * (size_t)ch);
  uint8_t *big = malloc((size_t)(cw + 2 * r) * (size_t)(ch + 2 * r));
  uint8_t *m = malloc((size_t)w * (size_t)h);
  if (!canvas || !big || !m) {
    free(canvas);
    free(big);
    free(m);
    return NULL;
  }
  memset(canvas, 255, (size_t)cw * (size_t)ch);
  int hw = w - 2 * spread, hh = h - 2 * spread;
  if (hw > 0 && hh > 0)
    fill_rect(canvas, cw, ch, pad + x + spread, pad + y + spread, hw, hh, 0);
  blur_a8(canvas, cw, ch, cw, r, big);
  for (int j = 0; j < h; j++)
    memcpy(m + j * w, big + (j + pad + r) * (cw + 2 * r) + pad + r, (size_t)w);
  free(canvas);
  free(big);
  *mx = 0;
  *my = 0;
  *mw = w;
  *mh = h;
  return m;
}
