#ifndef __BRIDGE_H__
#define __BRIDGE_H__

#include <stddef.h>
#include <stdint.h>

typedef struct {
  unsigned char *elements;
  int size;
} jni_bytearray;

typedef struct {
  int *elements;
  int size;
} jni_intarray;

/* ---------------------------------------------------------------------------
 * Text rasterizer
 *
 * libFF6.so drives FF6.java's Paint/Canvas pair through this exact sequence
 * (see createTextTexture @ 0x1e54dc):
 *
 *   setTextColor(argb); setTextFontSize(px); setTextTypeface(style);
 *   setTextUTF8(bytes);
 *   w = getTextWidth(); h = getTextHeight();
 *   img = getTextImg();
 *
 * On Android the backing store is a 1024x640 ARGB_8888 Bitmap, so the returned
 * int[] has a *fixed* row stride of TEXT_IMG_W pixels regardless of w. The .so
 * only consumes the alpha channel (it ORs in its own colour), so we only have
 * to get coverage right.
 * ------------------------------------------------------------------------- */

#define TEXT_IMG_W 1024
#define TEXT_IMG_H 640

void setTextColor(int argb);
void setTextFontSize(int size);
void setTextTypeface(int style);
void setTextUTF8(const unsigned char *utf8, int length);
int getTextWidth(void);
int getTextHeight(void);
jni_intarray *getTextImg(void);
int getTextFontSize(int size);

/* --------------------------------------------------------------------------
 * Environment queries
 * ------------------------------------------------------------------------ */

int getSystemLanguage(void);
const char *getDataDirectory(void);
const char *getObbDirectory(void);
int64_t getHeapSize(void);
int getBatteryInfo(void);
int getClockTime(void);
int isAndroidTV(void);
void getTerminalTimeFormat(jni_bytearray *out, int64_t millis);

/* --------------------------------------------------------------------------
 * Software keyboard (rename character / passcode entry)
 * ------------------------------------------------------------------------ */

void openInputDialog(int multiline);
const char *getInputText(void);
void setInputText(const char *text);
void setLimitInputText(int limit);
void setClipBoardCopy(const char *text);
int isInputDialogOpen(void);

void trace(const char *fmt, ...);
int debugTextRegionNonzero(void);

#endif
