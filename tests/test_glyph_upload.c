#include <assert.h>
#include <limits.h>
#include <stdio.h>

#include "../loader/glyph_upload.h"

static void test_font_texture_lifetime(void) {
  ff6_glyph_textures textures = {{0, 0}};
  assert(!ff6_is_glyph_texture(&textures, 0));
  assert(!ff6_is_glyph_texture(&textures, 17));
  ff6_register_glyph_texture(&textures, 0, 17);
  ff6_register_glyph_texture(&textures, 1, 18);
  assert(ff6_is_glyph_texture(&textures, 17));
  assert(ff6_is_glyph_texture(&textures, 18));
  assert(!ff6_is_glyph_texture(&textures, 19));

  /* A deleted atlas name reused for art must no longer get font filtering. */
  ff6_forget_glyph_texture(&textures, 17);
  assert(!ff6_is_glyph_texture(&textures, 17));
  assert(ff6_is_glyph_texture(&textures, 18));
  ff6_register_glyph_texture(&textures, 0, 25);
  assert(ff6_is_glyph_texture(&textures, 25));
  ff6_register_glyph_texture(&textures, 0, 26);
  assert(!ff6_is_glyph_texture(&textures, 25));
  assert(ff6_is_glyph_texture(&textures, 26));
  ff6_register_glyph_texture(&textures, 0, 0);
  ff6_register_glyph_texture(&textures, 1, UINT32_MAX);
  ff6_register_glyph_texture(&textures, 2, 99);
  assert(ff6_is_glyph_texture(&textures, 26));
  assert(ff6_is_glyph_texture(&textures, 18));
  assert(!ff6_is_glyph_texture(&textures, 0));
  assert(!ff6_is_glyph_texture(&textures, UINT32_MAX));
  assert(!ff6_is_glyph_texture(&textures, 99));
}

static void test_atlas_boundary(void) {
  ff6_glyph_region region;
  assert(ff6_make_glyph_region(1023, 32, 32, 32, &region));
  assert(region.x == 992 && region.y == 992);
  /* Regression: the old hook placed this native-admitted slot at y=1024. */
  assert(ff6_make_glyph_region(1024, 32, 32, 32, &region));
  assert(region.x == 0 && region.y == 0);
  assert(ff6_make_glyph_region(1368, 27, 27, 27, &region));
  assert(region.x == 972 && region.y == 972);
  assert(ff6_make_glyph_region(1369, 27, 27, 27, &region));
  assert(region.x == 0 && region.y == 0);
  assert(ff6_make_glyph_region(1, 1024, 1024, 640, &region));
  assert(region.x == 0 && region.y == 0);
  assert(region.width == 1024 && region.height == 640);

  /* Keep the existing cell clipping behavior, even for oversized metrics. */
  assert(ff6_make_glyph_region(1023, 32, 90, 70, &region));
  assert(region.width == 32 && region.height == 32);
  assert(!ff6_make_glyph_region(-1, 32, 20, 20, &region));
  assert(!ff6_make_glyph_region(0, 0, 20, 20, &region));
  assert(!ff6_make_glyph_region(0, INT_MAX, 20, 20, &region));
  assert(!ff6_make_glyph_region(0, 32, INT_MAX, 20, &region));
  assert(!ff6_make_glyph_region(0, 32, 20, INT_MAX, &region));
  assert(!ff6_make_glyph_region(0, 32, 0, 20, &region));
  assert(!ff6_make_glyph_region(0, 32, 20, 0, &region));
  assert(!ff6_make_glyph_region(0, 32, 20, 20, NULL));
}

static void test_large_strided_upload(void) {
  const size_t pixel_count = (size_t)TEXT_IMG_W * TEXT_IMG_H;
  uint32_t *source = (uint32_t *)malloc(pixel_count * sizeof(*source));
  uint32_t *packed = NULL;
  size_t capacity = 0;
  ff6_glyph_region region;
  assert(source);
  for (int y = 0; y < TEXT_IMG_H; y++)
    for (int x = 0; x < TEXT_IMG_W; x++)
      source[y * TEXT_IMG_W + x] = ((uint32_t)y << 16) | (uint32_t)x;

  /* Regression: a valid glyph larger than 64 was silently dropped. The
   * bitmap still has a 1024-pixel stride, not the glyph's upload width. */
  assert(ff6_make_glyph_region(3, 96, 65, 80, &region));
  assert(ff6_pack_glyph(source, &region, &packed, &capacity));
  assert(capacity == 65 * 80);
  for (int y = 0; y < 80; y++)
    for (int x = 0; x < 65; x++)
      assert(packed[y * 65 + x] == (((uint32_t)y << 16) | (uint32_t)x));

  uint32_t *previous = packed;
  packed[32 * 32] = 0xA5A5A5A5;
  assert(ff6_make_glyph_region(0, 32, 65, 80, &region));
  assert(ff6_pack_glyph(source, &region, &packed, &capacity));
  assert(packed == previous && capacity == 65 * 80);
  assert(packed[31 * 32 + 31] == 0x001F001F);
  assert(packed[32 * 32] == 0xA5A5A5A5);

  assert(ff6_make_glyph_region(0, 1024, 1024, 640, &region));
  assert(ff6_pack_glyph(source, &region, &packed, &capacity));
  assert(capacity == pixel_count);
  assert(memcmp(source, packed, pixel_count * sizeof(*source)) == 0);
  free(packed);
  free(source);
}

int main(void) {
  test_font_texture_lifetime();
  test_atlas_boundary();
  test_large_strided_upload();
  puts("glyph upload tests passed");
  return 0;
}
