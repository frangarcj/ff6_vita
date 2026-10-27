#ifndef FF6_OPTIONS_H
#define FF6_OPTIONS_H

#include "config.h"
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* VitaSDK's strict C++ mode hides these POSIX declarations, but newlib exports
 * both functions. Keep their native C linkage (the Android shims are unrelated). */
#if defined(__NEWLIB__) && defined(__cplusplus)
extern "C" int mkstemp(char *);
extern "C" FILE *fdopen(int, const char *);
#endif

typedef struct {
  const char *key;
  size_t offset;
  int minimum, maximum, fallback;
} ff6_option;

#define FF6_OPTION(key, field, lo, hi, def) \
  {key, offsetof(config_opts, field), lo, hi, def}
static const ff6_option ff6_options[] = {
  FF6_OPTION("resolution", res, 0, 2, 0),
  FF6_OPTION("postfx", postfx, 0, 7, 0),
  FF6_OPTION("swap_buttons", swap_buttons, 0, 1, 0),
  FF6_OPTION("aspect_ratio", aspect_ratio, 0, 4, 0),
  FF6_OPTION("bilinear", bilinear, 0, 1, 0),
  FF6_OPTION("language", lang, 0, 13, 0),
  FF6_OPTION("antialiasing", msaa, 0, 2, 2),
  FF6_OPTION("sound", sound, 0, 1, 1),
  FF6_OPTION("fontscale", font_scale, 25, 400, 100),
  FF6_OPTION("offscreen", offscreen, 0, 1, 0),
  FF6_OPTION("sharpbilinear", sharp_bilinear, 0, 1, 1),
  FF6_OPTION("integerscale", integer_scale, 0, 1, 0),
  FF6_OPTION("fixedcolor", fixed_color_native, 0, 1, 0),
  FF6_OPTION("inplacetex", inplace_tex, 0, 1, 1),
};
#undef FF6_OPTION
#define FF6_OPTION_COUNT (sizeof(ff6_options) / sizeof(ff6_options[0]))

static inline int *ff6_option_value(config_opts *opts, size_t index) {
  return (int *)((unsigned char *)opts + ff6_options[index].offset);
}

static inline void ff6_default_options(config_opts *opts) {
  memset(opts, 0, sizeof(*opts));
  for (size_t i = 0; i < FF6_OPTION_COUNT; ++i)
    *ff6_option_value(opts, i) = ff6_options[i].fallback;
}

static inline void ff6_validate_options(config_opts *opts) {
  for (size_t i = 0; i < FF6_OPTION_COUNT; ++i) {
    int *value = ff6_option_value(opts, i);
    if (*value < ff6_options[i].minimum || *value > ff6_options[i].maximum)
      *value = ff6_options[i].fallback;
  }
  if (opts->lang == 3) opts->lang = 0; /* Native FF6 language ID 2 is unused. */
}

/* Locate a whole key, not a prefix. Comments and unknown settings survive saves. */
static inline int ff6_option_index(const char *line, const char **value) {
  while (isspace((unsigned char)*line)) ++line;
  if (!*line || *line == '#' || *line == ';') return -1;
  const char *equals = strchr(line, '=');
  if (!equals) return -1;
  const char *end = equals;
  while (end > line && isspace((unsigned char)end[-1])) --end;
  for (size_t i = 0; i < FF6_OPTION_COUNT; ++i) {
    if ((size_t)(end - line) == strlen(ff6_options[i].key) &&
        !strncmp(line, ff6_options[i].key, (size_t)(end - line))) {
      if (value) *value = equals + 1;
      return (int)i;
    }
  }
  return -1;
}

/* 1 = read, 0 = absent/defaults, -1 = I/O error. No game data is required. */
static inline int ff6_load_options(const char *path, config_opts *opts) {
  ff6_default_options(opts);
  FILE *file = fopen(path, "rb");
  if (!file) return errno == ENOENT ? 0 : -1;
  char line[512];
  while (fgets(line, sizeof(line), file)) {
    if (!strchr(line, '\n') && !feof(file)) {
      int c;
      while ((c = fgetc(file)) != '\n' && c != EOF) {}
      continue; /* Never parse a truncated value. */
    }
    const char *value;
    int i = ff6_option_index(line, &value);
    if (i < 0) continue;
    errno = 0;
    char *end;
    long number = strtol(value, &end, 10);
    if (value == end || errno || number < INT_MIN || number > INT_MAX) continue;
    while (isspace((unsigned char)*end)) ++end;
    if (*end && *end != '#' && *end != ';') continue;
    *ff6_option_value(opts, (size_t)i) = (int)number;
  }
  int failed = ferror(file);
  if (fclose(file)) failed = 1;
  ff6_validate_options(opts);
  return failed ? -1 : 1;
}

/* Write beside the original, then rename only after a successful close.
 * Preserve comments/unknown keys; failure never truncates the previous config. */
static inline int ff6_save_options(const char *path, const config_opts *input) {
  config_opts opts = *input;
  ff6_validate_options(&opts);
  char temporary[512];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path) >=
      (int)sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
  FILE *old = fopen(path, "rb");
  if (!old && errno != ENOENT) return -1;
  int fd = mkstemp(temporary);
  if (fd < 0) { if (old) fclose(old); return -1; }
  FILE *out = fdopen(fd, "wb");
  if (!out) { close(fd); unlink(temporary); if (old) fclose(old); return -1; }
  int failed = 0;
  size_t total = 0;
  char line[512];
  if (old) {
    while (fgets(line, sizeof(line), old)) {
      total += strlen(line);
      if (total > 65536 || (!strchr(line, '\n') && !feof(old))) {
        failed = 1; break;
      }
      if (ff6_option_index(line, NULL) < 0 && fputs(line, out) == EOF) failed = 1;
    }
    if (ferror(old)) failed = 1;
    if (fclose(old)) failed = 1;
  }
  if (fputc('\n', out) == EOF) failed = 1;
  for (size_t i = 0; i < FF6_OPTION_COUNT; ++i)
    if (fprintf(out, "%s=%d\n", ff6_options[i].key,
                *ff6_option_value(&opts, i)) < 0) failed = 1;
  if (fclose(out)) failed = 1;
  if (!failed && rename(temporary, path) == 0) return 0;
  unlink(temporary);
  return -1;
}

static inline int ff6_button_key(int key, int swap) {
  return swap && (key == 96 || key == 97) ? (193 - key) : key;
}

static inline void ff6_output_size(int res, int *width, int *height) {
  static const int sizes[3][2] = {{960, 544}, {1280, 725}, {1920, 1088}};
  if (res < 0 || res > 2) res = 0;
  *width = sizes[res][0]; *height = sizes[res][1];
}
#endif
