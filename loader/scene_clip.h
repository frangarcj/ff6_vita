#ifndef FF6_SCENE_CLIP_H
#define FF6_SCENE_CLIP_H

#include <math.h>
#include <stddef.h>

/* Scissor rectangles are framebuffer coordinates, not projection-space
 * coordinates. Transform both edges, round outwards, and intersect with the
 * target. Use double before subtraction/addition to avoid signed overflow. */
static inline int ff6_scene_clip(const int rect[4], const int viewport[4],
                                 int width, int height, int output[4]) {
  if (!rect || !viewport || !output || width <= 0 || height <= 0 ||
      viewport[2] <= 0 || viewport[3] <= 0 || rect[2] < 0 || rect[3] < 0)
    return 0;
  for (int axis = 0; axis < 2; axis++) {
    int limit = axis ? height : width;
    double begin = (double)rect[axis] - viewport[axis];
    double end = begin + rect[axis + 2];
    double scale = (double)limit / viewport[axis + 2];
    double low = fmax(0.0, fmin((double)limit, floor(begin * scale)));
    double high = fmax(0.0, fmin((double)limit, ceil(end * scale)));
    output[axis] = (int)low;
    output[axis + 2] = rect[axis + 2] ? (int)(high - low) : 0;
  }
  return 1;
}

#endif
