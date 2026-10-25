#ifndef FF6_FONT_RASTER_H
#define FF6_FONT_RASTER_H

#include <math.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include "stb_truetype.h"

typedef struct {
  float scale;
  int ascent;
  int descent;
} ff6_font_metrics;

/* Paint.setTextSize is an EM size, not the font's ascent/descent line box.
 * Keep a separate, conservatively rounded line box for our substituted font;
 * Android's system-font top/bottom metrics are not available on Vita. */
static inline ff6_font_metrics ff6_measure_font(const stbtt_fontinfo *font,
                                               int size, int percent,
                                               int max_height) {
  ff6_font_metrics result = {0.0f, 0, 0};
  int ascent, descent, gap;
  if (!font || max_height < 3)
    return result;
  stbtt_GetFontVMetrics(font, &ascent, &descent, &gap);
  if (ascent <= 0 || descent > 0 || ascent <= descent)
    return result;
  if (size < 1)
    size = 1;
  if (percent < 1)
    percent = 100;
  float pixels = (float)size * ((float)percent / 100.0f);
  float scale = stbtt_ScaleForMappingEmToPixels(font, pixels);
  float limit = (float)(max_height - 2) / (float)(ascent - descent);
  if (!isfinite(scale) || scale > limit)
    scale = limit;
  result.scale = scale;
  result.ascent = (int)ceilf(ascent * scale);
  result.descent = (int)ceilf(-descent * scale);
  return result;
}

/* Rasterize the intersection with the canvas. A negative left bearing or a
 * descender at the bottom must clip pixels, never discard the whole letter.
 * Render before cropping: moving stb's rasterizer edge changes its coverage.
 * Compositing also avoids erasing earlier overlapping glyphs. */
static inline int ff6_raster_glyph(const stbtt_fontinfo *font, int codepoint,
                                  float scale, int origin_x, int baseline,
                                  unsigned char *pixels, int width, int height,
                                  int stride) {
  if (!font || !pixels || scale <= 0.0f || !isfinite(scale) ||
      width <= 0 || height <= 0 || stride < width)
    return 0;

  int glyph = stbtt_FindGlyphIndex(font, codepoint);
  int x0, y0, x1, y1;
  stbtt_GetGlyphBitmapBox(font, glyph, scale, scale, &x0, &y0, &x1, &y1);
  int left = origin_x + x0;
  int top = baseline + y0;
  int right = origin_x + x1;
  int bottom = baseline + y1;
  if (left < 0) left = 0;
  if (top < 0) top = 0;
  if (right > width) right = width;
  if (bottom > height) bottom = height;
  if (left >= right || top >= bottom)
    return 1;

  int glyph_width = x1 - x0;
  int glyph_height = y1 - y0;
  if (glyph_width <= 0 || glyph_height <= 0 ||
      glyph_width > INT_MAX / glyph_height)
    return 0;
  int clipped_width = right - left;
  int clipped_height = bottom - top;
  unsigned char *coverage = stbtt_GetGlyphBitmap(font, scale, scale, glyph,
                                                NULL, NULL, NULL, NULL);
  if (!coverage)
    return 0;

  for (int y = 0; y < clipped_height; y++) {
    unsigned char *dst = pixels + (size_t)(top + y) * stride + left;
    const unsigned char *src = coverage +
        (size_t)(top - baseline - y0 + y) * glyph_width + left - origin_x - x0;
    for (int x = 0; x < clipped_width; x++) {
      if (src[x] > dst[x])
        dst[x] = src[x];
    }
  }
  stbtt_FreeBitmap(coverage, font->userdata);
  return 1;
}

#endif
