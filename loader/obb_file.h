#ifndef FF6_OBB_FILE_H
#define FF6_OBB_FILE_H

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Serves the game's .obb without stdio. The engine reopens the archive for every
 * file inside it and seeks four or five times per open; newlib's fseek answers
 * each unaligned seek outside its buffer by reading a whole block, and its fread
 * splits every read into buffer-sized device reads. Here a seek is arithmetic and
 * a read is one device read of exactly what was asked, on one shared descriptor.
 * Positional reads keep that descriptor safe to share between threads. */

#define FF6_OBB_HANDLES 32

typedef struct {
  int (*open)(const char *path);
  int (*pread)(int fd, void *dst, unsigned size, long long offset);
  long long (*size)(int fd);
  void (*close)(int fd);
} ff6_obb_io;

typedef struct {
  int used;
  int eof;
  int err;
  long long pos;
} ff6_obb_handle;

typedef struct {
  const ff6_obb_io *io;
  const char *path;
  int lock;
  int opened;
  int fd;
  long long size;
  ff6_obb_handle handles[FF6_OBB_HANDLES];
} ff6_obb_reader;

static inline void ff6_obb_lock(ff6_obb_reader *reader) {
  while (__atomic_exchange_n(&reader->lock, 1, __ATOMIC_ACQUIRE)) {}
}

static inline void ff6_obb_unlock(ff6_obb_reader *reader) {
  __atomic_store_n(&reader->lock, 0, __ATOMIC_RELEASE);
}

/* Call with the lock held. */
static inline int ff6_obb_connect(ff6_obb_reader *reader) {
  int fd = reader->io->open(reader->path);
  if (fd < 0)
    return 0;
  long long size = reader->io->size(fd);
  if (size < 0) {
    reader->io->close(fd);
    return 0;
  }
  reader->fd = fd;
  reader->size = size;
  reader->opened = 1;
  return 1;
}

/* NULL means "not ours or not possible": the caller falls back to plain stdio. */
static inline ff6_obb_handle *ff6_obb_open(ff6_obb_reader *reader,
                                           const char *path, const char *mode) {
  if (!path || !mode || mode[0] != 'r' || strchr(mode, '+') ||
      strcmp(path, reader->path) != 0)
    return NULL;
  ff6_obb_handle *handle = NULL;
  ff6_obb_lock(reader);
  if (reader->opened || ff6_obb_connect(reader)) {
    for (int i = 0; i < FF6_OBB_HANDLES && !handle; i++) {
      if (!reader->handles[i].used) {
        handle = &reader->handles[i];
        memset(handle, 0, sizeof(*handle));
        handle->used = 1;
      }
    }
  }
  ff6_obb_unlock(reader);
  return handle;
}

static inline int ff6_obb_is_handle(const ff6_obb_reader *reader,
                                    const void *stream) {
  return stream >= (const void *)reader->handles &&
         stream < (const void *)(reader->handles + FF6_OBB_HANDLES);
}

/* A descriptor does not survive every suspend, so one failed read reopens the
 * archive before giving up. Another thread may have reopened it already. */
static inline int ff6_obb_reconnect(ff6_obb_reader *reader, int stale_fd) {
  int connected = 1;
  ff6_obb_lock(reader);
  if (reader->fd == stale_fd) {
    reader->io->close(stale_fd);
    connected = ff6_obb_connect(reader);
    if (!connected)
      reader->opened = 0;
  }
  ff6_obb_unlock(reader);
  return connected;
}

static inline size_t ff6_obb_read(ff6_obb_reader *reader,
                                  ff6_obb_handle *handle, void *dst,
                                  size_t size, size_t count) {
  if (size == 0 || count == 0)
    return 0;
  size_t wanted = count > (size_t)-1 / size ? (size_t)-1 : size * count;
  size_t done = 0;
  int retried = 0;
  while (done < wanted) {
    if (handle->pos >= reader->size) {
      handle->eof = 1;
      break;
    }
    long long left = reader->size - handle->pos;
    size_t remaining = wanted - done;
    unsigned chunk = (unsigned)((long long)remaining < left ? (long long)remaining : left);
    int fd = reader->fd;
    int got = reader->opened
                  ? reader->io->pread(fd, (char *)dst + done, chunk, handle->pos)
                  : -1;
    if (got < 0 && !retried) {
      retried = 1;
      if (ff6_obb_reconnect(reader, fd))
        continue;
    }
    if (got <= 0) {
      handle->err = 1;
      errno = EIO;
      break;
    }
    done += (size_t)got;
    handle->pos += got;
  }
  return done / size;
}

static inline int ff6_obb_seek(ff6_obb_reader *reader, ff6_obb_handle *handle,
                               long offset, int whence) {
  long long target;
  switch (whence) {
  case SEEK_SET: target = offset; break;
  case SEEK_CUR: target = handle->pos + offset; break;
  case SEEK_END: target = reader->size + offset; break;
  default: target = -1; break;
  }
  if (target < 0) {
    errno = EINVAL;
    return -1;
  }
  handle->pos = target;
  handle->eof = 0;
  return 0;
}

static inline int ff6_obb_close(ff6_obb_reader *reader, ff6_obb_handle *handle) {
  ff6_obb_lock(reader);
  handle->used = 0;
  ff6_obb_unlock(reader);
  return 0;
}

#endif
