#ifndef FF6_GLYPH_UPLOAD_H
#define FF6_GLYPH_UPLOAD_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bridge.h"

enum { FF6_GLYPH_ATLAS_SIZE = 1024 };

typedef struct {
  int x, y, width, height;
} ff6_glyph_region;

/* The reference engine owns one glyph cache with two atlas pages. Keep only
 * handles observed through drawGlyph; other textures can have the same size. */
typedef struct {
  uint32_t page[2];
} ff6_glyph_textures;

static inline void ff6_register_glyph_texture(ff6_glyph_textures *textures,
                                            int page, uint32_t texture) {
  if (page >= 0 && page < 2 && texture && texture != UINT32_MAX)
    textures->page[page] = texture;
}

static inline int ff6_is_glyph_texture(const ff6_glyph_textures *textures,
                                      uint32_t texture) {
  return texture && texture != UINT32_MAX &&
         (texture == textures->page[0] || texture == textures->page[1]);
}

static inline void ff6_forget_glyph_texture(ff6_glyph_textures *textures,
                                          uint32_t texture) {
  for (int page = 0; page < 2; page++)
    if (textures->page[page] == texture)
      textures->page[page] = 0;
}

static inline int ff6_make_glyph_region(int slot, int cell, int width,
                                      int height, ff6_glyph_region *region) {
  if (!region || slot < 0 || cell <= 0 || cell > FF6_GLYPH_ATLAS_SIZE ||
      width <= 0 || width > TEXT_IMG_W || height <= 0 || height > TEXT_IMG_H)
    return 0;

  int per_row = FF6_GLYPH_ATLAS_SIZE / cell;
  region->x = (slot % per_row) * cell;
  /* Both native drawGlyph and Draw wrap the row. The native registration
   * guard admits slot == per_row*per_row before resetting the cache. */
  region->y = ((slot / per_row) % per_row) * cell;
  region->width = width < cell ? width : cell;
  region->height = height < cell ? height : cell;
  return 1;
}

/* region must come from ff6_make_glyph_region. Its bounds limit allocation
 * to TEXT_IMG_W*TEXT_IMG_H pixels and keep every source row in the bitmap.
 * The caller owns the reusable buffer; allocation failure leaves it intact. */
static inline int ff6_pack_glyph(const uint32_t *pixels,
                                const ff6_glyph_region *region,
                                uint32_t **buffer, size_t *capacity) {
  size_t count = (size_t)region->width * (size_t)region->height;
  if (*capacity < count) {
    uint32_t *grown = (uint32_t *)realloc(*buffer, count * sizeof(*grown));
    if (!grown)
      return 0;
    *buffer = grown;
    *capacity = count;
  }
  for (int y = 0; y < region->height; y++)
    memcpy(*buffer + (size_t)y * region->width,
           pixels + (size_t)y * TEXT_IMG_W,
           (size_t)region->width * sizeof(**buffer));
  return 1;
}

#endif
