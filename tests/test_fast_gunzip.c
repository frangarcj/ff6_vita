#include "../loader/fast_gunzip.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <zlib.h>

static uint32_t rng_state = 0x12345678;
static uint32_t rng(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 17;
  rng_state ^= rng_state << 5;
  return rng_state;
}

static size_t gzip_with(const uint8_t *src, size_t size, uint8_t *dst, size_t cap, int level,
                        int strategy, int window, int mem) {
  z_stream z;
  memset(&z, 0, sizeof(z));
  assert(deflateInit2(&z, level, Z_DEFLATED, 16 + window, mem, strategy) == Z_OK);
  z.next_in = (Bytef *)src;
  z.avail_in = (uInt)size;
  z.next_out = dst;
  z.avail_out = (uInt)cap;
  assert(deflate(&z, Z_FINISH) == Z_STREAM_END);
  size_t out = z.total_out;
  deflateEnd(&z);
  return out;
}

/* What the engine's inflate would return: the reference. */
static int zlib_ok(const uint8_t *gz, size_t gz_size, uint8_t *out, size_t out_size) {
  z_stream z;
  memset(&z, 0, sizeof(z));
  assert(inflateInit2(&z, 31) == Z_OK);
  z.next_in = (Bytef *)gz;
  z.avail_in = (uInt)gz_size;
  z.next_out = out;
  z.avail_out = (uInt)out_size;
  int result = inflate(&z, Z_FINISH);
  inflateEnd(&z);
  return result == Z_STREAM_END && z.total_out == out_size;
}

static void fill(uint8_t *data, size_t size, int kind) {
  size_t i = 0;
  switch (kind) {
  case 0: /* Noise: stored blocks. */
    for (; i < size; i++)
      data[i] = (uint8_t)rng();
    break;
  case 1: /* One byte: distance 1. */
    memset(data, 0x5a, size);
    break;
  case 2: /* Short repeating patterns: every small distance. */
    while (i < size) {
      const size_t period = 1 + rng() % 24;
      const size_t run = 16 + rng() % 600;
      for (size_t j = 0; j < run && i < size; j++, i++)
        data[i] = j < period ? (uint8_t)rng() : data[i - period];
    }
    break;
  case 3: /* Far matches among noise: long distances, long codes. */
    while (i < size) {
      if (i > 4096 && rng() % 3 == 0) {
        const size_t dist = 1 + rng() % (i < 32768 ? i : 32768);
        const size_t len = 3 + rng() % 300;
        for (size_t j = 0; j < len && i < size; j++, i++)
          data[i] = data[i - dist];
      } else {
        const size_t run = 1 + rng() % 50;
        for (size_t j = 0; j < run && i < size; j++, i++)
          data[i] = (uint8_t)(rng() % (j % 2 ? 7 : 251));
      }
    }
    break;
  default: /* Skewed literals: very unbalanced codes, up to 15 bits. */
    for (; i < size; i++) {
      uint32_t r = rng();
      int zeros = __builtin_clz(r | 1);
      data[i] = (uint8_t)(zeros * 8 + (r & 7));
    }
    break;
  }
}

static void check_valid(const uint8_t *data, size_t size, int level, int strategy, int window,
                        int mem) {
  const size_t cap = size + size / 8 + 1024;
  uint8_t *gz = malloc(cap);
  uint8_t *out = malloc(size + 1);
  const size_t gz_size = gzip_with(data, size, gz, cap, level, strategy, window, mem);
  size_t used = 0;
  memset(out, 0xee, size + 1);
  assert(ff6_gunzip(gz, gz_size, out, size, &used));
  assert(used == gz_size);
  assert(!memcmp(out, data, size));
  /* Nothing past the output, whichever part of the decoder finished it. */
  assert(out[size] == 0xee);
  /* The stored size has to match what was asked for. */
  if (size) {
    assert(!ff6_gunzip(gz, gz_size, out, size - 1, NULL));
    assert(!ff6_gunzip(gz, gz_size, out, size + 1, NULL) || size == 0);
  }
  free(gz);
  free(out);
}

