#ifndef FF6_FILE_COMPAT_H
#define FF6_FILE_COMPAT_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#define BIONIC_O_CREAT 0x40
#define BIONIC_O_EXCL 0x80
#define BIONIC_O_TRUNC 0x200
#define BIONIC_O_APPEND 0x400
#define BIONIC_O_NONBLOCK 0x800

static inline int ff6_translate_open_flags(int flags) {
  int out = flags & O_ACCMODE;
  if (flags & BIONIC_O_CREAT) out |= O_CREAT;
  if (flags & BIONIC_O_EXCL) out |= O_EXCL;
  if (flags & BIONIC_O_TRUNC) out |= O_TRUNC;
  if (flags & BIONIC_O_APPEND) out |= O_APPEND;
  if (flags & BIONIC_O_NONBLOCK) out |= O_NONBLOCK;
  return out;
}

/* Never silently truncate a save path into the name of a different file. */
static inline const char *ff6_translate_path(const char *path,
                                            const char *data_path,
                                            char *buf, size_t capacity) {
  static const char prefix[] = "/sdcard/";
  if (!path) {
    errno = EINVAL;
    return NULL;
  }
  if (strncmp(path, prefix, sizeof(prefix) - 1) != 0)
    return path;
  int written = snprintf(buf, capacity, "%s/%s", data_path,
                         path + sizeof(prefix) - 1);
  if (written < 0 || (size_t)written >= capacity) {
    errno = ENAMETOOLONG;
    return NULL;
  }
  return buf;
}

#endif
