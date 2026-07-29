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

/* How textures are sampled.
 *   FILTER_NEAREST  every texture GL_NEAREST. Crisp, but the engine's 1.7x
 *                   scale to the pillarbox lands source pixels on one or two
 *                   screen pixels unevenly, which shows up as thin seams
 *                   along tile edges.
 *   FILTER_LINEAR   every texture GL_LINEAR, i.e. what the Android build asks
 *                   for. Smooth but blurs the art before it is scaled, and
 *                   only softens those seams rather than removing them.
 *
 * Drawing the world offscreen at a whole multiple and filtering once at the
 * end would fix the seams properly, but the attempt at it rendered nothing
 * on hardware and was removed rather than shipped broken -- see HUNT.md.
 */
#define FILTER_NEAREST 0
#define FILTER_LINEAR 1

typedef struct {
  int bilinear; /* one of FILTER_* above */
  int lang;     /* 0 = system, otherwise getSystemLanguage() value + 1 */
  int msaa;     /* 0 = off, 1 = 2x, 2 = 4x */
  int sound;    /* 0 disables the OpenSL ES engine entirely */
  int font_scale;    /* glyph size as a percentage. 100 keeps (ascent -
                      * descent) at the size the engine asks for, which is
                      * what matches a capture of the real game; 136 is what
                      * mapping the EM square gives, i.e. Android's own
                      * metrics, and runs into the line below. */
  int offscreen;     /* 1 = draw the world into a buffer at its own 480x320
                      * and stretch that once to the pillarbox. Sampling stays
                      * 1:1 so tile seams cannot form, and it rasterises fewer
                      * fragments than stretching every draw. */
  int integer_scale; /* 1 = snap the game area to a whole multiple of the
                      * engine's own 480x320 composition space instead of
                      * stretching it to the 816x544 pillarbox. Only whole
                      * multiples keep source pixels the same size on screen,
                      * which is what GL_NEAREST needs to look even. */
} config_opts;

extern config_opts options;

#endif
