#include <assert.h>
#include <stdio.h>
#include "../loader/sharp_bilinear.h"

static float pixel(int i) {
  if (i < 0) i = 0;
  if (i > 15) i = 15;
  return (float)((i * 7) % 11) / 10.0f;
}

static float linear_source(float coordinate) {
  float position = coordinate - 0.5f;
  int left = (int)floorf(position);
  float blend = position - left;
  return pixel(left) * (1 - blend) + pixel(left + 1) * blend;
}

static int floor_div(int n, int d) {
  int q = n / d;
  return q - (n < 0 && n % d != 0);
}

static float reference_prescaled(float coordinate, int scale) {
  float position = coordinate * scale - 0.5f;
  int left = (int)floorf(position);
  float blend = position - left;
  return pixel(floor_div(left, scale)) * (1 - blend) +
         pixel(floor_div(left + 1, scale)) * blend;
}

int main(void) {
  assert(ff6_sharp_prescale(480, 816) == 2);
  assert(ff6_sharp_prescale(320, 544) == 2);
  assert(ff6_sharp_prescale(480, 480) == 1);
  assert(ff6_sharp_prescale(480, 320) == 1);
  assert(ff6_sharp_prescale(0, 816) == 1);
  for (int scale = 1; scale <= 4; scale++) {
    float previous = -100;
    for (int i = -50; i < 16500; i++) {
      float coordinate = (float)i / 1000.0f;
      float mapped = ff6_sharp_texel(coordinate, (float)scale);
      assert(mapped >= previous - 0.00001f);
      previous = mapped;
      float result = linear_source(mapped);
      float reference = reference_prescaled(coordinate, scale);
      assert(fabsf(result - reference) < 0.00001f);
      if (scale == 1)
        assert(fabsf(mapped - coordinate) < 0.00001f);
    }
  }
  assert(ff6_sharp_texel(3.30f, 2) == 3.5f);
  assert(ff6_sharp_texel(3.70f, 2) == 3.5f);
  puts("sharp-bilinear matches nearest integer prescale plus bilinear resize");
  return 0;
}
