#ifndef FF6_SHARP_BILINEAR_H
#define FF6_SHARP_BILINEAR_H

#include <math.h>

/* Equivalent to an integer nearest prescale followed by a bilinear resize,
 * without allocating the intermediate image. Ceil makes the 1.7x case use
 * a 2x prescale instead of degenerating into ordinary bilinear. */
static inline float ff6_sharp_prescale(int source, int destination) {
  if (source <= 0 || destination <= source)
    return 1.0f;
  return ceilf((float)destination / (float)source);
}

static inline float ff6_sharp_texel(float texel, float prescale) {
  float base = floorf(texel);
  float distance = texel - base - 0.5f;
  float radius = 0.5f - 0.5f / prescale;
  float clipped = fminf(radius, fmaxf(-radius, distance));
  return base + 0.5f + (distance - clipped) * prescale;
}

#endif
