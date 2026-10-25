#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../loader/font_raster.h"

#define WIDTH 64
#define HEIGHT 64
#define STRIDE 73
#define GUARD 32

static unsigned char *load_font(const char *path, stbtt_fontinfo *font) {
  FILE *stream = fopen(path, "rb");
  assert(stream);
  assert(fseek(stream, 0, SEEK_END) == 0);
  long size = ftell(stream);
  assert(size > 0 && size < 32 * 1024 * 1024);
  rewind(stream);
  unsigned char *bytes = malloc((size_t)size);
  assert(bytes && fread(bytes, 1, (size_t)size, stream) == (size_t)size);
  assert(fclose(stream) == 0);
  assert(stbtt_InitFont(font, bytes, 0));
  return bytes;
}

/* The reference is stb's complete bitmap, cropped after rasterization. This
 * catches the old all-or-nothing rejection at each edge and stride mistakes. */
static void compare_crop(const stbtt_fontinfo *font, int codepoint, float scale,
                         int origin, int baseline) {
  unsigned char storage[GUARD + STRIDE * HEIGHT + GUARD];
  unsigned char expected[WIDTH * HEIGHT] = {0};
  memset(storage, 0xa5, sizeof(storage));
  unsigned char *canvas = storage + GUARD;
  for (int y = 0; y < HEIGHT; y++)
    memset(canvas + y * STRIDE, 0, WIDTH);
  int w, h, xoff, yoff;
  unsigned char *full = stbtt_GetCodepointBitmap(font, scale, scale, codepoint,
                                                &w, &h, &xoff, &yoff);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      int dx = origin + xoff + x, dy = baseline + yoff + y;
      if (dx >= 0 && dx < WIDTH && dy >= 0 && dy < HEIGHT)
        expected[dy * WIDTH + dx] = full[y * w + x];
    }
  }
  stbtt_FreeBitmap(full, font->userdata);
  assert(ff6_raster_glyph(font, codepoint, scale, origin, baseline,
                          canvas, WIDTH, HEIGHT, STRIDE));
  for (int y = 0; y < HEIGHT; y++) {
    for (int x = 0; x < WIDTH; x++) {
      int delta = abs((int)canvas[y * STRIDE + x] - expected[y * WIDTH + x]);
      if (delta != 0)
        fprintf(stderr, "crop U+%04x scale=%g origin=%d baseline=%d at %d,%d: got %u expected %u\n",
                codepoint, scale, origin, baseline, x, y,
                canvas[y * STRIDE + x], expected[y * WIDTH + x]);
      assert(delta == 0);
    }
    for (int x = WIDTH; x < STRIDE; x++)
      assert(canvas[y * STRIDE + x] == 0xa5);
  }
  for (int i = 0; i < GUARD; i++) {
    assert(storage[i] == 0xa5);
    assert(storage[GUARD + STRIDE * HEIGHT + i] == 0xa5);
  }
}

static void test_font(const char *path) {
  stbtt_fontinfo font;
  unsigned char *bytes = load_font(path, &font);
  const int sizes[] = {19, 28, 70};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
    ff6_font_metrics metrics = ff6_measure_font(&font, sizes[i], 100, 640);
    assert(fabsf(metrics.scale - stbtt_ScaleForMappingEmToPixels(
                     &font, (float)sizes[i])) < 0.000001f);
    assert(metrics.ascent > 0 && metrics.descent >= 0);
    assert(metrics.ascent + metrics.descent <= 640);
    compare_crop(&font, 'j', metrics.scale, 0, metrics.ascent);
    compare_crop(&font, 'j', metrics.scale, -3, metrics.ascent);
    compare_crop(&font, 'A', metrics.scale, 7, 3);
    compare_crop(&font, 'W', metrics.scale, WIDTH - 2, metrics.ascent);
    compare_crop(&font, 'g', metrics.scale, 7, HEIGHT + 1);
    compare_crop(&font, 'W', metrics.scale, WIDTH + 10, metrics.ascent);
    compare_crop(&font, ' ', metrics.scale, 0, metrics.ascent);
  }

  ff6_font_metrics metrics = ff6_measure_font(&font, 19, 100, 640);
  const int extra[] = {0x00e1, 0x00e9, 0x00ed, 0x00f3, 0x00fa, 0x00f1,
                       0x00fc, 0x00bf, 0x00a1, 0x0416, 0x4e16};
  unsigned char canvas[WIDTH * HEIGHT];
  for (int cp = 33; cp <= 126; cp++) {
    memset(canvas, 0, sizeof(canvas));
    assert(stbtt_FindGlyphIndex(&font, cp) != 0);
    assert(ff6_raster_glyph(&font, cp, metrics.scale, 0, metrics.ascent,
                            canvas, WIDTH, HEIGHT, WIDTH));
    size_t ink = 0;
    for (size_t j = 0; j < sizeof(canvas); j++) ink += canvas[j] != 0;
    assert(ink > 0);
  }
  for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
    assert(stbtt_FindGlyphIndex(&font, extra[i]) != 0);
    compare_crop(&font, extra[i], metrics.scale, 0, metrics.ascent);
  }

  memset(canvas, 127, sizeof(canvas));
  assert(ff6_raster_glyph(&font, 'j', metrics.scale, 0, metrics.ascent,
                          canvas, WIDTH, HEIGHT, WIDTH));
  for (size_t i = 0; i < sizeof(canvas); i++) assert(canvas[i] >= 127);

  ff6_font_metrics capped = ff6_measure_font(&font, INT_MAX, INT_MAX, 640);
  assert(isfinite(capped.scale));
  assert(capped.ascent + capped.descent <= 640);
  assert(capped.ascent > 0);
  printf("%s: ASCII, accents, edge clipping, overlap, stride and size limits passed\n", path);
  free(bytes);
}

int main(void) {
  test_font("NotoSansJP-Regular.ttf");
  test_font("NotoSansSC-Regular.ttf");
  test_font("NotoSansKR-Regular.ttf");
  return 0;
}
