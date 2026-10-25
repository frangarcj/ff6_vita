#include <assert.h>
#include <stdio.h>

#include "../loader/glyph_upload.h"

static void upload(uint32_t *atlas, const ff6_glyph_region *region,
                   const uint32_t *packed) {
  for (int y = 0; y < region->height; y++)
    memcpy(atlas + (size_t)(region->y + y) * FF6_GLYPH_ATLAS_SIZE + region->x,
           packed + (size_t)y * region->width,
           (size_t)region->width * sizeof(*packed));
}

int main(void) {
  const size_t source_count = (size_t)TEXT_IMG_W * TEXT_IMG_H;
  const size_t atlas_count = (size_t)FF6_GLYPH_ATLAS_SIZE * FF6_GLYPH_ATLAS_SIZE;
  const uint32_t neighbor = 0x12345678u;
  uint32_t *source = (uint32_t *)malloc(source_count * sizeof(*source));
  uint32_t *atlas = (uint32_t *)malloc(atlas_count * sizeof(*atlas));
  uint32_t *packed = NULL;
  size_t capacity = 0;
  ff6_glyph_region region;
  int ok = 1;
  assert(source && atlas);
  for (size_t i = 0; i < atlas_count; i++)
    atlas[i] = neighbor;
  for (size_t i = 0; i < source_count; i++)
    source[i] = 0xFFFFFFFFu;

  assert(ff6_make_glyph_region(33, 32, 32, 32, &region));
  assert(ff6_pack_glyph(source, &region, &packed, &capacity));
  upload(atlas, &region, packed);

  /* A narrow glyph reuses a formerly full cell. Dirty source padding must
   * not become atlas ink, and the old glyph must not survive beside it. */
  for (int y = 0; y < 18; y++)
    for (int x = 0; x < 6; x++)
      source[y * TEXT_IMG_W + x] = 0x80402010u;
  assert(ff6_make_glyph_region(33, 32, 6, 18, &region));
  assert(ff6_pack_glyph(source, &region, &packed, &capacity));
  upload(atlas, &region, packed);
  for (int y = 0; y < FF6_GLYPH_ATLAS_SIZE; y++) {
    for (int x = 0; x < FF6_GLYPH_ATLAS_SIZE; x++) {
      uint32_t expected = neighbor;
      if (x >= 32 && x < 64 && y >= 32 && y < 64)
        expected = (x < 38 && y < 50) ? 0x80402010u : 0;
      if (atlas[(size_t)y * FF6_GLYPH_ATLAS_SIZE + x] != expected) {
        fprintf(stderr, "reused glyph cell retains wrong pixels at (%d,%d)\n", x, y);
        ok = 0;
        goto done;
      }
    }
  }

  /* Blank/zero-advance glyphs must erase the reused cell, not skip upload. */
  assert(ff6_make_glyph_region(33, 32, 0, 18, &region));
  assert(ff6_pack_glyph(source, &region, &packed, &capacity));
  upload(atlas, &region, packed);
  for (int y = 32; y < 64; y++)
    for (int x = 32; x < 64; x++)
      assert(atlas[(size_t)y * FF6_GLYPH_ATLAS_SIZE + x] == 0);

 done:
  free(packed);
  free(atlas);
  free(source);
  if (ok)
    puts("glyph cell reuse tests passed");
  return ok ? 0 : 1;
}
