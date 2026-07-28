#ifndef __CONFIG_H__
#define __CONFIG_H__

/* libFF6.so is ~22MB of text plus ~12MB of data/bss, so it needs a large
 * contiguous block well away from the newlib heap. */
#define LOAD_ADDRESS 0x98000000

#define MEMORY_NEWLIB_MB 192
#define MEMORY_VITAGL_THRESHOLD_MB 8

#define DATA_PATH "ux0:data/ff6"
#define SO_PATH DATA_PATH "/" "libFF6.so"
#define OBB_PATH DATA_PATH "/" "main.obb"
#define SAVE_PATH DATA_PATH
#define CONFIG_FILE_PATH DATA_PATH "/" "options.cfg"

#define DEF_SCREEN_W 960
#define DEF_SCREEN_H 544

typedef struct {
  int bilinear; /* force GL_LINEAR filtering on every texture */
  int lang;     /* 0 = system, otherwise getSystemLanguage() value + 1 */
  int msaa;     /* 0 = off, 1 = 2x, 2 = 4x */
  int aspect;   /* 0 = pillarboxed 3:2, 1 = stretch to fullscreen */
} config_opts;

extern config_opts options;

#endif
