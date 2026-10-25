#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include "../loader/file_compat.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void) {
  char dir[] = "/tmp/ff6-file-compat-XXXXXX";
  assert(mkdtemp(dir));
  char path[256], name[64], data[8];
  const char *translated;
  const char *native = "ux0:data/ff6/RockDat3.sav";
  assert(ff6_translate_path(native, dir, path, sizeof(path)) == native);

  /* Exercise the original Android flags against real host file operations. */
  for (int slot = 0; slot <= 3; slot += 3) {
    snprintf(name, sizeof(name), "/sdcard/RockDat%d.sav", slot);
    translated = ff6_translate_path(name, dir, path, sizeof(path));
    assert(translated);
    int fd = open(translated, ff6_translate_open_flags(2 | BIONIC_O_CREAT), 0600);
    assert(fd >= 0);
    assert(write(fd, "save", 4) == 4);
    assert(close(fd) == 0);

    fd = open(translated, ff6_translate_open_flags(1 | BIONIC_O_APPEND));
    assert(fd >= 0);
    assert(write(fd, "!", 1) == 1);
    assert(close(fd) == 0);

    fd = open(translated, ff6_translate_open_flags(BIONIC_O_CREAT | BIONIC_O_EXCL), 0600);
    assert(fd == -1 && errno == EEXIST);
    fd = open(translated, ff6_translate_open_flags(0));
    assert(fd >= 0);
    memset(data, 0, sizeof(data));
    assert(read(fd, data, sizeof(data)) == 5);
    assert(strcmp(data, "save!") == 0);
    assert(close(fd) == 0);

    fd = open(translated, ff6_translate_open_flags(1 | BIONIC_O_TRUNC));
    assert(fd >= 0);
    assert(close(fd) == 0);
    struct stat info;
    assert(stat(translated, &info) == 0 && info.st_size == 0);
    assert(remove(translated) == 0);
  }

  struct { char small[5]; char guard; } bounded = {{0}, 'X'};
  errno = 0;
  assert(ff6_translate_path("/sdcard/RockDat0.sav", dir, bounded.small,
                            sizeof(bounded.small)) == NULL);
  assert(errno == ENAMETOOLONG && bounded.guard == 'X');
  errno = 0;
  assert(ff6_translate_path(NULL, dir, path, sizeof(path)) == NULL);
  assert(errno == EINVAL);
  assert((ff6_translate_open_flags(BIONIC_O_NONBLOCK) & O_NONBLOCK) != 0);
  assert(rmdir(dir) == 0);
  puts("file compatibility tests passed");
  return 0;
}
