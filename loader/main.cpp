/* main.cpp -- Final Fantasy VI .so loader
 *
 * Copyright (C) 2021 Andy Nguyen
 * Copyright (C) 2021 Francisco José García García
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 *
 * ---------------------------------------------------------------------------
 * libFF6.so is a stock Android NativeActivity built on android_native_app_glue:
 * ANativeActivity_onCreate() spins up a thread running android_main(), which
 * pumps an ALooper and, on APP_CMD_INIT_WINDOW, calls initGame(). initGame()
 * in turn spawns the real game thread (thread_main -> initDisplay -> AgbMain).
 *
 * Rather than emulating the looper, the input queue and the activity lifecycle,
 * we take advantage of the fact that libFF6.so is not stripped (15k exported
 * symbols) and drive the engine directly:
 *
 *   initMainFrame(); File_SetAAssetManager(); initGame();
 *
 * initGame() starts the game thread on its own and everything below it -- EGL,
 * GL, sound, the AGB main loop -- runs exactly as it does on Android. Our Vita
 * thread is then left with the same job the glue's main thread had: feeding
 * input events in. We do that by synthesizing AInputEvent objects (we own the
 * AInputEvent_* accessors, so the struct layout is ours) and handing them to
 * the engine's own handlers, which keeps auto-repeat, the virtual pad and the
 * in-game button config working untouched.
 * ------------------------------------------------------------------------- */

extern "C" {
#include <kubridge.h>
#include <psp2/appmgr.h>
#include <psp2/apputil.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/kernel/clib.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/power.h>
#include <psp2/touch.h>
#include <vitaGL.h>

#include <OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

extern "C" {
char *strdup(const char *);
long lrand48(void);
void srand48(long);
int strncasecmp(const char *, const char *, size_t);
int usleep(unsigned);
int sceIoGetstat(const char *, SceIoStat *);
}

extern "C" {
#include "bridge.h"
#include "config.h"
#include "dialog.h"
#include "main.h"
#include "so_util.h"
}

#include "softfp_wrappers.hpp"

int SCREEN_W = DEF_SCREEN_W;
int SCREEN_H = DEF_SCREEN_H;

config_opts options;

static so_module ff6_mod;

int _newlib_heap_size_user = MEMORY_NEWLIB_MB * 1024 * 1024;
int _opensles_user_freq = 44100;

extern "C" {
void *__wrap_memcpy(void *dest, const void *src, size_t n) {
  return sceClibMemcpy(dest, src, n);
}
void *__wrap_memmove(void *dest, const void *src, size_t n) {
  return sceClibMemmove(dest, src, n);
}
void *__wrap_memset(void *s, int c, size_t n) { return sceClibMemset(s, c, n); }
}

static void set_default_options(void) {
  options.bilinear = 0;
  options.lang = 0;
  options.msaa = 2;
  options.aspect = 0;
  options.sound = 1;
}

static void loadOptions(void) {
  char line[128];
  char key[32];
  int value = 0;

  set_default_options();

  FILE *f = fopen(CONFIG_FILE_PATH, "rb");
  if (!f)
    return;

  while (fgets(line, sizeof(line), f)) {
    if (sscanf(line, "%31[^=]=%d", key, &value) != 2)
      continue;

    if (strcmp("bilinear", key) == 0)
      options.bilinear = value;
    else if (strcmp("language", key) == 0)
      options.lang = value;
    else if (strcmp("antialiasing", key) == 0)
      options.msaa = value;
    else if (strcmp("aspect", key) == 0)
      options.aspect = value;
    else if (strcmp("sound", key) == 0)
      options.sound = value;
  }

  fclose(f);
}

/* ==========================================================================
 * Trivial libc / logging shims
 * ======================================================================== */

static int ret0(void) { return 0; }
static int ret1(void) { return 1; }
static void *retNULL(void) { return NULL; }

static const char *log_prio_name(int prio) {
  switch (prio) {
  case 2: return "V";
  case 3: return "D";
  case 4: return "I";
  case 5: return "W";
  case 6: return "E";
  case 7: return "F";
  default: return "?";
  }
}

int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
  va_list list;
  char string[512];

  va_start(list, fmt);
  vsnprintf(string, sizeof(string), fmt, list);
  va_end(list);

  trace("[%s/%s] %s\n", log_prio_name(prio), tag ? tag : "?", string);
  return 0;
}

static int puts_hook(const char *s) {
  trace("[puts] %s\n", s ? s : "(null)");
  return 0;
}

void __assert2(const char *file, int line, const char *func, const char *expr) {
  fatal_error("assertion failed:\n%s:%d (%s)\n%s", file, line, func, expr);
}

static int __stack_chk_guard_fake = 0x42424242;

/* Bionic exposes stdin/stdout/stderr as &__sF[0..2]. We hand out a dummy block
 * and intercept the stdio calls that could be pointed at it. */
static char __sF_fake[3][0x100];

static int is_fake_stdio(void *stream) {
  return stream >= (void *)__sF_fake &&
         stream < (void *)((char *)__sF_fake + sizeof(__sF_fake));
}

static int fprintf_hook(FILE *stream, const char *fmt, ...) {
  va_list list;
  int ret;

  va_start(list, fmt);
  if (is_fake_stdio(stream)) {
    char string[512];
    ret = vsnprintf(string, sizeof(string), fmt, list);
    trace("[stdio] %s", string);
  } else {
    ret = vfprintf(stream, fmt, list);
  }
  va_end(list);

  return ret;
}

static int fputc_hook(int c, FILE *stream) {
  if (is_fake_stdio(stream))
    return c;
  return fputc(c, stream);
}

static int fflush_hook(FILE *stream) {
  if (!stream || is_fake_stdio(stream))
    return 0;
  return fflush(stream);
}

/* Own sleep primitives rather than newlib's: its usleep is a thin wrapper that
 * we would otherwise have to trust to be linked against the same stub set. */
static int usleep_fake(unsigned int usec) {
  if (usec)
    sceKernelDelayThread(usec);
  return 0;
}

static unsigned int sleep_fake(unsigned int seconds) {
  if (seconds)
    sceKernelDelayThread(seconds * 1000000);
  return 0;
}

static int nanosleep_fake(const struct timespec *req, struct timespec *rem) {
  if (req)
    sceKernelDelayThread(req->tv_sec * 1000000 + req->tv_nsec / 1000);
  if (rem) {
    rem->tv_sec = 0;
    rem->tv_nsec = 0;
  }
  return 0;
}

/* Bionic and newlib disagree on the numeric values of the open() flags, and
 * the .so was compiled against Bionic's. Passing them through unchanged means
 * O_CREAT never arrives: saving called
 * open("ux0:ff6/RockDat0.sav", 0x42, 0600) -- O_RDWR|O_CREAT to Bionic -- and
 * newlib read 0x42 as O_RDWR with a flag it does not use, tried to open a file
 * that did not exist yet, and returned -1. The engine then gave up before its
 * fopen("wb"), which is why a save reported failure with no write-mode open
 * anywhere in the trace.
 *
 * Only the bits that differ need mapping; the low two bits (RDONLY/WRONLY/
 * RDWR) are the same on both. */
/* The .so builds its paths from a hardcoded "/sdcard/", the Android external
 * storage root, which means nothing here. Rather than rewrite string literals
 * inside the module, translate at the only door they can leave through: our
 * own file shims. Everything then lands under the game's own data directory. */
static const char *translate_path(const char *path, char *buf, size_t buflen) {
  static const char sdcard[] = "/sdcard/";

  if (!path || strncmp(path, sdcard, sizeof(sdcard) - 1) != 0)
    return path;

  snprintf(buf, buflen, DATA_PATH "/%s", path + sizeof(sdcard) - 1);
  return buf;
}

#define BIONIC_O_CREAT 0x40
#define BIONIC_O_EXCL 0x80
#define BIONIC_O_TRUNC 0x200
#define BIONIC_O_APPEND 0x400
#define BIONIC_O_NONBLOCK 0x800

static int translate_open_flags(int flags) {
  int out = flags & O_ACCMODE;

  if (flags & BIONIC_O_CREAT)
    out |= O_CREAT;
  if (flags & BIONIC_O_EXCL)
    out |= O_EXCL;
  if (flags & BIONIC_O_TRUNC)
    out |= O_TRUNC;
  if (flags & BIONIC_O_APPEND)
    out |= O_APPEND;
  if (flags & BIONIC_O_NONBLOCK)
    out |= O_NONBLOCK;

  return out;
}

static int open_fake(const char *path, int flags, ...) {
  va_list ap;
  va_start(ap, flags);
  int mode = va_arg(ap, int);
  va_end(ap);

  char buf[256];
  return open(translate_path(path, buf, sizeof(buf)),
              translate_open_flags(flags), mode);
}

static FILE *fopen_fake(const char *path, const char *mode) {
  char buf[256];
  return fopen(translate_path(path, buf, sizeof(buf)), mode);
}

static void *mmap_fake(void *addr, size_t length, int prot, int flags, int fd,
                       off_t offset) {
  return malloc(length);
}

static int munmap_fake(void *addr, size_t length) {
  free(addr);
  return 0;
}

static const short _C_toupper_[] = {
    -1,   0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20, 0x21, 0x22,
    0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e,
    0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a,
    0x3b, 0x3c, 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e, 0x4f, 0x50, 0x51, 0x52,
    0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e,
    0x5f, 0x60, 'A',  'B',  'C',  'D',  'E',  'F',  'G',  'H',  'I',  'J',
    'K',  'L',  'M',  'N',  'O',  'P',  'Q',  'R',  'S',  'T',  'U',  'V',
    'W',  'X',  'Y',  'Z',  0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x82,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x8b, 0x8c, 0x8d, 0x8e,
    0x8f, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a,
    0x9b, 0x9c, 0x9d, 0x9e, 0x9f, 0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6,
    0xa7, 0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb0, 0xb1, 0xb2,
    0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xbb, 0xbc, 0xbd, 0xbe,
    0xbf, 0xc0, 0xc1, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca,
    0xcb, 0xcc, 0xcd, 0xce, 0xcf, 0xd0, 0xd1, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6,
    0xd7, 0xd8, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde, 0xdf, 0xe0, 0xe1, 0xe2,
    0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xeb, 0xec, 0xed, 0xee,
    0xef, 0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
    0xfb, 0xfc, 0xfd, 0xfe, 0xff};

const short *_toupper_tab_ = _C_toupper_;

/* libFF6.so is built with -funwind-tables; give the unwinder the real section
 * so a stray throw does not take the whole process down.
 *
 * so_load() frees the staging buffer its ehdr/phdr pointers refer to, so we
 * snapshot PT_ARM_EXIDX straight from the file before loading. */
static uintptr_t exidx_vaddr = 0;
static size_t exidx_size = 0;

static void find_exidx_bounds(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return;

  Elf32_Ehdr ehdr;
  if (fread(&ehdr, sizeof(ehdr), 1, f) != 1) {
    fclose(f);
    return;
  }

  for (int i = 0; i < ehdr.e_phnum; i++) {
    Elf32_Phdr phdr;
    if (fseek(f, ehdr.e_phoff + i * sizeof(phdr), SEEK_SET) != 0)
      break;
    if (fread(&phdr, sizeof(phdr), 1, f) != 1)
      break;

    if (phdr.p_type == 0x70000001 /* PT_ARM_EXIDX */) {
      exidx_vaddr = phdr.p_vaddr;
      exidx_size = phdr.p_memsz;
      break;
    }
  }

  fclose(f);
}

