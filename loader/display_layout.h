#ifndef FF6_DISPLAY_LAYOUT_H
#define FF6_DISPLAY_LAYOUT_H
#include <stdint.h>
#include <string.h>

/* Presentation only: the engine continues to compose its original 3:2 view. */
static inline void ff6_display_rect(const int native[4], int screen_w,
                                   int screen_h, int aspect, int integer_scale,
                                   int base_w, int base_h, int result[4]) {
  memcpy(result, native, 4 * sizeof(int));
  if (integer_scale && base_w > 0 && base_h > 0) {
    int scale = native[2] / base_w;
    if (native[3] / base_h < scale) scale = native[3] / base_h;
    if (scale > 0) {
      result[2] = base_w * scale; result[3] = base_h * scale;
      result[0] += (native[2] - result[2]) / 2;
      result[1] += (native[3] - result[3]) / 2;
    }
    return;
  }
  if (aspect < 1 || aspect > 4 || screen_w <= 0 || screen_h <= 0) return;
  static const int ratios[4][2] = {{4, 3}, {16, 9}, {2, 1}, {21, 9}};
  int n = ratios[aspect - 1][0], d = ratios[aspect - 1][1];
  /* Keep FF6's screen-space side panels outside the game. Expanding beneath
   * them would hide the left/right part of the stretched battle menus. */
  int width = native[2], height = native[3];
  if ((int64_t)width * d > (int64_t)height * n) {
    result[3] = height; result[2] = height * n / d;
  } else {
    result[2] = width; result[3] = width * d / n;
  }
  result[0] = native[0] + (width - result[2]) / 2;
  result[1] = native[1] + (height - result[3]) / 2;
}

/* Touch uses top-left coordinates; OpenGL viewports use bottom-left. Outside
 * the displayed world, leave side-panel coordinates alone. Blank areas inside
 * the old world become out-of-bounds, not accidentally clickable menu items. */
static inline void ff6_display_touch(const int native[4], const int display[4],
                                    int screen_h, float *x, float *y) {
  if (display[2] <= 0 || display[3] <= 0 ||
      memcmp(native, display, 4 * sizeof(int)) == 0) return;
  float top = (float)(screen_h - display[1] - display[3]);
  float native_top = (float)(screen_h - native[1] - native[3]);
  if (*x >= display[0] && *x < display[0] + display[2] &&
      *y >= top && *y < top + display[3]) {
    *x = native[0] + (*x - display[0]) * native[2] / display[2];
    *y = native_top + (*y - top) * native[3] / display[3];
  } else if (*x >= native[0] && *x < native[0] + native[2] &&
             *y >= native_top && *y < native_top + native[3]) {
    *x = *y = -1.0f;
  }
}
#endif
