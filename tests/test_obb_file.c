#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "../loader/obb_file.h"
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static int opens, closes, fail_reads;

static int host_open(const char *path) {
  opens++;
  return open(path, O_RDONLY);
}
static int host_pread(int fd, void *dst, unsigned size, long long offset) {
  if (fail_reads > 0) {
    fail_reads--;
    return -1;
  }
  /* A short read must not look like the end of the archive. */
  return (int)pread(fd, dst, size > 4099 ? 4099 : size, (off_t)offset);
}
static long long host_size(int fd) {
  struct stat st;
  return fstat(fd, &st) ? -1 : (long long)st.st_size;
}
static void host_close(int fd) {
  closes++;
  close(fd);
}

static const ff6_obb_io host_io = {host_open, host_pread, host_size, host_close};

int main(void) {
  char dir[] = "/tmp/ff6-obb-XXXXXX";
  assert(mkdtemp(dir));
  char path[256], other[256];
  snprintf(path, sizeof(path), "%s/main.obb", dir);
  snprintf(other, sizeof(other), "%s/other.obb", dir);
  enum { SIZE = 300000 };
  unsigned char *data = malloc(SIZE), *a = malloc(SIZE), *b = malloc(SIZE);
  assert(data && a && b);
  srand(6);
  for (int i = 0; i < SIZE; i++)
    data[i] = (unsigned char)rand();
  FILE *f = fopen(path, "wb");
  assert(f && fwrite(data, 1, SIZE, f) == SIZE && !fclose(f));

  ff6_obb_reader reader;
  memset(&reader, 0, sizeof(reader));
  reader.io = &host_io;
  reader.path = path;

  /* Only reads of this one path are ours. */
  assert(!ff6_obb_open(&reader, other, "rb"));
  assert(!ff6_obb_open(&reader, path, "wb"));
  assert(!ff6_obb_open(&reader, path, "r+b"));
  assert(!ff6_obb_open(&reader, NULL, "rb"));
  assert(opens == 0);

  /* Same answers as stdio through a sequence shaped like the engine's. */
  f = fopen(path, "rb");
  assert(f);
  ff6_obb_handle *h = ff6_obb_open(&reader, path, "rb");
  assert(h && ff6_obb_is_handle(&reader, h) && !ff6_obb_is_handle(&reader, f));
  for (int i = 0; i < 4000; i++) {
    long off = rand() % (SIZE + 100);
    int whence = rand() % 3;
    long arg = whence == SEEK_SET ? off : whence == SEEK_CUR ? rand() % 2000 : -(rand() % 2000);
    if (whence == SEEK_CUR && h->pos + arg < 0)
      arg = 0;
    assert(ff6_obb_seek(&reader, h, arg, whence) == fseek(f, arg, whence));
    assert(h->pos == ftell(f));
    size_t size = 1 + (size_t)(rand() % 3), count = (size_t)(rand() % 9000);
    size_t got = ff6_obb_read(&reader, h, a, size, count);
    assert(got == fread(b, size, count, f));
    assert(!memcmp(a, b, got * size));
    assert(h->pos == ftell(f));
    assert(!!h->eof == !!feof(f));
    assert(!h->err);
  }
  fclose(f);

  /* Arguments that must not move the position. */
  long long pos = h->pos;
  assert(ff6_obb_seek(&reader, h, -1, SEEK_SET) == -1 && errno == EINVAL);
  assert(ff6_obb_seek(&reader, h, 0, 99) == -1 && errno == EINVAL);
  assert(h->pos == pos);
  assert(ff6_obb_read(&reader, h, a, 0, 5) == 0);
  assert(ff6_obb_read(&reader, h, a, 5, 0) == 0);

  /* A read crosses the end: whole items only, then EOF, cleared by a seek. */
  assert(!ff6_obb_seek(&reader, h, -5, SEEK_END));
  assert(ff6_obb_read(&reader, h, a, 2, 10) == 2 && h->eof);
  assert(!memcmp(a, data + SIZE - 5, 4));
  assert(!ff6_obb_seek(&reader, h, 0, SEEK_SET) && !h->eof);

  /* Every handle shares one descriptor, and each keeps its own position. */
  ff6_obb_handle *second = ff6_obb_open(&reader, path, "r");
  assert(second && second != h && opens == 1);
  assert(!ff6_obb_seek(&reader, second, 1000, SEEK_SET));
  assert(ff6_obb_read(&reader, second, a, 1, 16) == 16 && !memcmp(a, data + 1000, 16));
  assert(h->pos == 0 && second->pos == 1016);
  assert(!ff6_obb_close(&reader, second));

  /* The pool is finite: running out falls back rather than failing the open,
   * and a closed slot comes back clean. */
  ff6_obb_handle *held[FF6_OBB_HANDLES];
  int count = 0;
  while (count < FF6_OBB_HANDLES && (held[count] = ff6_obb_open(&reader, path, "rb")))
    count++;
  assert(count == FF6_OBB_HANDLES - 1);
  assert(!ff6_obb_open(&reader, path, "rb"));
  held[0]->eof = 1;
  held[0]->pos = 77;
  assert(!ff6_obb_close(&reader, held[0]));
  held[0] = ff6_obb_open(&reader, path, "rb");
  assert(held[0] && !held[0]->eof && held[0]->pos == 0);
  for (int i = 0; i < count; i++)
    assert(!ff6_obb_close(&reader, held[i]));

  /* A descriptor that went stale over a suspend is reopened once... */
  fail_reads = 1;
  int opens_before = opens;
  assert(ff6_obb_read(&reader, h, a, 1, 64) == 64 && !memcmp(a, data, 64));
  assert(opens == opens_before + 1 && closes == 1 && !h->err);
  /* ...and a device that keeps failing is an error, not an endless retry. */
  fail_reads = 3;
  assert(ff6_obb_read(&reader, h, a, 1, 64) == 0 && h->err && !h->eof);
  fail_reads = 0;

  /* Without the archive the caller gets NULL and uses stdio's own error. */
  ff6_obb_reader missing;
  memset(&missing, 0, sizeof(missing));
  missing.io = &host_io;
  missing.path = other;
  assert(!ff6_obb_open(&missing, other, "rb"));

  assert(!unlink(path));
  assert(!rmdir(dir));
  free(data);
  free(a);
  free(b);
  return 0;
}
