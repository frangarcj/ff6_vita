#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../loader/options.h"
#include "../loader/display_layout.h"

static void write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb"); assert(f);
  assert(fputs(text, f) >= 0); assert(!fclose(f));
}
int main(void) {
  char dir[] = "/tmp/ff6-options-XXXXXX";
  assert(mkdtemp(dir));
  char path[256]; snprintf(path, sizeof(path), "%s/options.cfg", dir);
  config_opts a, b;
  assert(ff6_load_options(path, &a) == 0);
  assert(a.res == 0 && a.postfx == 0 && a.msaa == 2 && a.font_scale == 100);
  assert(a.sound && a.inplace_tex && !a.swap_buttons && !a.aspect_ratio);
  assert(a.sharp_bilinear == 1);
  write_text(path, "# my settings\r\nnew_future_key=87\nresolution=2\npostfx=7\n"
    "language=7\nswap_buttons=1\nfontscale=69\nsharpbilinear=1\n"
    "antialiasing=-1\nsound=999999999999999999999999999999\n"
    "bilinear=1bad\n postfx = 3 # comment\n");
  assert(ff6_load_options(path, &a) == 1);
  assert(a.res == 2 && a.postfx == 3 && a.lang == 7 && a.swap_buttons == 1);
  assert(a.msaa == 2 && a.sound == 1 && a.bilinear == 0 && a.font_scale == 69);
  a.aspect_ratio = 4;
  assert(!ff6_save_options(path, &a));
  assert(ff6_load_options(path, &b) == 1 && !memcmp(&a, &b, sizeof(a)));
  FILE *f = fopen(path, "rb"); assert(f);
  char text[2048] = {0}; assert(fread(text, 1, sizeof(text) - 1, f) > 0); fclose(f);
  assert(strstr(text, "# my settings") && strstr(text, "new_future_key=87"));
  assert(strstr(text, "postfx=3") && !strstr(text, "postfx=7"));
  assert(ff6_button_key(96, 1) == 97 && ff6_button_key(97, 1) == 96);
  assert(ff6_button_key(99, 1) == 99 && ff6_button_key(96, 0) == 96);
  for (int key = 0; key < 150; ++key)
    assert(ff6_button_key(ff6_button_key(key, 1), 1) == key);
  int w, h; ff6_output_size(2, &w, &h); assert(w == 1920 && h == 1088);
  ff6_output_size(-1, &w, &h); assert(w == 960 && h == 544);
  /* Refuse malformed huge files rather than destroying unknown settings. */
  char long_line[700]; memset(long_line, 'X', sizeof(long_line));
  long_line[sizeof(long_line) - 1] = 0; write_text(path, long_line);
  assert(ff6_save_options(path, &a) == -1);
  f = fopen(path, "rb"); assert(f); assert(fgetc(f) == 'X'); fclose(f);
  assert(!unlink(path)); assert(!rmdir(dir));

  int native[4] = {72, 0, 816, 544}, result[4];
  ff6_display_rect(native, 960, 544, 0, 0, 480, 320, result);
  assert(!memcmp(native, result, sizeof(native)));
  ff6_display_rect(native, 960, 544, 1, 0, 480, 320, result);
  assert(result[0] == 117 && result[2] == 725 && result[3] == 544);
  ff6_display_rect(native, 960, 544, 2, 0, 480, 320, result);
  assert(result[0] == 72 && result[1] == 42 && result[2] == 816 && result[3] == 459);
  float x = 480, y = 272;
  ff6_display_touch(native, result, 544, &x, &y);
  assert(x == 480 && y > 271 && y < 273);
  x = 72; y = 43; ff6_display_touch(native, result, 544, &x, &y);
  assert(x == 72 && y == 0);
  ff6_display_rect(native, 960, 544, 4, 1, 480, 320, result);
  assert(result[0] == 240 && result[1] == 112 && result[2] == 480 && result[3] == 320);
  x = 100; y = 272; ff6_display_touch(native, result, 544, &x, &y);
  assert(x == -1 && y == -1);
  x = 30; y = 272; ff6_display_touch(native, result, 544, &x, &y);
  assert(x == 30 && y == 272);
  for (int aspect = 0; aspect <= 4; ++aspect) {
    ff6_display_rect(native, 960, 544, aspect, 0, 480, 320, result);
    assert(result[0] >= 0 && result[1] >= 0);
    assert(result[0] + result[2] <= 960 && result[1] + result[3] <= 544);
    assert(result[0] >= native[0] && result[0] + result[2] <= native[0] + native[2]);
  }
  puts("options: safe defaults, strict parsing, atomic round trip, preservation, buttons and layout passed");
  return 0;
}
