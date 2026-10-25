#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../loader/font_raster.h"

static unsigned char *load_font(const char *path, stbtt_fontinfo *font) {
  FILE *stream = fopen(path, "rb");
  assert(stream && fseek(stream, 0, SEEK_END) == 0);
  long length = ftell(stream);
  assert(length > 0 && length < 32 * 1024 * 1024);
  rewind(stream);
  unsigned char *data = malloc((size_t)length);
  assert(data && fread(data, 1, (size_t)length, stream) == (size_t)length);
  assert(fclose(stream) == 0 && stbtt_InitFont(font, data, 0));
  return data;
}

/* Compare complete stb coverage, not merely bounding boxes. Padding and guard
 * bytes also catch writes outside the bitmap when moving the baseline. */
static void verify_complete_ink(const stbtt_fontinfo *font, int cp,
                                ff6_font_metrics m) {
  int width, height, xoff, yoff;
  unsigned char *reference = stbtt_GetCodepointBitmap(
      font, m.scale, m.scale, cp, &width, &height, &xoff, &yoff);
  if (!width || !height) {
    stbtt_FreeBitmap(reference, font->userdata);
    return;
  }
  assert(reference);
  const int canvas_w = width + 4;
  const int canvas_h = m.ascent + m.descent;
  const int stride = canvas_w + 5;
  const size_t bytes = (size_t)stride * canvas_h;
  unsigned char *allocation = malloc(bytes + 128);
  assert(allocation);
  memset(allocation, 0xA5, bytes + 128);
  unsigned char *canvas = allocation + 64;
  for (int y = 0; y < canvas_h; ++y)
    memset(canvas + (size_t)y * stride, 0, (size_t)canvas_w);

  assert(m.baseline + yoff >= 0 && m.baseline + yoff + height <= canvas_h);
  assert(ff6_raster_glyph(font, cp, m.scale, 2 - xoff, m.baseline,
                          canvas, canvas_w, canvas_h, stride));
  for (int y = 0; y < canvas_h; ++y) {
    const int sy = y - m.baseline - yoff;
    for (int x = 0; x < stride; ++x) {
      unsigned char expected = x >= canvas_w ? 0xA5 : 0;
      if (x >= 2 && x < width + 2 && sy >= 0 && sy < height)
        expected = reference[(size_t)sy * width + x - 2];
      assert(canvas[(size_t)y * stride + x] == expected);
    }
  }
  for (size_t i = 0; i < 64; ++i) {
    assert(allocation[i] == 0xA5);
    assert(allocation[64 + bytes + i] == 0xA5);
  }
  free(allocation);
  stbtt_FreeBitmap(reference, font->userdata);
}

static void verify_font(const char *path) {
  stbtt_fontinfo font;
  unsigned char *data = load_font(path, &font);
  const int sizes[] = {12, 16, 19, 24, 28, 32, 70};
  const int extra[] = {0xc1, 0xc9, 0xcd, 0xd1, 0xd3, 0xda, 0xdc,
                       0xe1, 0xe9, 0xed, 0xf1, 0xf3, 0xfa, 0xfc,
                       0x416, 0x419, 0x4e16, 0x65e5, 0xd55c};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    ff6_font_metrics m = ff6_measure_font(&font, sizes[i], 100, 640);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
    // Alignment must not shrink the font, the reported box or the atlas cell.
    assert(fabsf(m.scale - stbtt_ScaleForMappingEmToPixels(&font, sizes[i])) < 1e-6f);
    assert(m.ascent == (int)ceilf(ascent * m.scale));
    assert(m.descent == (int)ceilf(-descent * m.scale));
    const int height = m.ascent + m.descent;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font, 'H', m.scale, m.scale, &x0, &y0, &x1, &y1);
    const int doubled_offset = 2 * m.baseline + y0 + y1 - height;
    if (abs(doubled_offset) > 1) {
      fprintf(stderr, "%s size=%d: capital centre offset=%g bitmap pixels\n",
              path, sizes[i], doubled_offset / 2.0);
      exit(1);
    }
    // A single font/size baseline is shared by every word. Accents and
    // descenders must retain all vertical ink rather than being recentered.
    for (int cp = 33; cp <= 126; ++cp) {
      stbtt_GetCodepointBitmapBox(&font, cp, m.scale, m.scale, &x0, &y0, &x1, &y1);
      assert(m.baseline + y0 >= 0 && m.baseline + y1 <= height);
      verify_complete_ink(&font, cp, m);
    }
    for (size_t j = 0; j < sizeof(extra) / sizeof(extra[0]); ++j) {
      if (!stbtt_FindGlyphIndex(&font, extra[j])) continue;
      stbtt_GetCodepointBitmapBox(&font, extra[j], m.scale, m.scale, &x0, &y0, &x1, &y1);
      assert(m.baseline + y0 >= 0 && m.baseline + y1 <= height);
      verify_complete_ink(&font, extra[j], m);
    }
    if (sizes[i] == 19 || sizes[i] == 28)
      printf("%s size=%d: box=%d, baseline %d -> %d\n",
             path, sizes[i], height, m.ascent, m.baseline);
  }

  const int percentages[] = {69, 100, 125, 200};
  for (size_t i = 0; i < sizeof(percentages) / sizeof(percentages[0]); ++i) {
    ff6_font_metrics m = ff6_measure_font(&font, 19, percentages[i], 640);
    assert(fabsf(m.scale - stbtt_ScaleForMappingEmToPixels(
                    &font, 19.0f * percentages[i] / 100.0f)) < 1e-6f);
    verify_complete_ink(&font, 'H', m);
    verify_complete_ink(&font, 'g', m);
    verify_complete_ink(&font, 0xc1, m);
  }
  ff6_font_metrics m = ff6_measure_font(&font, INT_MAX, INT_MAX, 640);
  assert(m.ascent + m.descent <= 640);
  assert(m.baseline >= 0 && m.baseline <= m.ascent + m.descent);
  m = ff6_measure_font(&font, 19, 100, 2);
  assert(m.scale == 0 && m.baseline == 0);
  free(data);
}

int main(void) {
  verify_font("NotoSansJP-Regular.ttf");
  verify_font("NotoSansSC-Regular.ttf");
  verify_font("NotoSansKR-Regular.ttf");
  puts("font alignment: stable capital centre, exact complete ink and intact guards");
  return 0;
}
