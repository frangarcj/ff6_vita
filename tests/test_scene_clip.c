#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include "../loader/scene_clip.h"

static void expect(const int rect[4], const int viewport[4], int x, int y,
                   int width, int height) {
  int result[4];
  assert(ff6_scene_clip(rect, viewport, 480, 320, result));
  assert(result[0] == x && result[1] == y);
  assert(result[2] == width && result[3] == height);
}

int main(void) {
  const int viewport[] = {72, 0, 816, 544};
  const int full[] = {72, 0, 816, 544};
  const int display[] = {0, 0, 960, 544};
  const int menu[] = {276, 34, 204, 102};
  const int fractional[] = {73, 1, 1, 1};
  const int empty[] = {73, 1, 0, 0};
  const int outside[] = {960, 600, 10, 10};
  const int crossing[] = {55, -17, 34, 34};
  const int extreme[] = {INT_MAX, INT_MAX, INT_MAX, INT_MAX};
  const int invalid[] = {0, 0, -1, 1};
  int output[4];
  expect(full, viewport, 0, 0, 480, 320);
  expect(display, viewport, 0, 0, 480, 320);
  expect(menu, viewport, 120, 20, 120, 60);
  expect(fractional, viewport, 0, 0, 2, 2);
  expect(empty, viewport, 0, 0, 0, 0);
  expect(outside, viewport, 480, 320, 0, 0);
  expect(crossing, viewport, 0, 0, 10, 10);
  expect(extreme, viewport, 480, 320, 0, 0);
  assert(!ff6_scene_clip(invalid, viewport, 480, 320, output));
  assert(!ff6_scene_clip(full, empty, 480, 320, output));
  assert(!ff6_scene_clip(NULL, viewport, 480, 320, output));
  assert(!ff6_scene_clip(full, NULL, 480, 320, output));
  assert(!ff6_scene_clip(full, viewport, 480, 320, NULL));
  assert(!ff6_scene_clip(full, viewport, 0, 320, output));
  puts("scene scissor mapping tests passed");
  return 0;
}