static void *__gnu_Unwind_Find_exidx(void *pc, int *pcount) {
  if (!exidx_size) {
    *pcount = 0;
    return NULL;
  }

  *pcount = exidx_size / 8;
  return (void *)(ff6_mod.text_base + exidx_vaddr);
}

/* ==========================================================================
 * pthread shims
 *
 * Bionic's pthread_mutex_t/cond_t are opaque ints that can be statically
 * initialised to 0; newlib's are pointers to heap objects. We therefore treat
 * the .so's storage as a pointer slot and allocate lazily.
 * ======================================================================== */

static int mutex_alloc(pthread_mutex_t **uid, int type) {
  pthread_mutex_t *m = (pthread_mutex_t *)calloc(1, sizeof(pthread_mutex_t));
  if (!m)
    return -1;

  *m = (type == PTHREAD_MUTEX_RECURSIVE) ? PTHREAD_RECURSIVE_MUTEX_INITIALIZER
                                         : PTHREAD_MUTEX_INITIALIZER;

  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, type);
  int ret = pthread_mutex_init(m, &attr);
  pthread_mutexattr_destroy(&attr);

  if (ret < 0) {
    free(m);
    return -1;
  }

  *uid = m;
  return 0;
}

/* Bionic's pthread_mutexattr_t is a bare 4-byte long whose low nibble holds
 * the type (NORMAL 0, RECURSIVE 1, ERRORCHECK 2). newlib's is a pointer to a
 * much larger struct, so handing the .so's on-stack attr to newlib's
 * initialiser both scribbles past it and loses the type -- which turns a
 * recursive mutex into a plain one and self-deadlocks the game thread the
 * moment it re-locks. Implement the Bionic shape ourselves. */
#define BIONIC_MUTEXATTR_TYPE_MASK 0x0f

static int pthread_mutexattr_init_fake(int *attr) {
  if (attr)
    *attr = 0;
  return 0;
}

static int pthread_mutexattr_settype_fake(int *attr, int type) {
  if (!attr)
    return 22 /* EINVAL */;
  *attr = (*attr & ~BIONIC_MUTEXATTR_TYPE_MASK) |
          (type & BIONIC_MUTEXATTR_TYPE_MASK);
  return 0;
}

static int pthread_mutexattr_destroy_fake(int *attr) { return 0; }

static int pthread_mutex_init_fake(pthread_mutex_t **uid, const int *attr) {
  int type = PTHREAD_MUTEX_DEFAULT;

  if (attr) {
    switch (*attr & BIONIC_MUTEXATTR_TYPE_MASK) {
    case 1:
      type = PTHREAD_MUTEX_RECURSIVE;
      break;
    case 2:
      type = PTHREAD_MUTEX_ERRORCHECK;
      break;
    default:
      break;
    }
  }

  return mutex_alloc(uid, type);
}

/* Bionic's statically initialised mutexes are small magic values rather than
 * pointers, and libFF6.so is full of them:
 *
 *   PTHREAD_MUTEX_INITIALIZER            = 0
 *   PTHREAD_RECURSIVE_MUTEX_INITIALIZER  = 0x4000
 *   PTHREAD_ERRORCHECK_MUTEX_INITIALIZER = 0x8000
 *
 * Anything at or below 0x8000 is therefore one of those sentinels and needs a
 * real mutex allocated behind it; anything above is one we already made. */
/* Promoting a sentinel to a real mutex has to be atomic. Two threads reaching
 * the same statically initialised mutex at once would otherwise both allocate,
 * the second store would win, and whoever locked the first object would never
 * be unlocked again -- a deadlock that only shows up once the game is properly
 * multi-threaded. Double-checked locking behind one global guard. */
static pthread_mutex_t lazy_init_guard = PTHREAD_MUTEX_INITIALIZER;

static int ensure_mutex(pthread_mutex_t **uid) {
  if ((uintptr_t)*uid > 0x8000)
    return 0;

  pthread_mutex_lock(&lazy_init_guard);

  int ret = 0;
  uintptr_t value = (uintptr_t)*uid;

  if (value <= 0x8000) {
    int type = PTHREAD_MUTEX_DEFAULT;
    if (value == 0x4000)
      type = PTHREAD_MUTEX_RECURSIVE;
    else if (value == 0x8000)
      type = PTHREAD_MUTEX_ERRORCHECK;
    ret = mutex_alloc(uid, type);
  }

  pthread_mutex_unlock(&lazy_init_guard);
  return ret;
}

static int pthread_mutex_destroy_fake(pthread_mutex_t **uid) {
  if (uid && (uintptr_t)*uid > 0x8000) {
    pthread_mutex_destroy(*uid);
    free(*uid);
    *uid = NULL;
  }
  return 0;
}

static int pthread_mutex_lock_fake(pthread_mutex_t **uid) {
  if (ensure_mutex(uid) < 0)
    return -1;
  return pthread_mutex_lock(*uid);
}

static void set_owner(void *slot, SceUID owner);

static int pthread_mutex_unlock_fake(pthread_mutex_t **uid) {
  if (ensure_mutex(uid) < 0)
    return -1;
  set_owner(uid, 0);
  return pthread_mutex_unlock(*uid);
}

static int pthread_cond_init_fake(pthread_cond_t **cnd, const int *attr) {
  pthread_cond_t *c = (pthread_cond_t *)calloc(1, sizeof(pthread_cond_t));
  if (!c)
    return -1;

  *c = PTHREAD_COND_INITIALIZER;
  if (pthread_cond_init(c, NULL) < 0) {
    free(c);
    return -1;
  }

  *cnd = c;
  return 0;
}

static int ensure_cond(pthread_cond_t **cnd) {
  /* PTHREAD_COND_INITIALIZER is 0 under Bionic, but guard the same range as
   * the mutexes so a stray sentinel is never dereferenced. Same race, same
   * double-checked fix. */
  if ((uintptr_t)*cnd > 0x8000)
    return 0;

  pthread_mutex_lock(&lazy_init_guard);
  int ret = 0;
  if ((uintptr_t)*cnd <= 0x8000)
    ret = pthread_cond_init_fake(cnd, NULL);
  pthread_mutex_unlock(&lazy_init_guard);
  return ret;
}

static int pthread_cond_broadcast_fake(pthread_cond_t **cnd) {
  if (ensure_cond(cnd) < 0)
    return -1;
  return pthread_cond_broadcast(*cnd);
}

static int pthread_cond_signal_fake(pthread_cond_t **cnd) {
  if (ensure_cond(cnd) < 0)
    return -1;
  return pthread_cond_signal(*cnd);
}

static int pthread_cond_destroy_fake(pthread_cond_t **cnd) {
  if (cnd && (uintptr_t)*cnd > 0x8000) {
    pthread_cond_destroy(*cnd);
    free(*cnd);
    *cnd = NULL;
  }
  return 0;
}

static int pthread_cond_wait_fake(pthread_cond_t **cnd,
                                  pthread_mutex_t **mtx) {
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0)
    return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

/* Android gives every thread a 1 MB stack by default and the .so relies on it:
 * thread_main runs the whole game (AgbMain, the renderer, the event VM) on the
 * thread it is handed. pthreads-embedded defaults to far less, which shows up
 * as a return address popped as 0 somewhere deep in the call chain rather than
 * as an honest overflow. Ask for the same 1 MB Android would have given. */
#define SO_THREAD_STACK_SIZE (1024 * 1024)

static int pthread_create_fake(pthread_t *thread, const void *unused,
                               void *entry, void *arg) {
  pthread_attr_t attr;

  if (pthread_attr_init(&attr) != 0)
    return pthread_create(thread, NULL, (void *(*)(void *))entry, arg);

  pthread_attr_setstacksize(&attr, SO_THREAD_STACK_SIZE);

  int ret = pthread_create(thread, &attr, (void *(*)(void *))entry, arg);
  pthread_attr_destroy(&attr);

  if (ret != 0)
    ret = pthread_create(thread, NULL, (void *(*)(void *))entry, arg);

  return ret;
}

static int pthread_attr_init_fake(void *attr) { return 0; }
static int pthread_attr_setdetachstate_fake(void *attr, int state) { return 0; }

/* ==========================================================================
 * Fake JNI
 * ======================================================================== */

enum MethodIDs {
  UNKNOWN = 0,
  /* text rasterizer */
  SET_TEXT_COLOR,
  SET_TEXT_FONT_SIZE,
  SET_TEXT_TYPEFACE,
  SET_TEXT_UTF8,
  GET_TEXT_WIDTH,
  GET_TEXT_HEIGHT,
  GET_TEXT_IMG,
  GET_TEXT_FONT_SIZE,
  GET_SYSTEM_LANGUAGE,
  /* environment */
  GET_DATA_DIRECTORY,
  GET_OBB_DIRECTORY,
  GET_HEAP_SIZE,
  GET_CURRENT_TIME_MILLIS,
  GET_TERMINAL_TIME_FORMAT,
  GET_BATTERY_INFO,
  GET_CLOCK_TIME,
  IS_ANDROID_TV,
  /* software keyboard */
  OPEN_INPUT_DIALOG,
  GET_INPUT_TEXT,
  SET_INPUT_TEXT,
  SET_LIMIT_INPUT_TEXT,
  SET_CLIPBOARD_COPY,
  /* lifecycle */
  SET_SUSPEND_TASK,
  ABORT_PAUSE_TO_RESUME,
  IS_KEYGUARD_DISP,
};

typedef struct {
  const char *name;
  enum MethodIDs id;
} NameToMethodID;

static NameToMethodID name_to_method_ids[] = {
    {"setTextColor", SET_TEXT_COLOR},
    {"setTextFontSize", SET_TEXT_FONT_SIZE},
    {"setTextTypeface", SET_TEXT_TYPEFACE},
    {"setTextUTF8", SET_TEXT_UTF8},
    {"getTextWidth", GET_TEXT_WIDTH},
    {"getTextHeight", GET_TEXT_HEIGHT},
    {"getTextImg", GET_TEXT_IMG},
    {"getTextFontSize", GET_TEXT_FONT_SIZE},
    {"getSystemLanguage", GET_SYSTEM_LANGUAGE},
    {"getDataDirectory", GET_DATA_DIRECTORY},
    {"getObbDirectory", GET_OBB_DIRECTORY},
    {"getHeapSize", GET_HEAP_SIZE},
    {"getCurrentTimeMillis", GET_CURRENT_TIME_MILLIS},
    {"getTerminalTimeFormat", GET_TERMINAL_TIME_FORMAT},
    {"getBatteryInfo", GET_BATTERY_INFO},
    {"getClockTime", GET_CLOCK_TIME},
    {"IsAndroidTV", IS_ANDROID_TV},
    {"OpenInputDialog", OPEN_INPUT_DIALOG},
    {"GetInputText", GET_INPUT_TEXT},
    {"SetInputText", SET_INPUT_TEXT},
    {"SetLimitInputText", SET_LIMIT_INPUT_TEXT},
    {"SetClipBoardCopy", SET_CLIPBOARD_COPY},
    {"SetSuspendTask", SET_SUSPEND_TASK},
    {"AbortPauseToResume", ABORT_PAUSE_TO_RESUME},
    {"IsKeyguardDisp", IS_KEYGUARD_DISP},
};

static const char *method_name(int id) {
  for (size_t i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID);
       i++) {
    if (name_to_method_ids[i].id == id)
      return name_to_method_ids[i].name;
  }
  return "?";
}

/* Report anything the game asks for that we do not implement: an unknown name
 * means a missing entry in the table, an unhandled id means the name resolved
 * but the matching Call*Method dispatcher has no case for it. Both show up as
 * a silently wrong return value otherwise. */
