/* bridge.c -- Java-side services libFF6.so expects from FF6.java
 *
 * Copyright (C) 2021 Francisco José García García
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "bridge.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <vitasdk.h>

#include "stb_truetype.h"

#include "config.h"
#include "dialog.h"
#include "text_encoding.h"
#include "font_raster.h"

#include <stdarg.h>

#ifdef FF6_DEBUG
/* Boot tracing: the emulator buffers tty output, so a host-side crash loses it.
 * Writing through sceIo and closing every time survives anything. */
void trace(const char *fmt, ...) {
  char line[256];
  va_list list;

  va_start(list, fmt);
  int len = vsnprintf(line, sizeof(line), fmt, list);
  va_end(list);

  if (len < 0)
    return;
  if (len > (int)sizeof(line) - 1)
    len = sizeof(line) - 1;

  char tagged[288];
  int tlen = snprintf(tagged, sizeof(tagged), "[t%08x] %s",
                      (unsigned)sceKernelGetThreadId(), line);
  if (tlen < 0)
    return;
  if (tlen > (int)sizeof(tagged) - 1)
    tlen = sizeof(tagged) - 1;

  sceClibPrintf("%s", tagged);

  /* Collapse runs of the same message: the game emits one libpng warning per
     decoded image and they would otherwise bury everything else. */
  static char last[288];
  static int repeats;

  if (strcmp(tagged, last) == 0) {
    repeats++;
    return;
  }

  static SceUID fd = -1;
  if (fd < 0) {
    fd = sceIoOpen(DATA_PATH "/boot.log",
                   SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
    if (fd < 0)
      return;
  }

  if (repeats) {
    char summary[64];
    int slen = snprintf(summary, sizeof(summary), "  ... x%d more\n", repeats);
    sceIoWrite(fd, summary, slen);
    repeats = 0;
  }

  strncpy(last, tagged, sizeof(last) - 1);
  last[sizeof(last) - 1] = '\0';
  sceIoWrite(fd, tagged, tlen);
}
#endif /* FF6_DEBUG */

/* ==========================================================================
 * Font handling
 * ======================================================================== */

static stbtt_fontinfo *font_info = NULL;
static unsigned char *font_buffer = NULL;
static int font_language = -1;

static const char *font_path_for_language(int language) {
  switch (language) {
  case 0: /* Japanese */
    return "app0:/NotoSansJP-Regular.ttf";
  case 7: /* Simplified Chinese */
  case 8: /* Traditional Chinese */
    return "app0:/NotoSansSC-Regular.ttf";
  case 9: /* Korean */
    return "app0:/NotoSansKR-Regular.ttf";
  default:
    /* Noto Sans JP carries full Latin/Cyrillic coverage, so it doubles as the
     * western font and saves us from shipping a fourth file. */
    return "app0:/NotoSansJP-Regular.ttf";
  }
}

static void unload_font(void) {
  if (font_info) {
    free(font_info);
    font_info = NULL;
  }
  if (font_buffer) {
    free(font_buffer);
    font_buffer = NULL;
  }
}

static int init_font(void) {
  int language = getSystemLanguage();

  if (font_info && language == font_language)
    return 1;

  unload_font();
  font_language = language;

  const char *path = font_path_for_language(language);
  trace("[font] lang=%d opening %s\n", language, path);

  FILE *f = fopen(path, "rb");
  if (!f) {
    trace("[font] fopen failed\n");
    return 0;
  }

  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (size <= 0) {
    fclose(f);
    return 0;
  }

  trace("[font] size=%ld, allocating\n", size);
  font_buffer = malloc(size);
  if (!font_buffer) {
    trace("[font] malloc failed\n");
    fclose(f);
    return 0;
  }

  if (fread(font_buffer, size, 1, f) != 1) {
    fclose(f);
    unload_font();
    return 0;
  }
  fclose(f);

  font_info = malloc(sizeof(stbtt_fontinfo));
  if (!font_info) {
    unload_font();
    return 0;
  }

  trace("[font] read ok, stbtt_InitFont...\n");
  if (!stbtt_InitFont(font_info, font_buffer, 0)) {
    trace("[font] stbtt_InitFont failed\n");
    unload_font();
    return 0;
  }

  trace("[font] ready\n");
  return 1;
}

/* ==========================================================================
 * UTF-8 iteration
 * ======================================================================== */

static const char *utf8_next(const char *s, const char *end, int *codepoint) {
  unsigned char c = (unsigned char)*s;

  if (c < 0x80) {
    *codepoint = c;
    return s + 1;
  }
  if ((c & 0xE0) == 0xC0 && s + 1 < end) {
    *codepoint = ((c & 0x1F) << 6) | (s[1] & 0x3F);
    return s + 2;
  }
  if ((c & 0xF0) == 0xE0 && s + 2 < end) {
    *codepoint = ((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F);
    return s + 3;
  }
  if ((c & 0xF8) == 0xF0 && s + 3 < end) {
    *codepoint = ((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
                 ((s[2] & 0x3F) << 6) | (s[3] & 0x3F);
    return s + 4;
  }

  /* Malformed byte: consume it so we always make progress. */
  *codepoint = 0xFFFD;
  return s + 1;
}

/* ==========================================================================
 * Paint state — mirrors the static Paint in FF6.java
 * ======================================================================== */

#define TEXT_MAX_LEN 1024

static int paint_color = 0xFFFFFFFF;
static int paint_size = 16;
static int paint_style = 0;

static char paint_text[TEXT_MAX_LEN];
static int paint_text_len = 0;

static jni_intarray text_image = {NULL, 0};
static int text_dirty = 1;
/* Region touched by the previous rasterization, so we only clear what we used
 * instead of memsetting 2.6 MB per string. */
static int text_clear_w = 0;
static int text_clear_h = 0;

static float paint_scale = 0.0f;
static int paint_ascent = 0;
static int paint_descent = 0;
static int paint_baseline = 0;

static void update_metrics(void) {
  if (!init_font()) {
    paint_scale = 0.0f;
    paint_ascent = 0;
    paint_descent = 0;
    paint_baseline = 0;
    return;
  }

  ff6_font_metrics metrics = ff6_measure_font(
      font_info, paint_size, options.font_scale, TEXT_IMG_H);
  paint_scale = metrics.scale;
  paint_ascent = metrics.ascent;
  paint_descent = metrics.descent;
  paint_baseline = metrics.baseline;
}

void setTextColor(int argb) {
  static int logged;
  if (logged < 8) {
    logged++;
    trace("[txt] setTextColor(0x%08x)\n", argb);
  }
  if (argb == paint_color)
    return;

  paint_color = argb;
  /* rasterize_text() bakes this into the bitmap, so a colour change has to
   * invalidate the cached rasterization the same way a text change does. */
  text_dirty = 1;
}

void setTextFontSize(int size) {
  static int logged;
  if (logged < 8) {
    logged++;
    trace("[txt] setTextFontSize(%d)\n", size);
  }
  if (size <= 0)
    size = 1;
  if (size == paint_size)
    return;

  paint_size = size;
  update_metrics();
  text_dirty = 1;
}

void setTextTypeface(int style) {
  if (style == paint_style)
    return;

  paint_style = style;
  text_dirty = 1;
}

void setTextUTF8(const unsigned char *utf8, int length) {
  if (length < 0)
    length = 0;
  if (length > TEXT_MAX_LEN - 1)
    length = TEXT_MAX_LEN - 1;

  memcpy(paint_text, utf8, length);
  paint_text[length] = '\0';
  paint_text_len = length;
  text_dirty = 1;

  {
    static int logged;
    if (logged < 120) {
      logged++;
      trace("[seq] setTextUTF8('%.24s' len=%d)\n", paint_text, length);
    }
  }
}

int getTextHeight(void) {
  if (paint_scale == 0.0f)
    update_metrics();

  int height = paint_ascent + paint_descent;
  if (height > TEXT_IMG_H)
    height = TEXT_IMG_H;
  return height;
}

int getTextWidth(void) {
  if (paint_scale == 0.0f)
    update_metrics();
  if (!font_info || paint_text_len == 0)
    return 0;

  const char *p = paint_text;
  const char *end = paint_text + paint_text_len;
  float width = 0.0f;
  int previous = 0;

  while (p < end) {
    int codepoint;
    p = utf8_next(p, end, &codepoint);

    int advance, left_bearing;
    stbtt_GetCodepointHMetrics(font_info, codepoint, &advance, &left_bearing);
    width += advance * paint_scale;

    if (previous)
      width += stbtt_GetCodepointKernAdvance(font_info, previous, codepoint) *
               paint_scale;
    previous = codepoint;
  }

  /* Bold is faked by smearing one pixel to the right, so it is one pixel wider
   * than the plain run. */
  if (paint_style & 1)
    width += 1.0f;

  int result = (int)(width + 0.5f);
  if (result > TEXT_IMG_W)
    result = TEXT_IMG_W;
  return result;
}

static void rasterize_text(void) {
  int width = getTextWidth();
  int height = getTextHeight();

  /* TextGlyphTexture::drawGlyph reads this array with a hard-coded stride of
   * 1024 pixels per row (verified in the disassembly: `add ip, ip, #4096` per
   * row), so glyphs must be laid out at TEXT_IMG_W stride starting at (0,0).
   * Clear only the rows the previous string dirtied. */
  if (text_clear_h > 0) {
    int rows = text_clear_h > TEXT_IMG_H ? TEXT_IMG_H : text_clear_h;
    int span = text_clear_w > TEXT_IMG_W ? TEXT_IMG_W : text_clear_w;
    for (int y = 0; y < rows; y++)
      memset(&text_image.elements[y * TEXT_IMG_W], 0, span * sizeof(int));
  }
  text_clear_w = width + 2;
  text_clear_h = height;

  if (!font_info || paint_text_len == 0 || width <= 0 || height <= 0)
    return;

  /* Rasterize into an 8bpp scratch buffer first: stb_truetype composites
   * coverage, and doing that straight into the int buffer would need a
   * read-modify-write per pixel per glyph. */
  unsigned char *coverage = calloc((size_t)TEXT_IMG_W * height, 1);
  if (!coverage)
    return;

  const char *p = paint_text;
  const char *end = paint_text + paint_text_len;
  float pen_x = 0.0f;
  int previous = 0;

  while (p < end) {
    int codepoint;
    p = utf8_next(p, end, &codepoint);

    if (previous)
      pen_x += stbtt_GetCodepointKernAdvance(font_info, previous, codepoint) *
               paint_scale;
    previous = codepoint;

    int advance, left_bearing;
    stbtt_GetCodepointHMetrics(font_info, codepoint, &advance, &left_bearing);

    if (!ff6_raster_glyph(font_info, codepoint, paint_scale,
                          (int)floorf(pen_x + 0.5f), paint_baseline, coverage,
                          TEXT_IMG_W, height, TEXT_IMG_W))
      trace("[font] unable to rasterize U+%04x\n", codepoint);

    pen_x += advance * paint_scale;
  }

  /* Faux bold: OR the run one pixel to the right. */
  if (paint_style & 1) {
    for (int y = 0; y < height; y++) {
      unsigned char *row = coverage + y * TEXT_IMG_W;
      for (int x = TEXT_IMG_W - 1; x > 0; x--) {
        if (row[x - 1] > row[x])
          row[x] = row[x - 1];
      }
    }
  }

  /* Colour has to come from us. The engine hands the bitmap straight to
   * TextGlyphTexture::drawGlyph without touching it, so coverage alone leaves
   * the atlas texels at RGB 0 -- and the glyph draws MODULATE against the
   * current colour, which multiplies black by anything and stays black. That
   * is why every atlas-drawn string (the copyright line, menus) was invisible
   * while the AGB tile text was fine.
   *
   * The buffer is Android ARGB_8888, whose bytes are R,G,B,A in memory, so the
   * int is (A<<24)|(B<<16)|(G<<8)|R -- red and blue swapped relative to the
   * 0xAARRGGBB colour the engine passes to setTextColor. */
  unsigned int r = ((unsigned int)paint_color >> 16) & 0xFF;
  unsigned int g = ((unsigned int)paint_color >> 8) & 0xFF;
  unsigned int b = (unsigned int)paint_color & 0xFF;
  unsigned int alpha = ((unsigned int)paint_color >> 24) & 0xFF;
  unsigned int rgb = (b << 16) | (g << 8) | r;

  for (int y = 0; y < height; y++) {
    const unsigned char *src = coverage + y * TEXT_IMG_W;
    int *dst = &text_image.elements[y * TEXT_IMG_W];
    for (int x = 0; x < width; x++) {
      unsigned int a = src[x];
      if (alpha != 0xFF)
        a = (a * alpha + 127) / 255;
      dst[x] = (int)((a << 24) | rgb);
    }
  }

  free(coverage);
}

jni_intarray *getTextImg(void) {
  if (!text_image.elements) {
    text_image.size = TEXT_IMG_W * TEXT_IMG_H;
    text_image.elements = calloc(text_image.size, sizeof(int));
    if (!text_image.elements) {
      text_image.size = 0;
      return &text_image;
    }
  }

  if (text_dirty) {
    rasterize_text();
    text_dirty = 0;
  }

#ifdef FF6_DEBUG
  /* DEBUG: prove whether the glyphs we rasterize actually carry coverage, and
   * for which string, so an empty upload can be blamed on the right side of
   * the JNI boundary. */
  {
    static int logged;
    if (logged < 120) {
      logged++;
      int w = getTextWidth();
      int h = getTextHeight();
      int nonzero = 0;
      for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
          if ((unsigned int)text_image.elements[y * TEXT_IMG_W + x] >> 24)
            nonzero++;
      trace("[seq] getTextImg '%.24s' w=%d h=%d nonzero=%d\n", paint_text, w, h,
            nonzero);
    }
  }
#endif

  return &text_image;
}

#ifdef FF6_DEBUG
/* Nonzero count over the glyph-cell region at the exact moment the engine
 * takes the pixel pointer; a zero here with ink at getTextImg time means
 * something cleared the buffer in between. */
int debugTextRegionNonzero(void) {
  if (!text_image.elements)
    return -1;
  int nonzero = 0;
  for (int y = 0; y < 48; y++)
    for (int x = 0; x < 48; x++)
      if ((unsigned int)text_image.elements[y * TEXT_IMG_W + x] >> 24)
        nonzero++;
  return nonzero;
}
#endif

int getTextFontSize(int size) {
  /* FF6.java sets the size and hands back the resulting line height. The
   * glyph atlas geometry hangs off this value: TextGlyphTexture uses it as
   * the atlas cell size and derives its glyph capacity as (1024/cell)^2, so
   * returning 0 here permanently disables glyph registration. */
  setTextFontSize(size);
  int height = getTextHeight();
  trace("[txt] getTextFontSize(%d) -> %d\n", size, height);
  return height;
}

/* ==========================================================================
 * Environment queries
 * ======================================================================== */

int getSystemLanguage(void) {
  /* Values come straight from FF6.getSystemLanguage(): 0 ja, 1 en, 3 de,
   * 4 fr, 5 it, 6 es, 7 zh_CN, 8 zh_TW, 9 ko, 10 th, 11 ru, 12 pt_BR. */
  if (options.lang)
    return options.lang - 1;

  int language = -1;
  sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &language);

  switch (language) {
  case SCE_SYSTEM_PARAM_LANG_JAPANESE:
    return 0;
  case SCE_SYSTEM_PARAM_LANG_GERMAN:
    return 3;
  case SCE_SYSTEM_PARAM_LANG_FRENCH:
    return 4;
  case SCE_SYSTEM_PARAM_LANG_ITALIAN:
    return 5;
  case SCE_SYSTEM_PARAM_LANG_SPANISH:
    return 6;
  case SCE_SYSTEM_PARAM_LANG_CHINESE_S:
    return 7;
  case SCE_SYSTEM_PARAM_LANG_CHINESE_T:
    return 8;
  case SCE_SYSTEM_PARAM_LANG_KOREAN:
    return 9;
  case SCE_SYSTEM_PARAM_LANG_RUSSIAN:
    return 11;
  case SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR:
    return 12;
  default:
    return 1;
  }
}

const char *getDataDirectory(void) {
  /* Trailing slash matters: the .so appends "RockDat%d.sav" straight onto this
     with no separator (its built-in fallback is the literal "/sdcard/"). */
  return SAVE_PATH "/";
}

const char *getObbDirectory(void) {
  /* Java returns the full path of the .obb file, not the directory holding it
   * -- Rock::ObbFile strcpy()s the result straight into msObbFilePath. */
  return OBB_PATH;
}

int64_t getHeapSize(void) {
  /* Only used to pick cache sizes; report the free newlib heap. */
  SceKernelFreeMemorySizeInfo info;
  info.size = sizeof(info);
  if (sceKernelGetFreeMemorySize(&info) >= 0)
    return (int64_t)info.size_user;

  return (int64_t)64 * 1024 * 1024;
}

/* Percentage in 0..100, offset by 1000 while charging and 2000 when full --
 * the game draws its iPhone-style status bar straight from this. */
int getBatteryInfo(void) {
  int percent = scePowerGetBatteryLifePercent();
  if (percent < 0)
    percent = 100;

  if (percent > 100)
    percent = 100;
  if (percent < 0)
    percent = 0;

  if (percent == 100 && scePowerIsBatteryCharging())
    return percent + 2000;
  if (scePowerIsBatteryCharging())
    return percent + 1000;

  return percent;
}

/* hour * 100 + minute, local time. */
int getClockTime(void) {
  SceDateTime time;

  if (sceRtcGetCurrentClockLocalTime(&time) < 0)
    return 0;

  return time.hour * 100 + time.minute;
}

/* Tempting to answer yes -- a Vita has a real d-pad, and that is what FF3 does
 * -- but the .so's TV branch calls further into the activity and crashes here,
 * so report a phone. Our synthesized key events reach InputDriverAndroid
 * either way; the virtual pad is dealt with separately. */
int isAndroidTV(void) { return 0; }

void getTerminalTimeFormat(jni_bytearray *out, int64_t millis) {
  if (!out || !out->elements || out->size <= 0)
    return;

  time_t seconds = (time_t)(millis / 1000);
  struct tm *t = gmtime(&seconds);

  char buffer[64];
  if (t)
    strftime(buffer, sizeof(buffer), "%d/%m/%Y", t);
  else
    buffer[0] = '\0';

  int length = (int)strlen(buffer);
  if (length > out->size)
    length = out->size;

  memcpy(out->elements, buffer, length);
  if (length < out->size)
    memset(out->elements + length, 0, out->size - length);
}

/* ==========================================================================
 * Software keyboard
 * ======================================================================== */

static int ime_open = 0;
static int ime_limit = 99;
static char ime_text[256] = {0};
static char ime_result[256] = {0};

void openInputDialog(int multiline) {
  (void)multiline;

  if (ime_open)
    return;

  if (init_ime_dialog("", ime_text, ime_limit) >= 0)
    ime_open = 1;
}

const char *getInputText(void) {
  if (ime_open) {
    char *result = get_ime_dialog_result();
    if (result) {
      /* Android and the Vita IME count UTF-16 units, not UTF-8 bytes. */
      ff6_copy_utf8(ime_result, sizeof(ime_result), result,
                    ime_limit > 0 ? (size_t)ime_limit : (size_t)-1);
      ime_open = 0;
    }
  }

  return ime_result;
}

void setInputText(const char *text) {
  if (!text) {
    ime_text[0] = '\0';
    return;
  }

  ff6_copy_utf8(ime_text, sizeof(ime_text), text, (size_t)-1);
}

void setLimitInputText(int limit) { ime_limit = limit; }

void setClipBoardCopy(const char *text) { (void)text; }

int isInputDialogOpen(void) { return ime_open; }
