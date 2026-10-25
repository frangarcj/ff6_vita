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

/* Private engine layouts and hooks below target the ARMv7 library from the
 * original Android 2.1.6 release. Check it before parsing or patching it. */
#define FF6_ANDROID_VERSION "2.1.6"
#define FF6_SO_SIZE 22340224u
#define FF6_SO_CRC32 0x1441da16u

#define DEF_SCREEN_W 960
#define DEF_SCREEN_H 544

/* How textures are sampled.
 *   FILTER_NEAREST  GL_NEAREST except for registered glyph atlases, whose
 *                   antialiased coverage keeps GL_LINEAR. The engine's 1.7x
 *                   scale still gives uneven pixel widths; atlas seams need
 *                   separate sampling/precision validation.
 *   FILTER_LINEAR   every texture GL_LINEAR, i.e. what the Android build asks
 *                   for. Smooth but blurs the art before it is scaled, and
 *                   only softens those seams rather than removing them.
 *
 * The optional offscreen path is experimental and stays disabled by default;
 * its render-target and state handling still need hardware validation.
 */
#define FILTER_NEAREST 0
#define FILTER_LINEAR 1

typedef struct {
  int res;      /* 0 = 544p, 1 = 720p, 2 = 1080i; VitaGL can fall back to 544p. */
  int postfx;   /* 0 = none, 1..7 = packaged full-screen effects. */
  int swap_buttons; /* Swap Android confirm/cancel key events, including release. */
  int aspect_ratio; /* 0 = native 3:2; 1..4 stretch presentation, never change FOV. */
  int bilinear; /* one of FILTER_* above */
  int lang;     /* 0 = system, otherwise getSystemLanguage() value + 1 */
  int msaa;     /* 0 = off, 1 = 2x, 2 = 4x */
  int sound;    /* 0 disables the OpenSL ES engine entirely */
  int font_scale;    /* percentage of the EM size requested by Android Paint.
                      * 100 preserves that size; the substituted font has its
                      * own ascent/descent and still needs visual validation. */
  int offscreen;     /* 1 = draw the world into a buffer at its own 480x320
                      * and stretch that once to the pillarbox. Experimental:
                      * UI clipping and scene transitions need validation. */
  int sharp_bilinear; /* 1 = enable the world buffer and the sharp-bilinear
                       * final pass; independent of atlas texture filtering. */
  int integer_scale; /* 1 = snap the game area to a whole multiple of the
                      * engine's own 480x320 composition space instead of
                      * stretching it to the 816x544 pillarbox. Only whole
                      * multiples keep source pixels the same size on screen,
                      * which is what GL_NEAREST needs to look even. */
  int fixed_color_native; /* 1 = pass GL_FIXED colour arrays straight to the
                           * vitaGL fork, which unpacks 16.16 in its generated
                           * vertex shader; 0 = expand to floats in the loader
                           * at draw time (works on stock vitaGL too). */
  int inplace_tex;   /* 1 = vglInPlaceTexSubImage: the AGB compositor streams
                      * tile strips into two 1024x2048 RGBA atlases while they
                      * are being drawn, and vitaGL's default response is to
                      * orphan the texture -- an 8 MB uncached full-surface
                      * copy, ~5 MB/frame while the town scrolls. Writing in
                      * place skips that; the strips entering view are not
                      * sampled by the frames still in flight. */
} config_opts;

extern config_opts options;

#endif