static void report_unhandled(const char *kind, int id) {
  static int seen[64];
  static int seen_count;

  for (int i = 0; i < seen_count; i++) {
    if (seen[i] == id)
      return;
  }
  if (seen_count < (int)(sizeof(seen) / sizeof(seen[0])))
    seen[seen_count++] = id;

  trace("[jni] UNHANDLED %s id=%d (%s)\n", kind, id, method_name(id));
}

static int lookup_method(const char *name, const char *sig) {
  for (size_t i = 0; i < sizeof(name_to_method_ids) / sizeof(NameToMethodID);
       i++) {
    if (strcmp(name, name_to_method_ids[i].name) == 0)
      return name_to_method_ids[i].id;
  }

  trace("[jni] UNKNOWN method %s%s\n", name, sig ? sig : "");
  return UNKNOWN;
}

static int GetMethodID(void *env, void *clazz, const char *name,
                       const char *sig) {
  return lookup_method(name, sig);
}

static int GetStaticMethodID(void *env, void *clazz, const char *name,
                             const char *sig) {
  return lookup_method(name, sig);
}

/* Local references
 *
 * On a real VM every local ref a native method creates is reclaimed when that
 * method returns, and libFF6.so leans on exactly that: it never calls
 * DeleteLocalRef, so handing out fresh allocations would leak one string per
 * getInputText() poll and one byte[] per text draw.
 *
 * Instead we hand out slots from fixed rotating pools. Nothing in this game
 * keeps a local ref alive across more than a couple of JNI calls -- a byte[]
 * lives from NewByteArray() to the method call that consumes it -- so a pool
 * this size is never wrapped around while a reference is still in use, and
 * memory stays bounded with no ownership tracking at all. */

#define JNI_STRING_POOL 32
#define JNI_ARRAY_POOL 32

static char *string_pool[JNI_STRING_POOL];
static int string_pool_next = 0;

static jni_bytearray array_pool[JNI_ARRAY_POOL];
static int array_pool_next = 0;

static void DeleteLocalRef(void *env, void *localRef) {}

static void *FindClass(void) { return (void *)0x41414141; }
static void *GetObjectClass(void *env, void *obj) { return (void *)0x41414141; }

static void CallVoidMethodV(void *env, void *obj, int methodID,
                            uintptr_t *args) {
  switch (methodID) {
  case GET_TERMINAL_TIME_FORMAT: {
    /* (byte[], long): the 64-bit argument is 8-byte aligned in the va_list. */
    jni_bytearray *out = (jni_bytearray *)args[0];
    int64_t millis;
    memcpy(&millis, &args[2], sizeof(millis));
    getTerminalTimeFormat(out, millis);
    break;
  }
  case OPEN_INPUT_DIALOG:
    openInputDialog((int)args[0]);
    break;
  case SET_INPUT_TEXT:
    setInputText((const char *)args[0]);
    break;
  case SET_LIMIT_INPUT_TEXT:
    setLimitInputText((int)args[0]);
    break;
  case SET_CLIPBOARD_COPY:
    setClipBoardCopy((const char *)args[0]);
    break;
  case SET_SUSPEND_TASK:
  case ABORT_PAUSE_TO_RESUME:
    break;
  default:
    report_unhandled("void", methodID);
    break;
  }
}

static void CallStaticVoidMethodV(void *env, void *clazz, int methodID,
                                  uintptr_t *args) {
  switch (methodID) {
  case SET_TEXT_COLOR:
    setTextColor((int)args[0]);
    break;
  case SET_TEXT_FONT_SIZE:
    setTextFontSize((int)args[0]);
    break;
  case SET_TEXT_TYPEFACE:
    setTextTypeface((int)args[0]);
    break;
  case SET_TEXT_UTF8: {
    jni_bytearray *array = (jni_bytearray *)args[0];
    if (array)
      setTextUTF8(array->elements, array->size);
    break;
  }
  default:
    CallVoidMethodV(env, clazz, methodID, args);
    break;
  }
}

static int CallIntMethodV(void *env, void *obj, int methodID,
                          uintptr_t *args) {
  switch (methodID) {
  case GET_TEXT_WIDTH:
    return getTextWidth();
  case GET_TEXT_HEIGHT:
    return getTextHeight();
  case GET_TEXT_FONT_SIZE:
    return getTextFontSize((int)args[0]);
  case GET_SYSTEM_LANGUAGE:
    return getSystemLanguage();
  case GET_BATTERY_INFO:
    return getBatteryInfo();
  case GET_CLOCK_TIME:
    return getClockTime();
  default:
    report_unhandled("int", methodID);
    return 0;
  }
}

static int CallStaticIntMethodV(void *env, void *clazz, int methodID,
                                uintptr_t *args) {
  return CallIntMethodV(env, clazz, methodID, args);
}

static int64_t CallLongMethodV(void *env, void *obj, int methodID,
                               uintptr_t *args) {
  switch (methodID) {
  case GET_HEAP_SIZE:
    return getHeapSize();
  case GET_CURRENT_TIME_MILLIS: {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
  }
  default:
    report_unhandled("long", methodID);
    return 0;
  }
}

static int64_t CallStaticLongMethodV(void *env, void *clazz, int methodID,
                                     uintptr_t *args) {
  return CallLongMethodV(env, clazz, methodID, args);
}

static char *NewStringUTF(void *env, const char *bytes);

/* Every object-returning method lives here, static or not: FF6.java mixes the
 * two (getTextImg() is an instance method reached through GetMethodID, while
 * the setText*() family is static) and the method IDs are unique anyway. */
static void *CallObjectMethodV(void *env, void *obj, int methodID,
                               uintptr_t *args) {
  switch (methodID) {
  case GET_TEXT_IMG:
    return getTextImg();
  case GET_DATA_DIRECTORY:
    return NewStringUTF(env, getDataDirectory());
  case GET_OBB_DIRECTORY:
    return NewStringUTF(env, getObbDirectory());
  case GET_INPUT_TEXT:
    return NewStringUTF(env, getInputText());
  default:
    report_unhandled("object", methodID);
    return NULL;
  }
}

static void *CallStaticObjectMethodV(void *env, void *clazz, int methodID,
                                     uintptr_t *args) {
  return CallObjectMethodV(env, clazz, methodID, args);
}

static int CallBooleanMethodV(void *env, void *obj, int methodID,
                              uintptr_t *args) {
  switch (methodID) {
  case IS_KEYGUARD_DISP:
    return 0;
  case IS_ANDROID_TV:
    return isAndroidTV();
  default:
    report_unhandled("boolean", methodID);
    return 0;
  }
}

static int CallStaticBooleanMethodV(void *env, void *clazz, int methodID,
                                    uintptr_t *args) {
  return CallBooleanMethodV(env, clazz, methodID, args);
}

static SOFTFP_ABI float CallFloatMethodV(void *env, void *obj, int methodID,
                                         uintptr_t *args) {
  return 0.0f;
}

static SOFTFP_ABI float CallStaticFloatMethodV(void *env, void *clazz,
                                               int methodID, uintptr_t *args) {
  return 0.0f;
}

static void *NewObjectV(void *env, void *clazz, int methodID, va_list args) {
  return NULL;
}

static int GetFieldID(void *env, void *clazz, const char *name,
                      const char *sig) {
  return 0;
}

static int GetStaticFieldID(void *env, void *clazz, const char *name,
                            const char *sig) {
  return 0;
}

static void *GetStaticObjectField(void *clazz, int fieldID) {
  return (void *)"";
}

static char *NewStringUTF(void *env, const char *bytes) {
  if (!bytes)
    return NULL;

  size_t length = strlen(bytes) + 1;

  char **slot = &string_pool[string_pool_next];
  string_pool_next = (string_pool_next + 1) % JNI_STRING_POOL;

  char *buffer = (char *)realloc(*slot, length);
  if (!buffer)
    return NULL;
  *slot = buffer;

  memcpy(buffer, bytes, length);
  return buffer;
}

static char *GetStringUTFChars(void *env, char *string, int *isCopy) {
  if (isCopy)
    *isCopy = 0;
  return string;
}

static int GetStringUTFLength(void *env, char *string) {
  return string ? strlen(string) : 0;
}

static void GetStringUTFRegion(void *env, char *str, int start, int len,
                               char *buf) {
  if (str && buf)
    memcpy(buf, str + start, len);
}

static int GetArrayLength(void *env, jni_bytearray *obj) {
  return obj ? obj->size : 0;
}

static void *GetByteArrayElements(void *env, jni_bytearray *obj) {
  return obj ? obj->elements : NULL;
}

static void *GetIntArrayElements(void *env, jni_intarray *obj) {
  return obj ? obj->elements : NULL;
}

static void *NewByteArray(void *env, size_t length) {
  if (!length)
    length = 1;

  jni_bytearray *array = &array_pool[array_pool_next];
  array_pool_next = (array_pool_next + 1) % JNI_ARRAY_POOL;

  unsigned char *elements =
      (unsigned char *)realloc(array->elements, length);
  if (!elements)
    return NULL;

  memset(elements, 0, length);
  array->elements = elements;
  array->size = length;
  return array;
}

/* Both arrays are pool-owned and the .so hands back the very pointer we gave
 * it, so there is nothing to copy back or free. */
static int ReleaseByteArrayElements(void *env, jni_bytearray *obj, void *elems,
                                    int mode) {
  return 0;
}

static int ReleaseIntArrayElements(void *env, jni_intarray *obj, void *elems,
                                   int mode) {
  return 0;
}

static void SetByteArrayRegion(void *env, jni_bytearray *array, size_t start,
                               size_t len, const unsigned char *buf) {
  if (array && array->elements)
    memcpy(array->elements + start, buf, len);
}

/* The .so reaches JNIEnv both ways: its C++ wrapper class funnels through the
 * ...MethodV slots, but plenty of call sites index the plain variadic slot
 * directly (IsAndroidTV loads env+0x94, CallBooleanMethod). Leaving those NULL
 * shows up as a jump to 0, so every variadic entry gets a trampoline that
 * hands the va_list on to the V implementation -- which is what the wrapper
 * would have done anyway. */
#define JNI_VARIADIC_TRAMPOLINE(ret_type, name)                                \
  static ret_type name(void *env, void *obj, int methodID, ...) {              \
    va_list list;                                                              \
    va_start(list, methodID);                                                  \
    ret_type ret = name##V(env, obj, methodID, *(uintptr_t **)&list);             \
    va_end(list);                                                              \
    return ret;                                                                \
  }

JNI_VARIADIC_TRAMPOLINE(void *, CallObjectMethod)
JNI_VARIADIC_TRAMPOLINE(int, CallBooleanMethod)
JNI_VARIADIC_TRAMPOLINE(int, CallIntMethod)
JNI_VARIADIC_TRAMPOLINE(int64_t, CallLongMethod)
JNI_VARIADIC_TRAMPOLINE(void *, CallStaticObjectMethod)
JNI_VARIADIC_TRAMPOLINE(int, CallStaticBooleanMethod)
JNI_VARIADIC_TRAMPOLINE(int, CallStaticIntMethod)
JNI_VARIADIC_TRAMPOLINE(int64_t, CallStaticLongMethod)

/* void returns cannot go through the macro's `return ret`. */
static void CallVoidMethod(void *env, void *obj, int methodID, ...) {
  va_list list;
  va_start(list, methodID);
  CallVoidMethodV(env, obj, methodID, *(uintptr_t **)&list);
  va_end(list);
}

static void CallStaticVoidMethod(void *env, void *clazz, int methodID, ...) {
  va_list list;
  va_start(list, methodID);
  CallStaticVoidMethodV(env, clazz, methodID, *(uintptr_t **)&list);
  va_end(list);
}

static SOFTFP_ABI float CallFloatMethod(void *env, void *obj, int methodID,
                                        ...) {
  return 0.0f;
}