static void check_header_flags(void) {
  static const uint8_t data[] = "header flags: extra, name and comment are skipped";
  uint8_t gz[512], out[sizeof(data)];
  const size_t plain = gzip_with(data, sizeof(data), gz, sizeof(gz), 6, Z_DEFAULT_STRATEGY, 15, 8);

  uint8_t with[640];
  memcpy(with, gz, 10);
  with[3] = 4 | 8 | 16 | 2;
  size_t at = 10;
  with[at++] = 3;
  with[at++] = 0;
  memcpy(with + at, "abc", 3);
  at += 3;
  memcpy(with + at, "name", 5);
  at += 5;
  memcpy(with + at, "comment", 8);
  at += 8;
  with[at++] = 0;
  with[at++] = 0;
  memcpy(with + at, gz + 10, plain - 10);
  at += plain - 10;
  size_t used = 0;
  assert(ff6_gunzip(with, at, out, sizeof(data), &used));
  assert(used == at && !memcmp(out, data, sizeof(data)));

  /* Reserved flags, wrong method, too short. */
  memcpy(with, gz, plain);
  with[3] = 0x20;
  assert(!ff6_gunzip(with, plain, out, sizeof(data), NULL));
  with[3] = gz[3];
  with[2] = 7;
  assert(!ff6_gunzip(with, plain, out, sizeof(data), NULL));
  assert(!ff6_gunzip(gz, 17, out, sizeof(data), NULL));
}

/* Damaged input must never read or write out of bounds (the sanitizers watch that)
 * and must never claim success with something zlib would reject. */
static void check_damaged(const uint8_t *data, size_t size) {
  const size_t cap = size + size / 8 + 1024;
  uint8_t *gz = malloc(cap);
  const size_t gz_size = gzip_with(data, size, gz, cap, 6, Z_DEFAULT_STRATEGY, 15, 8);
  uint8_t *out = malloc(size);
  uint8_t *reference = malloc(size);
  for (int round = 0; round < 400; round++) {
    /* An exact heap block: one byte over is a sanitizer report. */
    uint8_t *bad = malloc(gz_size);
    memcpy(bad, gz, gz_size);
    size_t bad_size = gz_size;
    if (round % 4 == 3) {
      bad_size = rng() % gz_size;
    } else {
      const int flips = 1 + (int)(rng() % 4);
      for (int i = 0; i < flips; i++)
        bad[10 + rng() % (gz_size - 18)] ^= (uint8_t)(1u << (rng() % 8));
    }
    if (ff6_gunzip(bad, bad_size, out, size, NULL)) {
      /* Same size by luck: only the CRC, which isn't checked, could tell. What
       * zlib accepts has to be identical. */
      if (zlib_ok(bad, bad_size, reference, size))
        assert(!memcmp(out, reference, size));
    }
    free(bad);
  }
  free(gz);
  free(out);
  free(reference);
}

/* Sizes around the input and output safety margins, so every block ends in the
 * careful part at least once. */
static void check_tails(void) {
  uint8_t *data = malloc(2048);
  fill(data, 2048, 3);
  for (size_t size = 0; size <= 600; size++)
    check_valid(data, size, 6, Z_DEFAULT_STRATEGY, 15, 8);
  free(data);
}

int main(void) {
  const size_t sizes[] = {1, 2, 3, 15, 16, 17, 257, 258, 259, 4096, 32768, 65536, 300000};
  const int levels[] = {0, 1, 6, 9};
  for (int kind = 0; kind < 5; kind++) {
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
      uint8_t *data = malloc(sizes[s]);
      fill(data, sizes[s], kind);
      for (size_t l = 0; l < sizeof(levels) / sizeof(levels[0]); l++)
        check_valid(data, sizes[s], levels[l], Z_DEFAULT_STRATEGY, 15, 8);
      /* Fixed blocks, literal-only blocks with no distance code, small windows. */
      check_valid(data, sizes[s], 6, Z_FIXED, 15, 8);
      check_valid(data, sizes[s], 6, Z_HUFFMAN_ONLY, 15, 8);
      check_valid(data, sizes[s], 6, Z_RLE, 15, 8);
      check_valid(data, sizes[s], 9, Z_DEFAULT_STRATEGY, 9, 1);
      free(data);
    }
  }
  check_tails();
  check_header_flags();

  for (int kind = 1; kind < 5; kind++) {
    uint8_t *data = malloc(60000);
    fill(data, 60000, kind);
    check_damaged(data, 60000);
    free(data);
  }
  puts("fast_gunzip: ok");
  return 0;
}