static SOFTFP_ABI float CallStaticFloatMethod(void *env, void *clazz,
                                              int methodID, ...) {
  return 0.0f;
}

static char fake_env[0x1000];

static void InitJNIEnv(void) {
  memset(fake_env, 0, sizeof(fake_env));

  *(uintptr_t *)(fake_env + 0x00) = (uintptr_t)fake_env;
  *(uintptr_t *)(fake_env + 0x18) = (uintptr_t)FindClass;
  *(uintptr_t *)(fake_env + 0x5C) = (uintptr_t)DeleteLocalRef;
  *(uintptr_t *)(fake_env + 0x74) = (uintptr_t)NewObjectV;
  *(uintptr_t *)(fake_env + 0x7C) = (uintptr_t)GetObjectClass;
  *(uintptr_t *)(fake_env + 0x84) = (uintptr_t)GetMethodID;
  *(uintptr_t *)(fake_env + 0x88) = (uintptr_t)CallObjectMethod;
  *(uintptr_t *)(fake_env + 0x8C) = (uintptr_t)CallObjectMethodV;
  *(uintptr_t *)(fake_env + 0x94) = (uintptr_t)CallBooleanMethod;
  *(uintptr_t *)(fake_env + 0x98) = (uintptr_t)CallBooleanMethodV;
  *(uintptr_t *)(fake_env + 0xC4) = (uintptr_t)CallIntMethod;
  *(uintptr_t *)(fake_env + 0xC8) = (uintptr_t)CallIntMethodV;
  *(uintptr_t *)(fake_env + 0xD0) = (uintptr_t)CallLongMethod;
  *(uintptr_t *)(fake_env + 0xD4) = (uintptr_t)CallLongMethodV;
  *(uintptr_t *)(fake_env + 0xDC) = (uintptr_t)CallFloatMethod;
  *(uintptr_t *)(fake_env + 0xE0) = (uintptr_t)CallFloatMethodV;
  *(uintptr_t *)(fake_env + 0xF4) = (uintptr_t)CallVoidMethod;
  *(uintptr_t *)(fake_env + 0xF8) = (uintptr_t)CallVoidMethodV;
  *(uintptr_t *)(fake_env + 0x178) = (uintptr_t)GetFieldID;
  *(uintptr_t *)(fake_env + 0x1C4) = (uintptr_t)GetStaticMethodID;
  *(uintptr_t *)(fake_env + 0x1C8) = (uintptr_t)CallStaticObjectMethod;
  *(uintptr_t *)(fake_env + 0x1CC) = (uintptr_t)CallStaticObjectMethodV;
  *(uintptr_t *)(fake_env + 0x1D4) = (uintptr_t)CallStaticBooleanMethod;
  *(uintptr_t *)(fake_env + 0x1D8) = (uintptr_t)CallStaticBooleanMethodV;
  *(uintptr_t *)(fake_env + 0x204) = (uintptr_t)CallStaticIntMethod;
  *(uintptr_t *)(fake_env + 0x208) = (uintptr_t)CallStaticIntMethodV;
  *(uintptr_t *)(fake_env + 0x210) = (uintptr_t)CallStaticLongMethod;
  *(uintptr_t *)(fake_env + 0x214) = (uintptr_t)CallStaticLongMethodV;
  *(uintptr_t *)(fake_env + 0x21C) = (uintptr_t)CallStaticFloatMethod;
  *(uintptr_t *)(fake_env + 0x220) = (uintptr_t)CallStaticFloatMethodV;
  *(uintptr_t *)(fake_env + 0x234) = (uintptr_t)CallStaticVoidMethod;
  *(uintptr_t *)(fake_env + 0x238) = (uintptr_t)CallStaticVoidMethodV;
  *(uintptr_t *)(fake_env + 0x240) = (uintptr_t)GetStaticFieldID;
  *(uintptr_t *)(fake_env + 0x244) = (uintptr_t)GetStaticObjectField;
  *(uintptr_t *)(fake_env + 0x29C) = (uintptr_t)NewStringUTF;
  *(uintptr_t *)(fake_env + 0x2A0) = (uintptr_t)GetStringUTFLength;
  *(uintptr_t *)(fake_env + 0x2A4) = (uintptr_t)GetStringUTFChars;
  *(uintptr_t *)(fake_env + 0x2A8) = (uintptr_t)ret0;
  *(uintptr_t *)(fake_env + 0x2AC) = (uintptr_t)GetArrayLength;
  *(uintptr_t *)(fake_env + 0x2C0) = (uintptr_t)NewByteArray;
  *(uintptr_t *)(fake_env + 0x2E0) = (uintptr_t)GetByteArrayElements;
  *(uintptr_t *)(fake_env + 0x2EC) = (uintptr_t)GetIntArrayElements;
  *(uintptr_t *)(fake_env + 0x300) = (uintptr_t)ReleaseByteArrayElements;
  *(uintptr_t *)(fake_env + 0x30C) = (uintptr_t)ReleaseIntArrayElements;
  *(uintptr_t *)(fake_env + 0x340) = (uintptr_t)SetByteArrayRegion;
  *(uintptr_t *)(fake_env + 0x374) = (uintptr_t)GetStringUTFRegion;
}

/* JavaVM: Rock::ObbFile::SetObbFilePath attaches and detaches around its call
 * into getObbDirectory(), so those two slots have to be real. */
static int AttachCurrentThread(void *vm, void **p_env, void *args) {
  *p_env = fake_env;
  return 0;
}

static int DetachCurrentThread(void *vm) { return 0; }

static int GetEnv(void *vm, void **p_env, int version) {
  *p_env = fake_env;
  return 0;
}

static uintptr_t fake_vm[8];

static void InitJavaVM(void) {
  memset(fake_vm, 0, sizeof(fake_vm));
  fake_vm[0] = (uintptr_t)fake_vm;
  fake_vm[4] = (uintptr_t)AttachCurrentThread; /* +0x10 */
  fake_vm[5] = (uintptr_t)DetachCurrentThread; /* +0x14 */
  fake_vm[6] = (uintptr_t)GetEnv;              /* +0x18 */
}

/* ==========================================================================
 * Fake NativeActivity / android_app
 * ======================================================================== */

typedef struct {
  void *callbacks;
  void *vm;
  void *env;
  void *clazz;
  const char *internalDataPath;
  const char *externalDataPath;
  int32_t sdkVersion;
  void *instance;
  void *assetManager;
  const char *obbPath;
} fake_native_activity;

typedef struct {
  void *userData;         /* +0  */
  void *onAppCmd;         /* +4  */
  void *onInputEvent;     /* +8  */
  void *activity;         /* +12 */
  void *config;           /* +16 */
  void *savedState;       /* +20 */
  size_t savedStateSize;  /* +24 */
  void *looper;           /* +28 */
  void *inputQueue;       /* +32 */
  void *window;           /* +36 */
  int32_t contentRect[4]; /* +40 */
  int32_t activityState;  /* +56 */
  int32_t destroyRequested;
  char glue_private[128];
} fake_android_app;

static fake_native_activity g_activity;
static fake_android_app g_android_app;

/* Offsets into libFF6.so's `g_app` engine struct, recovered from
 * initDisplay / GetViewPort / initGame. */
#define ENGINE_APP 0
#define ENGINE_RENDER_W 24
#define ENGINE_RENDER_H 28
#define ENGINE_SCREEN_W 32
#define ENGINE_SCREEN_H 36

static uint8_t *g_app = NULL;

static inline int32_t *engine_field(int offset) {
  return (int32_t *)(g_app + offset);
}

/* ==========================================================================
 * Fake AAssetManager
 *
 * Rock::File::Open tries the .obb first and only falls back to the asset
 * manager, and the APK's assets/ folder holds a single 10-byte ct.inf, so a
 * thin stdio shim over DATA_PATH/assets is all this needs to be.
 * ======================================================================== */

typedef struct {
  FILE *fp;
  long size;
} fake_asset;

static void *AAssetManager_open(void *mgr, const char *filename, int mode) {
  char path[512];
  snprintf(path, sizeof(path), DATA_PATH "/assets/%s", filename);

  FILE *fp = fopen(path, "rb");
  if (!fp)
    return NULL;

  fake_asset *asset = (fake_asset *)malloc(sizeof(fake_asset));
  if (!asset) {
    fclose(fp);
    return NULL;
  }

  fseek(fp, 0, SEEK_END);
  asset->size = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  asset->fp = fp;

  return asset;
}

static int AAsset_read(void *a, void *buf, size_t count) {
  fake_asset *asset = (fake_asset *)a;
  if (!asset)
    return -1;
  return (int)fread(buf, 1, count, asset->fp);
}

static long AAsset_seek(void *a, long offset, int whence) {
  fake_asset *asset = (fake_asset *)a;
  if (!asset || fseek(asset->fp, offset, whence) != 0)
    return -1;
  return ftell(asset->fp);
}

static long AAsset_getLength(void *a) {
  fake_asset *asset = (fake_asset *)a;
  return asset ? asset->size : 0;
}

static void AAsset_close(void *a) {
  fake_asset *asset = (fake_asset *)a;
  if (!asset)
    return;
  fclose(asset->fp);
  free(asset);
}

static void *AAssetManager_fromJava(void *env, void *obj) {
  return &g_activity;
}

/* ==========================================================================
 * android_native_app_glue / looper stubs
 *
 * Dead code in this port: we never enter the glue. They exist so relocation
 * succeeds and so a stray call cannot jump into the void.
 * ======================================================================== */

static int ALooper_pollAll(int timeout, int *fd, int *events, void **data) {
  if (events)
    *events = 0;
  if (data)
    *data = NULL;
  return -1;
}

static void *AConfiguration_new(void) { return calloc(1, 64); }
static void AConfiguration_delete(void *config) { free(config); }
static void AConfiguration_fromAssetManager(void *config, void *mgr) {}

static void AConfiguration_getLanguage(void *config, char *out) {
  static const char *codes[] = {"ja", "en", "en", "de", "fr", "it", "es",
                                "zh", "zh", "ko", "th", "ru", "pt"};
  int language = getSystemLanguage();
  if (language < 0 || language >= (int)(sizeof(codes) / sizeof(codes[0])))
    language = 1;

  out[0] = codes[language][0];
  out[1] = codes[language][1];
}

static void AConfiguration_getCountry(void *config, char *out) {
  out[0] = '\0';
  out[1] = '\0';
}

/* ==========================================================================
 * Synthetic input events
 * ======================================================================== */

#define AINPUT_EVENT_TYPE_KEY 1
#define AINPUT_EVENT_TYPE_MOTION 2

#define AKEY_EVENT_ACTION_DOWN 0
#define AKEY_EVENT_ACTION_UP 1

#define AMOTION_EVENT_ACTION_DOWN 0
#define AMOTION_EVENT_ACTION_UP 1
#define AMOTION_EVENT_ACTION_MOVE 2

#define AINPUT_SOURCE_GAMEPAD 0x00000401
#define AINPUT_SOURCE_TOUCHSCREEN 0x00001002

#define MAX_TOUCH_POINTS 4

typedef struct {
  int32_t type;
  int32_t source;
  int32_t action;
  int32_t keycode;
  int32_t pointer_count;
  float x[MAX_TOUCH_POINTS];
  float y[MAX_TOUCH_POINTS];
} fake_input_event;

static int32_t AInputEvent_getType(const void *e) {
  return ((const fake_input_event *)e)->type;
}

static int32_t AInputEvent_getSource(const void *e) {
  return ((const fake_input_event *)e)->source;
}

static int32_t AKeyEvent_getAction(const void *e) {
  return ((const fake_input_event *)e)->action;
}

static int32_t AKeyEvent_getKeyCode(const void *e) {
  return ((const fake_input_event *)e)->keycode;
}

static size_t AMotionEvent_getPointerCount(const void *e) {
  return ((const fake_input_event *)e)->pointer_count;
}

/* The .so is softfp, so these have to hand the float back in r0. */
static SOFTFP_ABI float AMotionEvent_getX(const void *e, size_t index) {
  const fake_input_event *event = (const fake_input_event *)e;
  if (index >= MAX_TOUCH_POINTS)
    return 0.0f;
  return event->x[index];
}

static SOFTFP_ABI float AMotionEvent_getY(const void *e, size_t index) {
  const fake_input_event *event = (const fake_input_event *)e;
  if (index >= MAX_TOUCH_POINTS)
    return 0.0f;
  return event->y[index];
}

/* ==========================================================================
 * EGL over vitaGL
 * ======================================================================== */

static void gl_stats_report(void);

#define FAKE_EGL_DISPLAY ((void *)0xE9100001)
#define FAKE_EGL_SURFACE ((void *)0xE9100002)
#define FAKE_EGL_CONTEXT ((void *)0xE9100003)
#define FAKE_EGL_CONFIG ((void *)0xE9100004)

#define EGL_HEIGHT 0x3056
#define EGL_WIDTH 0x3057

static void *fake_eglGetDisplay(void *display_id) { return FAKE_EGL_DISPLAY; }

static unsigned fake_eglInitialize(void *dpy, int *major, int *minor) {
  if (major)
    *major = 1;
  if (minor)
    *minor = 4;
  return 1;
}

static unsigned fake_eglTerminate(void *dpy) { return 1; }

static unsigned fake_eglChooseConfig(void *dpy, const int *attrib, void **configs,
                                int size, int *num_config) {
  if (configs && size > 0)
    configs[0] = FAKE_EGL_CONFIG;
  if (num_config)
    *num_config = 1;
  return 1;
}

static unsigned fake_eglGetConfigAttrib(void *dpy, void *config, int attribute,
                                   int *value) {
  if (value)
    *value = 0;
  return 1;
}

static void *fake_eglCreateWindowSurface(void *dpy, void *config, void *window,
                                    const int *attrib) {
  return FAKE_EGL_SURFACE;
}

static void *fake_eglCreateContext(void *dpy, void *config, void *share,
                              const int *attrib) {
  return FAKE_EGL_CONTEXT;
}

static unsigned fake_eglMakeCurrent(void *dpy, void *draw, void *read, void *ctx) {
  return 1;
}

static unsigned fake_eglDestroySurface(void *dpy, void *surface) { return 1; }
static unsigned fake_eglDestroyContext(void *dpy, void *ctx) { return 1; }

static unsigned fake_eglQuerySurface(void *dpy, void *surface, int attribute,
                                int *value) {
  if (!value)
    return 0;

  switch (attribute) {
  case EGL_WIDTH:
    *value = SCREEN_W;
    break;
  case EGL_HEIGHT:
    *value = SCREEN_H;
    break;
  default:
    *value = 0;
    break;
  }
  return 1;
}

static unsigned fake_eglSwapBuffers(void *dpy, void *surface) {
  gl_stats_report();

  /* The IME dialog has to be composited by the Vita's own display stack, so we
   * ask vitaGL to keep the framebuffer around while it is up. */
  vglSwapBuffers(isInputDialogOpen() ? GL_TRUE : GL_FALSE);
  return 1;
}

static void ANativeWindow_setBuffersGeometry(void *window, int w, int h,
                                             int format) {}

/* ==========================================================================
 * GL shims
 * ======================================================================== */

static void glTexParameteriHook(GLenum target, GLenum pname, GLint param) {
  if (options.bilinear &&
      (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)) {
    glTexParameteri(target, pname, GL_LINEAR);
    return;
  }
  glTexParameteri(target, pname, param);
}

static SOFTFP_ABI void glTexParameterfHook(GLenum target, GLenum pname,
                                           GLfloat param) {
  if (options.bilinear &&
      (pname == GL_TEXTURE_MIN_FILTER || pname == GL_TEXTURE_MAG_FILTER)) {
    glTexParameterf(target, pname, (GLfloat)GL_LINEAR);
    return;
  }
  glTexParameterf(target, pname, param);
}

/* vitaGL has no fixed-function light model; the game only ever sets the global
 * ambient, which we can safely ignore. */
static SOFTFP_ABI void glLightModelf(GLenum pname, GLfloat param) {}

/* Hang watchdog: per-thread record of the last shim called, plus who owns each
 * contended mutex, so a stall names both the blocked thread and the holder. */
#define TRACK_SLOTS 16

static struct {
  SceUID tid;
  const char *shim;
  unsigned arg;
} thread_marks[TRACK_SLOTS];

static struct {
  void *slot;
  SceUID owner;
} mutex_owners[TRACK_SLOTS];

static void mark_shim(const char *what, unsigned arg) {
  SceUID tid = sceKernelGetThreadId();
  for (int i = 0; i < TRACK_SLOTS; i++) {
    if (thread_marks[i].tid == tid || thread_marks[i].tid == 0) {
      thread_marks[i].tid = tid;
      thread_marks[i].shim = what;
      thread_marks[i].arg = arg;
      return;
    }
  }
}

static const char *shim_of(SceUID tid, unsigned *arg) {
  for (int i = 0; i < TRACK_SLOTS; i++) {
    if (thread_marks[i].tid == tid) {
      *arg = thread_marks[i].arg;
      return thread_marks[i].shim;
    }
  }
  *arg = 0;
  return "(unknown)";
}

static void set_owner(void *slot, SceUID owner) {
  for (int i = 0; i < TRACK_SLOTS; i++) {
    if (mutex_owners[i].slot == slot || mutex_owners[i].slot == NULL) {
      mutex_owners[i].slot = slot;
      mutex_owners[i].owner = owner;
      return;
    }
  }
}

static SceUID owner_of(void *slot) {
  for (int i = 0; i < TRACK_SLOTS; i++) {
    if (mutex_owners[i].slot == slot)
      return mutex_owners[i].owner;
  }
  return 0;
}

static volatile const char *last_shim = "(none)";
static volatile unsigned last_shim_arg;

#define MARK(x)                                                                \
  do {                                                                         \
    last_shim = (x);                                                           \
    mark_shim((x), 0);                                                         \
  } while (0)
#define MARK1(x, a)                                                            \
  do {                                                                         \
    last_shim = (x);                                                           \
    last_shim_arg = (unsigned)(a);                                             \
    mark_shim((x), (unsigned)(a));                                             \
  } while (0)

static FILE *fopen_mark(const char *path, const char *mode) {
  MARK("fopen");
  char buf[256];
  path = translate_path(path, buf, sizeof(buf));
  return fopen(path, mode);
}

static size_t fread_mark(void *p, size_t sz, size_t n, FILE *f) {
  MARK1("fread", sz * n);
  return fread(p, sz, n, f);
}

static int fseek_mark(FILE *f, long off, int whence) {
  MARK("fseek");
  return fseek(f, off, whence);
}

static int usleep_mark(unsigned int usec) {
  MARK1("usleep", usec);
  if (usec)
    sceKernelDelayThread(usec);
  return 0;
}

static int mutex_lock_mark(pthread_mutex_t **uid) {
  MARK("pthread_mutex_lock");
  if (ensure_mutex(uid) < 0)
    return -1;

  if (pthread_mutex_trylock(*uid) == 0) {
    set_owner(uid, sceKernelGetThreadId());
    return 0;
  }

  /* Contended. Report who is stuck and on what: the slot address lands in the
   * .so's data, so it maps straight back to a named global. */
  SceUInt64 start = sceKernelGetSystemTimeWide();
  int reported = 0;

  for (;;) {
    if (pthread_mutex_trylock(*uid) == 0) {
      set_owner(uid, sceKernelGetThreadId());
      if (reported)
        trace("[lock] t%08x got slot %p at last\n",
              (unsigned)sceKernelGetThreadId(), (void *)uid);
      return 0;
    }

    if (!reported && sceKernelGetSystemTimeWide() - start > 1000000) {
      SceUID owner = owner_of(uid);
      unsigned oarg = 0;
      const char *oshim = shim_of(owner, &oarg);
      trace("[lock] t%08x STUCK on .so+0x%x; holder=t%08x last=%s arg=0x%x\n",
            (unsigned)sceKernelGetThreadId(),
            (unsigned)((uintptr_t)uid - ff6_mod.text_base), (unsigned)owner,
            oshim, oarg);
      reported = 1;
    }

    sceKernelDelayThread(1000);
  }
}

static int cond_wait_mark(pthread_cond_t **cnd, pthread_mutex_t **mtx) {
  MARK("pthread_cond_wait");
  if (ensure_cond(cnd) < 0 || ensure_mutex(mtx) < 0)
    return -1;
  return pthread_cond_wait(*cnd, *mtx);
}

/* Experiment hook: with options.sound == 0 we refuse to create the OpenSL ES
 * engine, which is the quickest way to tell whether the game's clock is gated
 * on the sound system coming up. */
static SLresult slCreateEngineHook(SLObjectItf *engine, SLuint32 numOptions,
                                   const SLEngineOption *options_,
                                   SLuint32 numInterfaces,
                                   const SLInterfaceID *ids,
                                   const SLboolean *req) {
  if (!options.sound) {
    trace("[snd] slCreateEngine refused (sound disabled)\n");
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return slCreateEngine(engine, numOptions, options_, numInterfaces, ids, req);
}

/* ==========================================================================
 * GL instrumentation
 *
 * Every GL entry point the .so can reach is ours, so counting them is the
 * cheapest way to see what the game actually submits per frame without
 * patching a single byte of the module.
 * ======================================================================== */

static GLuint gl_bound_texture;

static struct {
  unsigned frames;
  unsigned draws;
  unsigned clears;
  unsigned textures;
  unsigned tex_subs;
  unsigned binds;
  int last_viewport[4];
  float last_ortho[6];
  unsigned draws_this_frame;
  unsigned max_draws;
} gl_stats;

static void glDrawArraysHook(GLenum mode, GLint first, GLsizei count) {
  gl_stats.draws++;
  gl_stats.draws_this_frame++;

  /* Dump one settled frame's worth of draw calls: which texture, how many
     vertices. That is enough to tell a UI overlay from a real scene. */
  if (gl_stats.frames == 300)
    trace("[gl] draw #%u mode=0x%x first=%d count=%d tex=%u\n",
          gl_stats.draws_this_frame, mode, first, count, gl_bound_texture);

  glDrawArrays(mode, first, count);
}

static void glClearHook(GLbitfield mask) {
  gl_stats.clears++;
  glClear(mask);
}

static void glViewportHook(GLint x, GLint y, GLsizei w, GLsizei h) {
  gl_stats.last_viewport[0] = x;
  gl_stats.last_viewport[1] = y;
  gl_stats.last_viewport[2] = w;
  gl_stats.last_viewport[3] = h;
  glViewport(x, y, w, h);
}

static SOFTFP_ABI void glOrthofHook(GLfloat l, GLfloat r, GLfloat b, GLfloat t,
                                    GLfloat n, GLfloat f) {
  gl_stats.last_ortho[0] = l;
  gl_stats.last_ortho[1] = r;
  gl_stats.last_ortho[2] = b;
  gl_stats.last_ortho[3] = t;
  gl_stats.last_ortho[4] = n;
  gl_stats.last_ortho[5] = f;
  glOrthof(l, r, b, t, n, f);
}

static void glTexImage2DHook(GLenum target, GLint level, GLint internalformat,
                             GLsizei width, GLsizei height, GLint border,
                             GLenum format, GLenum type, const void *pixels) {
  gl_stats.textures++;
  MARK1("glTexImage2D", (unsigned)(width << 16 | height));
  trace("[gl] tex #%u id=%u %dx%d ifmt=0x%x fmt=0x%x type=0x%x%s\n",
        gl_stats.textures, gl_bound_texture, width, height, internalformat,
        format, type, pixels ? "" : " (empty)");
  glTexImage2D(target, level, internalformat, width, height, border, format,
               type, pixels);
}

static void glTexSubImage2DHook(GLenum target, GLint level, GLint xoffset,
                                GLint yoffset, GLsizei width, GLsizei height,
                                GLenum format, GLenum type,
                                const void *pixels) {
  gl_stats.tex_subs++;
  glTexSubImage2D(target, level, xoffset, yoffset, width, height, format, type,
                  pixels);
}

static void glBindTextureHook(GLenum target, GLuint texture) {
  gl_stats.binds++;
  gl_bound_texture = texture;
  glBindTexture(target, texture);
}

static void glCompressedTexImage2DHook(GLenum target, GLint level,
                                       GLenum internalformat, GLsizei width,
                                       GLsizei height, GLint border,
                                       GLsizei imageSize, const void *data) {
  trace("[gl] compressed tex fmt=0x%x %dx%d size=%d\n", internalformat, width,
        height, imageSize);
  glCompressedTexImage2D(target, level, internalformat, width, height, border,
                         imageSize, data);
}

static void gl_stats_report(void) {
  MARK("eglSwapBuffers");
  gl_stats.frames++;
  if (gl_stats.draws_this_frame > gl_stats.max_draws)
    gl_stats.max_draws = gl_stats.draws_this_frame;
  gl_stats.draws_this_frame = 0;

  if (gl_stats.frames % 60)
    return;

  trace("[gl] frame %u draws=%u (max/frame %u) clears=%u tex=%u sub=%u "
        "binds=%u vp=%d,%d %dx%d ortho=%d..%d,%d..%d\n",
        gl_stats.frames, gl_stats.draws, gl_stats.max_draws, gl_stats.clears,
        gl_stats.textures, gl_stats.tex_subs, gl_stats.binds,
        gl_stats.last_viewport[0], gl_stats.last_viewport[1],
        gl_stats.last_viewport[2], gl_stats.last_viewport[3],
        (int)gl_stats.last_ortho[0], (int)gl_stats.last_ortho[1],
        (int)gl_stats.last_ortho[2], (int)gl_stats.last_ortho[3]);
}

/* ==========================================================================
 * Symbol table
 * ======================================================================== */

extern "C" {
extern void *__aeabi_atexit;
extern void *__cxa_atexit;
extern void *__cxa_finalize;
extern void *__stack_chk_fail;
}

static so_default_dynlib default_dynlib[] = {
    /* android asset manager */
    {"AAssetManager_open", (uintptr_t)&AAssetManager_open},
    {"AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava},
    {"AAsset_close", (uintptr_t)&AAsset_close},
    {"AAsset_getLength", (uintptr_t)&AAsset_getLength},
    {"AAsset_read", (uintptr_t)&AAsset_read},
    {"AAsset_seek", (uintptr_t)&AAsset_seek},

    /* android configuration */
    {"AConfiguration_delete", (uintptr_t)&AConfiguration_delete},
    {"AConfiguration_fromAssetManager",
     (uintptr_t)&AConfiguration_fromAssetManager},
    {"AConfiguration_getCountry", (uintptr_t)&AConfiguration_getCountry},
    {"AConfiguration_getLanguage", (uintptr_t)&AConfiguration_getLanguage},
    {"AConfiguration_new", (uintptr_t)&AConfiguration_new},

    /* android input */
    {"AInputEvent_getSource", (uintptr_t)&AInputEvent_getSource},
    {"AInputEvent_getType", (uintptr_t)&AInputEvent_getType},
    {"AInputQueue_attachLooper", (uintptr_t)&ret0},
    {"AInputQueue_detachLooper", (uintptr_t)&ret0},
    {"AInputQueue_finishEvent", (uintptr_t)&ret0},
    {"AInputQueue_getEvent", (uintptr_t)&ret0},
    {"AInputQueue_preDispatchEvent", (uintptr_t)&ret0},
    {"AKeyEvent_getAction", (uintptr_t)&AKeyEvent_getAction},
    {"AKeyEvent_getKeyCode", (uintptr_t)&AKeyEvent_getKeyCode},
    {"AMotionEvent_getPointerCount", (uintptr_t)&AMotionEvent_getPointerCount},
    {"AMotionEvent_getX", (uintptr_t)&AMotionEvent_getX},
    {"AMotionEvent_getY", (uintptr_t)&AMotionEvent_getY},

    /* android looper / window */
    {"ALooper_addFd", (uintptr_t)&ret0},
    {"ALooper_pollAll", (uintptr_t)&ALooper_pollAll},
    {"ALooper_prepare", (uintptr_t)&retNULL},
    {"ANativeWindow_setBuffersGeometry",
     (uintptr_t)&ANativeWindow_setBuffersGeometry},

    /* OpenSL ES */
    {"SL_IID_ANDROIDSIMPLEBUFFERQUEUE",
     (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE},
    {"SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE},
    {"SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY},
    {"SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME},
    {"slCreateEngine", (uintptr_t)&slCreateEngineHook},

    /* EGL */
    {"eglChooseConfig", (uintptr_t)&fake_eglChooseConfig},
    {"eglCreateContext", (uintptr_t)&fake_eglCreateContext},
    {"eglCreateWindowSurface", (uintptr_t)&fake_eglCreateWindowSurface},
    {"eglDestroyContext", (uintptr_t)&fake_eglDestroyContext},
    {"eglDestroySurface", (uintptr_t)&fake_eglDestroySurface},
    {"eglGetConfigAttrib", (uintptr_t)&fake_eglGetConfigAttrib},
    {"eglGetDisplay", (uintptr_t)&fake_eglGetDisplay},
    {"eglInitialize", (uintptr_t)&fake_eglInitialize},
    {"eglMakeCurrent", (uintptr_t)&fake_eglMakeCurrent},
    {"eglQuerySurface", (uintptr_t)&fake_eglQuerySurface},
    {"eglSwapBuffers", (uintptr_t)&fake_eglSwapBuffers},
    {"eglTerminate", (uintptr_t)&fake_eglTerminate},

    /* GLES 1.x */
    {"glActiveTexture", (uintptr_t)&glActiveTexture},
    {"glAlphaFunc", WRAP(glAlphaFunc)},
    {"glBindTexture", (uintptr_t)&glBindTextureHook},
    {"glBlendFunc", (uintptr_t)&glBlendFunc},
    {"glClear", (uintptr_t)&glClearHook},
    {"glClearColor", WRAP(glClearColor)},
    {"glClearDepthf", WRAP(glClearDepthf)},
    {"glColor4f", WRAP(glColor4f)},
    {"glColorMask", (uintptr_t)&glColorMask},
    {"glColorPointer", (uintptr_t)&glColorPointer},
    {"glCompressedTexImage2D", (uintptr_t)&glCompressedTexImage2DHook},
    {"glCullFace", (uintptr_t)&glCullFace},
    {"glDeleteTextures", (uintptr_t)&glDeleteTextures},
    {"glDepthFunc", (uintptr_t)&glDepthFunc},
    {"glDepthMask", (uintptr_t)&glDepthMask},
    {"glDisable", (uintptr_t)&glDisable},
    {"glDisableClientState", (uintptr_t)&glDisableClientState},
    {"glDrawArrays", (uintptr_t)&glDrawArraysHook},
    {"glEnable", (uintptr_t)&glEnable},
    {"glEnableClientState", (uintptr_t)&glEnableClientState},
    {"glFogf", WRAP(glFogf)},
    {"glFogfv", (uintptr_t)&glFogfv},
    {"glFrontFace", (uintptr_t)&glFrontFace},
    {"glFrustumf", WRAP(glFrustumf)},
    {"glGenTextures", (uintptr_t)&glGenTextures},
    {"glGetFloatv", (uintptr_t)&glGetFloatv},
    {"glGetIntegerv", (uintptr_t)&glGetIntegerv},
    {"glHint", (uintptr_t)&glHint},
    {"glLightModelf", (uintptr_t)&glLightModelf},
    {"glLightfv", (uintptr_t)&glLightfv},
    {"glLoadIdentity", (uintptr_t)&glLoadIdentity},
    {"glLoadMatrixf", (uintptr_t)&glLoadMatrixf},
    {"glMaterialf", WRAP(glMaterialf)},
    {"glMaterialfv", (uintptr_t)&glMaterialfv},
    {"glMatrixMode", (uintptr_t)&glMatrixMode},
    {"glMultMatrixf", (uintptr_t)&glMultMatrixf},
    {"glNormalPointer", (uintptr_t)&glNormalPointer},
    {"glOrthof", (uintptr_t)&glOrthofHook},
    {"glPopMatrix", (uintptr_t)&glPopMatrix},
    {"glPushMatrix", (uintptr_t)&glPushMatrix},
    {"glRotatef", WRAP(glRotatef)},
    {"glScalef", WRAP(glScalef)},
    {"glScissor", (uintptr_t)&glScissor},
    {"glShadeModel", (uintptr_t)&glShadeModel},
    {"glTexCoordPointer", (uintptr_t)&glTexCoordPointer},
    {"glTexEnvf", WRAP(glTexEnvf)},
    {"glTexImage2D", (uintptr_t)&glTexImage2DHook},
    {"glTexParameterf", (uintptr_t)&glTexParameterfHook},
    {"glTexParameteri", (uintptr_t)&glTexParameteriHook},
    {"glTexSubImage2D", (uintptr_t)&glTexSubImage2DHook},
    {"glTranslatef", WRAP(glTranslatef)},
    {"glVertexPointer", (uintptr_t)&glVertexPointer},
    {"glViewport", (uintptr_t)&glViewportHook},

    /* zlib */
    {"adler32", (uintptr_t)&adler32},
    {"crc32", (uintptr_t)&crc32},
    {"deflate", (uintptr_t)&deflate},
    {"deflateEnd", (uintptr_t)&deflateEnd},
    {"deflateInit2_", (uintptr_t)&deflateInit2_},
    {"deflateReset", (uintptr_t)&deflateReset},
    {"inflate", (uintptr_t)&inflate},
    {"inflateEnd", (uintptr_t)&inflateEnd},
    {"inflateInit2_", (uintptr_t)&inflateInit2_},
    {"inflateInit_", (uintptr_t)&inflateInit_},
    {"inflateReset", (uintptr_t)&inflateReset},

    /* runtime support */
    {"__aeabi_atexit", (uintptr_t)&__aeabi_atexit},
    {"__android_log_print", (uintptr_t)&__android_log_print},
    {"__assert2", (uintptr_t)&__assert2},
    {"__cxa_atexit", (uintptr_t)&__cxa_atexit},
    {"__cxa_finalize", (uintptr_t)&__cxa_finalize},
    {"__errno", (uintptr_t)&__errno},
    {"__gnu_Unwind_Find_exidx", (uintptr_t)&__gnu_Unwind_Find_exidx},
    {"__sF", (uintptr_t)&__sF_fake},
    {"__stack_chk_fail", (uintptr_t)&__stack_chk_fail},
    {"__stack_chk_guard", (uintptr_t)&__stack_chk_guard_fake},
    {"_toupper_tab_", (uintptr_t)&_toupper_tab_},

    /* libc */
    {"abort", (uintptr_t)&abort},
    {"acosf", WRAP(acosf)},
    {"atan2f", WRAP(atan2f)},
    {"atoi", (uintptr_t)&atoi},
    {"calloc", (uintptr_t)&calloc},
    {"ceilf", WRAP(ceilf)},
    {"clock_gettime", (uintptr_t)&clock_gettime},
    {"close", (uintptr_t)&close},
    {"cos", WRAP((double (*)(double))cos)},
    {"cosf", WRAP(cosf)},
    {"dlclose", (uintptr_t)&ret0},
    /* Returning NULL keeps InputDriverAndroid on its built-in fallback instead
     * of trying to call AMotionEvent_getAxisValue through a bogus pointer. */
    {"dlopen", (uintptr_t)&retNULL},
    {"dlsym", (uintptr_t)&retNULL},
    {"fclose", (uintptr_t)&fclose},
    {"feof", (uintptr_t)&feof},
    {"fflush", (uintptr_t)&fflush_hook},
    {"floor", WRAP((double (*)(double))floor)},
    {"fopen", (uintptr_t)&fopen_mark},
    {"fprintf", (uintptr_t)&fprintf_hook},
    {"fputc", (uintptr_t)&fputc_hook},
    {"fread", (uintptr_t)&fread_mark},
    {"free", (uintptr_t)&free},
    {"frexp", WRAP((double (*)(double, int *))frexp)},
    {"fseek", (uintptr_t)&fseek_mark},
    {"ftell", (uintptr_t)&ftell},
    {"fwrite", (uintptr_t)&fwrite},
    {"gettimeofday", (uintptr_t)&gettimeofday},
    {"gmtime", (uintptr_t)&gmtime},
    {"longjmp", (uintptr_t)&longjmp},
    {"lrand48", (uintptr_t)&lrand48},
    {"malloc", (uintptr_t)&malloc},
    {"memchr", (uintptr_t)&memchr},
    {"memcmp", (uintptr_t)&memcmp},
    {"memcpy", (uintptr_t)&memcpy},
    {"memmove", (uintptr_t)&memmove},
    {"memset", (uintptr_t)&memset},
    {"mmap", (uintptr_t)&mmap_fake},
    {"modf", WRAP((double (*)(double, double *))modf)},
    {"munmap", (uintptr_t)&munmap_fake},
    {"nanosleep", (uintptr_t)&nanosleep_fake},
    {"open", (uintptr_t)&open_fake},
    {"pipe", (uintptr_t)&ret0},
    {"pow", WRAP((double (*)(double, double))pow)},
    {"powf", WRAP(powf)},
    {"puts", (uintptr_t)&puts_hook},
    {"qsort", (uintptr_t)&qsort},
    {"raise", (uintptr_t)&raise},
    {"read", (uintptr_t)&read},
    {"realloc", (uintptr_t)&realloc},
    {"remove", (uintptr_t)&remove},
    {"setjmp", (uintptr_t)&setjmp},
    {"sin", WRAP((double (*)(double))sin)},
    {"sinf", WRAP(sinf)},
    {"sleep", (uintptr_t)&sleep_fake},
    {"snprintf", (uintptr_t)&snprintf},
    {"sprintf", (uintptr_t)&sprintf},
    {"sqrt", WRAP((double (*)(double))sqrt)},
    {"sqrtf", WRAP(sqrtf)},
    {"srand48", (uintptr_t)&srand48},
    {"strcat", (uintptr_t)&strcat},
    {"strchr", (uintptr_t)&strchr},
    {"strcmp", (uintptr_t)&strcmp},
    {"strcpy", (uintptr_t)&strcpy},
    {"strdup", (uintptr_t)&strdup},
    {"strerror", (uintptr_t)&strerror},
    {"strlen", (uintptr_t)&strlen},
    {"strncasecmp", (uintptr_t)&strncasecmp},
    {"strncmp", (uintptr_t)&strncmp},
    {"strncpy", (uintptr_t)&strncpy},
    {"strstr", (uintptr_t)&strstr},
    {"strtod", WRAP(strtod)},
    {"strtok", (uintptr_t)&strtok},
    {"strtol", (uintptr_t)&strtol},
    {"tanf", WRAP(tanf)},
    {"time", (uintptr_t)&time},
    {"usleep", (uintptr_t)&usleep_mark},
    {"vsprintf", (uintptr_t)&vsprintf},
    {"write", (uintptr_t)&write},

    /* pthread */
    {"pthread_attr_init", (uintptr_t)&pthread_attr_init_fake},
    {"pthread_attr_setdetachstate",
     (uintptr_t)&pthread_attr_setdetachstate_fake},
    {"pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_fake},
    {"pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_fake},
    {"pthread_cond_init", (uintptr_t)&pthread_cond_init_fake},
    {"pthread_cond_signal", (uintptr_t)&pthread_cond_signal_fake},
    {"pthread_cond_wait", (uintptr_t)&cond_wait_mark},
    {"pthread_create", (uintptr_t)&pthread_create_fake},
    {"pthread_detach", (uintptr_t)&pthread_detach},
    {"pthread_equal", (uintptr_t)&pthread_equal},
    {"pthread_exit", (uintptr_t)&pthread_exit},
    {"pthread_getspecific", (uintptr_t)&pthread_getspecific},
    {"pthread_join", (uintptr_t)&pthread_join},
    {"pthread_key_create", (uintptr_t)&pthread_key_create},
    {"pthread_key_delete", (uintptr_t)&pthread_key_delete},
    {"pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_fake},
    {"pthread_mutex_init", (uintptr_t)&pthread_mutex_init_fake},
    {"pthread_mutex_lock", (uintptr_t)&mutex_lock_mark},
    {"pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_fake},
    {"pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_fake},
    {"pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_fake},
    {"pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_fake},
    {"pthread_self", (uintptr_t)&pthread_self},
    {"pthread_setspecific", (uintptr_t)&pthread_setspecific},
};

/* ==========================================================================
 * Engine entry points
 * ======================================================================== */

static int (*ff6_JNI_OnLoad)(void *vm, void *reserved);
static void (*ff6_initMainFrame)(void);
static void (*ff6_File_SetAAssetManager)(void *mgr);
static void (*ff6_initGame)(void);
static int (*ff6_InputDriverAndroid_onInputEvent)(void *event);
static void (*ff6_inputMotion)(void *app, void *event);
static void (*ff6_inputGamePadSetActive)(int active);

/* Read-only state probes: all of these are tiny exported accessors, so we can
 * poll the engine's own idea of what it is doing without patching anything. */
static int (*ff6_IsFadeSmartPhone)(void);
static int (*ff6_getFieldMapNo)(void);
static int (*ff6_MotionPointFlag)(int);

/* The AGB side keeps its whole state machine in named globals, and AgbIo is the
 * emulated GBA I/O block -- its first halfword is DISPCNT, whose bit 7 is
 * forced blank. That single bit is the difference between "drawing nothing"
 * and "the hardware is told to show black". */
static uint8_t *ff6_system_mode;
static uint8_t *ff6_maincnt;
static uint8_t *ff6_loadhaltflag;
static uint8_t *ff6_ishaltdata;
static uint8_t *ff6_loaded_flag;
static uint8_t *ff6_opening_loop;
static uint8_t *ff6_Bg0Disp;
static uint16_t *ff6_AgbIo;

static void resolve_symbols(void) {
  g_app = (uint8_t *)so_symbol(&ff6_mod, "g_app");

  ff6_JNI_OnLoad =
      (int (*)(void *, void *))so_symbol(&ff6_mod, "JNI_OnLoad");
  ff6_initMainFrame = (void (*)(void))so_symbol(&ff6_mod, "initMainFrame");
  ff6_File_SetAAssetManager =
      (void (*)(void *))so_symbol(&ff6_mod, "File_SetAAssetManager");
  ff6_initGame = (void (*)(void))so_symbol(&ff6_mod, "initGame");
  ff6_InputDriverAndroid_onInputEvent =
      (int (*)(void *))so_symbol(&ff6_mod, "InputDriverAndroid_onInputEvent");
  ff6_inputMotion =
      (void (*)(void *, void *))so_symbol(&ff6_mod, "inputMotion");
  ff6_inputGamePadSetActive =
      (void (*)(int))so_symbol(&ff6_mod, "inputGamePadSetActive");

  ff6_IsFadeSmartPhone =
      (int (*)(void))so_symbol(&ff6_mod, "IsFadeSmartPhone");
  ff6_getFieldMapNo = (int (*)(void))so_symbol(&ff6_mod, "getFieldMapNo");
  ff6_MotionPointFlag = (int (*)(int))so_symbol(&ff6_mod, "MotionPointFlag");

  ff6_system_mode = (uint8_t *)so_symbol(&ff6_mod, "system_mode");
  ff6_maincnt = (uint8_t *)so_symbol(&ff6_mod, "maincnt");
  ff6_loadhaltflag = (uint8_t *)so_symbol(&ff6_mod, "loadhaltflag");
  ff6_ishaltdata = (uint8_t *)so_symbol(&ff6_mod, "ishaltdata");
  ff6_loaded_flag = (uint8_t *)so_symbol(&ff6_mod, "__loaded_flag");
  ff6_opening_loop = (uint8_t *)so_symbol(&ff6_mod, "opening_loop");
  ff6_Bg0Disp = (uint8_t *)so_symbol(&ff6_mod, "Bg0Disp");
  ff6_AgbIo = (uint16_t *)so_symbol(&ff6_mod, "AgbIo");

  if (!g_app || !ff6_initMainFrame || !ff6_initGame ||
      !ff6_File_SetAAssetManager || !ff6_InputDriverAndroid_onInputEvent ||
      !ff6_inputMotion)
    fatal_error("Error could not resolve libFF6.so entry points.");
}

/* AgbMain gates the boot on ReadSaveDataResult():
 *
 *   if (ReadSaveDataResult() <= 1) goto SquEni;     // normal boot
 *   InitSaveDataDamage();
 *   ... 3/4/5 pick a message, anything else picks none ...
 *   do { vsyncProc(); } while (!UpdateSaveDataDamage());
 *
 * With no save file the read falls back to parsing a zeroed buffer, which
 * yields 2 -- inside the wait loop but with no message set, so the game sits
 * on a black screen waiting for a tap on a dialog that was never drawn. There
 * is genuinely no save data here, so answer "read fine" and let it boot; the
 * game writes a real save the first time the player saves. */
static int ReadSaveDataResultHook(void) { return 1; }

static void patch_game(void) {
  hook_addr(so_symbol(&ff6_mod, "ReadSaveDataResult"),
            (uintptr_t)&ReadSaveDataResultHook);

  if (options.aspect) {
    /* DeviceIsGen5() together with !SysConfigIsWide() makes the engine render
     * at the full screen size instead of the pillarboxed 3:2 box that matches
     * the GBA's 240x160 output. */
    hook_addr(so_symbol(&ff6_mod, "DeviceIsGen5"), (uintptr_t)&ret1);
    hook_addr(so_symbol(&ff6_mod, "SysConfigIsWide"), (uintptr_t)&ret0);
  }
}

/* ==========================================================================
 * Input pump
 * ======================================================================== */

typedef struct {
  unsigned int mask;
  int keycode;
} ButtonMapping;

/* Standard Android keycodes: the engine's own driver turns these into its
 * internal bitmask and then into AGB KEYINPUT bits, so the in-game button
 * config keeps working. */
static const ButtonMapping button_map[] = {
    {SCE_CTRL_CROSS, 96},     /* AKEYCODE_BUTTON_A */
    {SCE_CTRL_CIRCLE, 97},    /* AKEYCODE_BUTTON_B */
    {SCE_CTRL_SQUARE, 99},    /* AKEYCODE_BUTTON_X */
    {SCE_CTRL_TRIANGLE, 100}, /* AKEYCODE_BUTTON_Y */
    {SCE_CTRL_L1, 102},       /* AKEYCODE_BUTTON_L1 */
    {SCE_CTRL_R1, 103},       /* AKEYCODE_BUTTON_R1 */
    {SCE_CTRL_START, 108},    /* AKEYCODE_BUTTON_START */
    {SCE_CTRL_SELECT, 109},   /* AKEYCODE_BUTTON_SELECT */
    {SCE_CTRL_UP, 19},        /* AKEYCODE_DPAD_UP */
    {SCE_CTRL_DOWN, 20},      /* AKEYCODE_DPAD_DOWN */
    {SCE_CTRL_LEFT, 21},      /* AKEYCODE_DPAD_LEFT */
    {SCE_CTRL_RIGHT, 22},     /* AKEYCODE_DPAD_RIGHT */
};

#define ANALOG_DEADZONE 64

static void send_key(int keycode, int down) {
  fake_input_event event;
  memset(&event, 0, sizeof(event));

  event.type = AINPUT_EVENT_TYPE_KEY;
  event.source = AINPUT_SOURCE_GAMEPAD;
  event.action = down ? AKEY_EVENT_ACTION_DOWN : AKEY_EVENT_ACTION_UP;
  event.keycode = keycode;

  ff6_InputDriverAndroid_onInputEvent(&event);
}

static void pump_buttons(void) {
  static unsigned int previous = 0;

  SceCtrlData pad;
  sceCtrlPeekBufferPositiveExt2(0, &pad, 1);

  unsigned int buttons = pad.buttons;

  /* Fold the left stick into the d-pad so both drive the same events. */
  if (pad.ly < 128 - ANALOG_DEADZONE)
    buttons |= SCE_CTRL_UP;
  if (pad.ly > 128 + ANALOG_DEADZONE)
    buttons |= SCE_CTRL_DOWN;
  if (pad.lx < 128 - ANALOG_DEADZONE)
    buttons |= SCE_CTRL_LEFT;
  if (pad.lx > 128 + ANALOG_DEADZONE)
    buttons |= SCE_CTRL_RIGHT;

  unsigned int changed = buttons ^ previous;
  if (changed) {
    for (size_t i = 0; i < sizeof(button_map) / sizeof(button_map[0]); i++) {
      if (changed & button_map[i].mask)
        send_key(button_map[i].keycode, (buttons & button_map[i].mask) != 0);
    }
    previous = buttons;
  }
}

static void pump_touch(void) {
  static fake_input_event previous = {};

  SceTouchData touch;
  sceTouchPeek(SCE_TOUCH_PORT_FRONT, &touch, 1);

  int count = touch.reportNum > MAX_TOUCH_POINTS ? MAX_TOUCH_POINTS
                                                 : touch.reportNum;

  if (count == 0 && previous.pointer_count == 0)
    return;

  fake_input_event event;
  memset(&event, 0, sizeof(event));

  event.type = AINPUT_EVENT_TYPE_MOTION;
  event.source = AINPUT_SOURCE_TOUCHSCREEN;

  if (count == 0) {
    /* A release still has to carry the coordinates the finger lifted from --
     * that is where the game registers the tap. */
    event = previous;
    event.action = AMOTION_EVENT_ACTION_UP;
  } else {
    event.action = previous.pointer_count == 0 ? AMOTION_EVENT_ACTION_DOWN
                                               : AMOTION_EVENT_ACTION_MOVE;
    event.pointer_count = count;

    for (int i = 0; i < count; i++) {
      /* The front panel reports 1920x1088; the engine works in screen pixels. */
      event.x[i] = (float)touch.report[i].x * SCREEN_W / 1920.0f;
      event.y[i] = (float)touch.report[i].y * SCREEN_H / 1088.0f;
    }
  }

  ff6_inputMotion(&g_android_app, &event);

  previous = event;
  previous.pointer_count = count;
}

/* ==========================================================================
 * Boot
 * ======================================================================== */

static int main_thread(SceSize args, void *argp) {
  SceGxmMultisampleMode msaa;

  switch (options.msaa) {
  case 0:
    msaa = SCE_GXM_MULTISAMPLE_NONE;
    break;
  case 1:
    msaa = SCE_GXM_MULTISAMPLE_2X;
    break;
  default:
    msaa = SCE_GXM_MULTISAMPLE_4X;
    break;
  }

  /* The high-resolution battle renderer is the only path that uses the depth
   * buffer, so vitaGL has to be brought up with one or combats break while the
   * field looks fine. */
  trace("[ff6] vglInitExtended...\n");
  vglInitExtended(0, SCREEN_W, SCREEN_H,
                  MEMORY_VITAGL_THRESHOLD_MB * 1024 * 1024, msaa);

  trace("[ff6] resolve_symbols...\n");
  resolve_symbols();
  trace("[ff6] g_app=%p initGame=%p\n", g_app, (void *)ff6_initGame);

  /* Stand in for what ANativeActivity_onCreate + the glue would have set up. */
  memset(&g_activity, 0, sizeof(g_activity));
  g_activity.vm = fake_vm;
  g_activity.env = fake_env;
  g_activity.clazz = (void *)0x41414141;
  g_activity.internalDataPath = DATA_PATH;
  g_activity.externalDataPath = DATA_PATH;
  g_activity.sdkVersion = 19;
  g_activity.assetManager = &g_activity;
  g_activity.obbPath = OBB_PATH;

  memset(&g_android_app, 0, sizeof(g_android_app));
  g_android_app.activity = &g_activity;
  /* engine_handle_cmd gates APP_CMD_INIT_WINDOW on app->window; we call
   * initGame() ourselves, but initDisplay() still reads the field. */
  g_android_app.window = (void *)0xA9100001;

  if (ff6_JNI_OnLoad)
    ff6_JNI_OnLoad(fake_vm, NULL);

  /* This is android_main() minus the looper. Order matters: initMainFrame()
   * memsets the whole 224-byte g_app, so the app pointer has to go in after. */
  trace("[ff6] initMainFrame...\n");
  ff6_initMainFrame();
  trace("[ff6] File_SetAAssetManager...\n");
  ff6_File_SetAAssetManager(&g_activity);

  /* android_main() wires these two at each other; inputMotion() reaches the
   * engine through app->userData, so leaving it NULL silently swallows every
   * touch event. */
  *(void **)(g_app + ENGINE_APP) = &g_android_app;
  g_android_app.userData = g_app;
  /* initDisplay() overwrites these from eglQuerySurface, but anything that
   * peeks at the screen size before then should still see the truth. */
  *engine_field(ENGINE_SCREEN_W) = SCREEN_W;
  *engine_field(ENGINE_SCREEN_H) = SCREEN_H;

  trace("[ff6] got[usleep]=0x%x before initGame\n",
        *(unsigned int *)(ff6_mod.text_base + 0x1513ef8));

  /* initGame() spawns thread_main, which owns EGL/GL and runs AgbMain. */
  trace("[ff6] initGame...\n");
  ff6_initGame();
  trace("[ff6] initGame done, entering input loop\n");

  if (ff6_inputGamePadSetActive)
    ff6_inputGamePadSetActive(1);

  /* Same job the glue's main thread had: feed input, nothing else. */
  for (int tick = 0;; tick++) {
    pump_touch();
    pump_buttons();
    {
      static unsigned seen_frames;
      static int stalled_for;

      if (gl_stats.frames != seen_frames) {
        seen_frames = gl_stats.frames;
        stalled_for = 0;
      } else if (++stalled_for == 750) { /* 3 s without a swap */
        trace("[watchdog] no swap for 3s at frame %u; last shim=%s arg=0x%x\n",
              gl_stats.frames, (const char *)last_shim, last_shim_arg);
      }
    }

    if (tick % 250 == 0) {
      unsigned dispcnt = ff6_AgbIo ? ff6_AgbIo[0] : 0;
      trace("[state] mode=%d maincnt=%d touchflag=0x%x loadhalt=%d ishalt=%d "
            "loaded=%d openloop=%d bg0=%d fade=%d map=%d DISPCNT=0x%04x%s\n",
            ff6_system_mode ? *ff6_system_mode : -1,
            ff6_maincnt ? *ff6_maincnt : -1,
            ff6_MotionPointFlag ? ff6_MotionPointFlag(0) : -1,
            ff6_loadhaltflag ? *ff6_loadhaltflag : -1,
            ff6_ishaltdata ? *ff6_ishaltdata : -1,
            ff6_loaded_flag ? *ff6_loaded_flag : -1,
            ff6_opening_loop ? *ff6_opening_loop : -1,
            ff6_Bg0Disp ? *ff6_Bg0Disp : -1,
            ff6_IsFadeSmartPhone ? ff6_IsFadeSmartPhone() : -1,
            ff6_getFieldMapNo ? ff6_getFieldMapNo() : -1, dispcnt,
            (dispcnt & 0x80) ? " FORCED-BLANK" : "");
    }
    sceKernelDelayThread(4000);
  }

  return 0;
}

static int check_kubridge(void) {
  int search_unk[2];
  return _vshKernelSearchModuleByName("kubridge", search_unk);
}

static int file_exists(const char *path) {
  SceIoStat stat;
  return sceIoGetstat(path, &stat) >= 0;
}

int main(int argc, char *argv[]) {
  SceAppUtilInitParam initParam{};
  SceAppUtilBootParam bootParam{};
  sceAppUtilInit(&initParam, &bootParam);

  loadOptions();

  sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG_WIDE);
  sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
                           SCE_TOUCH_SAMPLING_STATE_START);

  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);

  if (check_kubridge() < 0)
    fatal_error("Error kubridge.skprx is not installed.");

  if (!file_exists("ur0:/data/libshacccg.suprx") &&
      !file_exists("ur0:/data/external/libshacccg.suprx"))
    fatal_error("Error libshacccg.suprx is not installed.");

  if (!file_exists(SO_PATH))
    fatal_error("Error could not find %s.", SO_PATH);

  if (!file_exists(OBB_PATH))
    fatal_error("Error could not find %s.", OBB_PATH);

  InitJNIEnv();
  InitJavaVM();
  find_exidx_bounds(SO_PATH);
  trace("[ff6] exidx vaddr=0x%x size=0x%x\n", (unsigned)exidx_vaddr,
         (unsigned)exidx_size);

  trace("[ff6] so_load...\n");
  if (so_load(&ff6_mod, SO_PATH, LOAD_ADDRESS) < 0)
    fatal_error("Error could not load %s.", SO_PATH);
  trace("[ff6] loaded text=0x%x (%u) data=0x%x (%u)\n",
         (unsigned)ff6_mod.text_base, (unsigned)ff6_mod.text_size,
         (unsigned)ff6_mod.data_base, (unsigned)ff6_mod.data_size);

  trace("[ff6] so_relocate...\n");
  so_relocate(&ff6_mod);
  trace("[ff6] so_resolve...\n");
  so_resolve(&ff6_mod, default_dynlib, sizeof(default_dynlib), 0);
  trace("[ff6] patch_game...\n");
  patch_game();
  so_flush_caches(&ff6_mod);

  trace("[ff6] so_initialize...\n");
  so_initialize(&ff6_mod);
  trace("[ff6] module ready\n");

  SceUID thid =
      sceKernelCreateThread("ff6_main", (SceKernelThreadEntry)main_thread, 0x40,
                            1024 * 1024, 0, 0, NULL);
  sceKernelStartThread(thid, 0, NULL);

  return sceKernelExitDeleteThread(0);
}
