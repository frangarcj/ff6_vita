#ifndef FF6_FAST_GUNZIP_H
#define FF6_FAST_GUNZIP_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#endif

/* One-shot gzip decoder for the engine's .gz assets. Yat::Gzip hands zlib the
 * whole input and an output of exactly the stored size, so nothing has to survive
 * between calls: no window, no state machine, and no CRC of data that comes out of
 * a read-only archive. It refuses what it does not handle rather than guessing;
 * the caller then runs zlib on the same input, which gives the proper verdict.
 * Every read and write is bounds checked, whatever the input holds. */

enum {
  FF6_GZ_LIT_BITS = 10,
  FF6_GZ_DIST_BITS = 8,
  /* Largest two-level tables for these root sizes (zlib's `enough`). */
  FF6_GZ_LIT_ENTRIES = 1332,
  FF6_GZ_DIST_ENTRIES = 400,
};

/* Entry: symbol or base << 16 | extra << 8 | kind << 4 | code bits. */
enum {
  FF6_GZ_LITERAL = 0,
  FF6_GZ_LENGTH = 1,
  FF6_GZ_END = 2,
  FF6_GZ_SUBTABLE = 3,
  FF6_GZ_INVALID = 4,
};

#define FF6_GZ_ENTRY(value, extra, kind, bits)                                 \
  (((uint32_t)(value) << 16) | ((uint32_t)(extra) << 8) |                      \
   ((uint32_t)(kind) << 4) | (uint32_t)(bits))
#define FF6_GZ_BITS(entry) ((entry) & 15u)
#define FF6_GZ_KIND(entry) (((entry) >> 4) & 15u)
#define FF6_GZ_EXTRA(entry) (((entry) >> 8) & 255u)
#define FF6_GZ_VALUE(entry) ((entry) >> 16)

static inline uint32_t ff6_gz_reverse(uint32_t code, int bits) {
  uint32_t reversed = 0;
  while (bits-- > 0) {
    reversed = (reversed << 1) | (code & 1);
    code >>= 1;
  }
  return reversed;
}

/* Fills a table from canonical code lengths. Returns 0 on an over-subscribed set,
 * and on an incomplete one unless `sparse` allows a code with at most one symbol:
 * those leave every other entry invalid. `capacity` limits the entries written. */
static inline int ff6_gz_build(uint32_t *table, int capacity, int root,
                               const uint8_t *lengths, int count,
                               const uint32_t *entries, int sparse) {
  unsigned histogram[16] = {0};
  for (int i = 0; i < count; i++)
    histogram[lengths[i]]++;
  histogram[0] = 0;

  int left = 1, used = 0;
  for (int bits = 1; bits <= 15; bits++) {
    left = (left << 1) - (int)histogram[bits];
    if (left < 0)
      return 0;
    used += (int)histogram[bits];
  }
  if (left > 0 && !(sparse && (used == 0 || (used == 1 && histogram[1] == 1))))
    return 0;

  const int root_size = 1 << root;
  const uint32_t invalid = FF6_GZ_ENTRY(0, 0, FF6_GZ_INVALID, 1);
  for (int i = 0; i < root_size; i++)
    table[i] = invalid;

  unsigned next[16];
  unsigned code = 0;
  for (int bits = 1; bits <= 15; bits++) {
    code = (code + histogram[bits - 1]) << 1;
    next[bits] = code;
  }

  /* Short codes repeat through the root. */
  for (int symbol = 0; symbol < count; symbol++) {
    const int bits = lengths[symbol];
    if (!bits || bits > root)
      continue;
    const uint32_t reversed = ff6_gz_reverse(next[bits]++, bits);
    const uint32_t entry = entries[symbol] | (uint32_t)bits;
    for (int i = (int)reversed; i < root_size; i += 1 << bits)
      table[i] = entry;
  }

  /* Long codes share a subtable per root prefix. In canonical order a prefix's
   * codes are consecutive, so the codes still to come tell how wide it must be. */
  int remaining[16];
  for (int bits = 0; bits <= 15; bits++)
    remaining[bits] = (int)histogram[bits];
  int top = root_size;
  for (int bits = root + 1; bits <= 15; bits++) {
    for (int symbol = 0; symbol < count; symbol++) {
      if (lengths[symbol] != bits)
        continue;
      const uint32_t reversed = ff6_gz_reverse(next[bits]++, bits);
      const uint32_t prefix = reversed & ((1u << root) - 1);
      uint32_t link = table[prefix];
      if (FF6_GZ_KIND(link) != FF6_GZ_SUBTABLE) {
        int sub_bits = bits - root;
        int room = 1 << sub_bits;
        while (sub_bits + root < 15) {
          room -= remaining[sub_bits + root];
          if (room <= 0)
            break;
          sub_bits++;
          room <<= 1;
        }
        if (top + (1 << sub_bits) > capacity)
          return 0;
        for (int i = 0; i < (1 << sub_bits); i++)
          table[top + i] = invalid;
        link = FF6_GZ_ENTRY(top, sub_bits, FF6_GZ_SUBTABLE, root);
        table[prefix] = link;
        top += 1 << sub_bits;
      }
      remaining[bits]--;
      const int code_bits = bits - root;
      const int sub_bits = (int)FF6_GZ_EXTRA(link);
      if (code_bits > sub_bits)
        return 0;
      const uint32_t entry = entries[symbol] | (uint32_t)code_bits;
      uint32_t *sub = table + FF6_GZ_VALUE(link);
      for (int i = (int)(reversed >> root); i < (1 << sub_bits); i += 1 << code_bits)
        sub[i] = entry;
    }
  }
  return 1;
}

typedef struct {
  const uint8_t *in, *in_end;
  uint32_t bits;
  int count;
  int overrun; /* Zero bytes invented past the end of the input. */
} ff6_gz_reader;

/* Leaves at least 24 bits, or everything left followed by zeros. Never 32: the
 * fast loop's refill shifts by the count. */
static inline void ff6_gz_fill(ff6_gz_reader *r) {
  while (r->count < 24) {
    if (r->in < r->in_end)
      r->bits |= (uint32_t)*r->in++ << r->count;
    else
      r->overrun++;
    r->count += 8;
  }
}

static inline uint32_t ff6_gz_take(ff6_gz_reader *r, int n) {
  ff6_gz_fill(r);
  const uint32_t value = n ? r->bits & ((1u << n) - 1) : 0;
  r->bits >>= n;
  r->count -= n;
  return value;
}

/* Reads one code; the subtable step needs up to 15 bits, which fill guarantees. */
static inline uint32_t ff6_gz_decode(ff6_gz_reader *r, const uint32_t *table, int root) {
  ff6_gz_fill(r);
  uint32_t entry = table[r->bits & ((1u << root) - 1)];
  if (FF6_GZ_KIND(entry) == FF6_GZ_SUBTABLE) {
    r->bits >>= root;
    r->count -= root;
    entry = table[FF6_GZ_VALUE(entry) + (r->bits & ((1u << FF6_GZ_EXTRA(entry)) - 1))];
  }
  r->bits >>= FF6_GZ_BITS(entry);
  r->count -= (int)FF6_GZ_BITS(entry);
  return entry;
}

static inline void ff6_gz_copy(uint8_t *out, size_t dist, size_t len) {
  const uint8_t *from = out - dist;
#if defined(__ARM_NEON) || defined(__ARM_NEON__)
  if (dist >= 16) {
    for (size_t done = 0; done < len; done += 16)
      vst1q_u8(out + done, vld1q_u8(from + done));
    return;
  }
#else
  if (dist >= 16) {
    for (size_t done = 0; done < len; done += 16)
      memcpy(out + done, from + done, 16);
    return;
  }
#endif
  if (dist >= 8) {
    for (size_t done = 0; done < len; done += 8)
      memcpy(out + done, from + done, 8);
    return;
  }
  if (dist == 1) {
    memset(out, *from, len + 16);
    return;
  }
  for (size_t i = 0; i < len; i++)
    out[i] = from[i];
}

/* Room a copy may overrun past its length: the 16-byte steps and the memset. */
#define FF6_GZ_MARGIN (258 + 32)

static inline uint32_t ff6_gz_load32(const uint8_t *p) {
  uint32_t value;
  memcpy(&value, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  value = __builtin_bswap32(value);
#endif
  return value;
}

/* Decodes one compressed block's symbols. Returns 1 at its end, 0 on bad data. */
static int ff6_gz_symbols(ff6_gz_reader *r, const uint32_t *lit, const uint32_t *dist,
                          const uint8_t *out_begin, uint8_t **out_at,
                          const uint8_t *out_end) {
  static const uint16_t dist_base[32] = {
      1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,
      49,  65,  97,  129, 193, 257,  385,  513,  769,  1025, 1537,
      2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577, 0, 0};
  static const uint8_t dist_extra[32] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4,
                                         4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9,
                                         10, 10, 11, 11, 12, 12, 13, 13, 0, 0};
  const uint32_t lit_mask = (1u << FF6_GZ_LIT_BITS) - 1;
  const uint32_t dist_mask = (1u << FF6_GZ_DIST_BITS) - 1;
  uint8_t *out = *out_at;

  /* Fast part: enough input and output that no single step needs a check. A step
   * refills three times at most, three bytes each, plus the four-byte load. */
  const uint8_t *in = r->in;
  uint32_t bits = r->bits;
  int count = r->count;
  while (r->in_end - in >= 16 && out_end - out >= FF6_GZ_MARGIN) {
#define FF6_GZ_REFILL()                                                        \
  do {                                                                         \
    bits |= ff6_gz_load32(in) << count;                                        \
    in += (31 - count) >> 3;                                                   \
    count |= 24;                                                               \
  } while (0)
#define FF6_GZ_DROP(n)                                                         \
  do {                                                                         \
    bits >>= (n);                                                              \
    count -= (int)(n);                                                         \
  } while (0)
    FF6_GZ_REFILL();
    uint32_t entry = lit[bits & lit_mask];
    if (FF6_GZ_KIND(entry) == FF6_GZ_LITERAL) {
      FF6_GZ_DROP(FF6_GZ_BITS(entry));
      *out++ = (uint8_t)FF6_GZ_VALUE(entry);
      /* Worst case for a second one is the root, all still in the buffer. */
      entry = lit[bits & lit_mask];
      if (FF6_GZ_KIND(entry) == FF6_GZ_LITERAL) {
        FF6_GZ_DROP(FF6_GZ_BITS(entry));
        *out++ = (uint8_t)FF6_GZ_VALUE(entry);
        continue;
      }
      FF6_GZ_REFILL();
    }
    if (FF6_GZ_KIND(entry) == FF6_GZ_SUBTABLE) {
      FF6_GZ_DROP(FF6_GZ_BITS(entry));
      entry = lit[FF6_GZ_VALUE(entry) + (bits & ((1u << FF6_GZ_EXTRA(entry)) - 1))];
      FF6_GZ_DROP(FF6_GZ_BITS(entry));
      FF6_GZ_REFILL();
      if (FF6_GZ_KIND(entry) == FF6_GZ_LITERAL) {
        *out++ = (uint8_t)FF6_GZ_VALUE(entry);
        continue;
      }
    } else {
      FF6_GZ_DROP(FF6_GZ_BITS(entry));
    }
    const unsigned kind = FF6_GZ_KIND(entry);
    if (kind != FF6_GZ_LENGTH) {
      r->in = in;
      r->bits = bits & ((1u << count) - 1);
      r->count = count;
      *out_at = out;
      return kind == FF6_GZ_END;
    }
    const unsigned extra = FF6_GZ_EXTRA(entry);
    const size_t length = FF6_GZ_VALUE(entry) + (bits & ((1u << extra) - 1));
    FF6_GZ_DROP(extra);

    FF6_GZ_REFILL();
    entry = dist[bits & dist_mask];
    if (FF6_GZ_KIND(entry) == FF6_GZ_SUBTABLE) {
      FF6_GZ_DROP(FF6_GZ_BITS(entry));
      entry = dist[FF6_GZ_VALUE(entry) + (bits & ((1u << FF6_GZ_EXTRA(entry)) - 1))];
    }
    FF6_GZ_DROP(FF6_GZ_BITS(entry));
    if (FF6_GZ_KIND(entry) != FF6_GZ_LITERAL)
      return 0;
    const unsigned symbol = FF6_GZ_VALUE(entry);
    FF6_GZ_REFILL();
    const unsigned dist_bits = dist_extra[symbol];
    const size_t distance = dist_base[symbol] + (bits & ((1u << dist_bits) - 1));
    FF6_GZ_DROP(dist_bits);
    if (!dist_base[symbol] || distance > (size_t)(out - out_begin))
      return 0;
    ff6_gz_copy(out, distance, length);
    out += length;
  }
  r->in = in;
  r->bits = bits & ((1u << count) - 1);
  r->count = count;
#undef FF6_GZ_REFILL
#undef FF6_GZ_DROP

  /* Careful part: the tail of the input or of the output. */
  for (;;) {
    uint32_t entry = ff6_gz_decode(r, lit, FF6_GZ_LIT_BITS);
    const unsigned kind = FF6_GZ_KIND(entry);
    if (kind == FF6_GZ_LITERAL) {
      if (out == out_end)
        return 0;
      *out++ = (uint8_t)FF6_GZ_VALUE(entry);
      continue;
    }
    if (kind != FF6_GZ_LENGTH) {
      *out_at = out;
      return kind == FF6_GZ_END;
    }
    const size_t length = FF6_GZ_VALUE(entry) + ff6_gz_take(r, (int)FF6_GZ_EXTRA(entry));
    entry = ff6_gz_decode(r, dist, FF6_GZ_DIST_BITS);
    if (FF6_GZ_KIND(entry) != FF6_GZ_LITERAL)
      return 0;
    const unsigned symbol = FF6_GZ_VALUE(entry);
    const size_t distance = dist_base[symbol] + ff6_gz_take(r, dist_extra[symbol]);
    if (!dist_base[symbol] || distance > (size_t)(out - out_begin) ||
        length > (size_t)(out_end - out))
      return 0;
    for (size_t i = 0; i < length; i++, out++)
      *out = out[-(ptrdiff_t)distance];
    if (r->overrun > 8)
      return 0;
  }
}

static int ff6_gz_tables(ff6_gz_reader *r, uint32_t *lit, uint32_t *dist) {
  static const uint8_t order[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                    11, 4,  12, 3, 13, 2, 14, 1, 15};
  static const uint16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11, 13,
                                           15, 17, 19, 23, 27, 31, 35, 43,  51, 59,
                                           67, 83, 99, 115, 131, 163, 195, 227, 258};
  static const uint8_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                           2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
  const int hlit = (int)ff6_gz_take(r, 5) + 257;
  const int hdist = (int)ff6_gz_take(r, 5) + 1;
  const int hclen = (int)ff6_gz_take(r, 4) + 4;
  if (hlit > 286 || hdist > 30)
    return 0;

  uint8_t lengths[286 + 30];
  memset(lengths, 0, 19);
  for (int i = 0; i < hclen; i++)
    lengths[order[i]] = (uint8_t)ff6_gz_take(r, 3);

  uint32_t clen_entries[19];
  for (int i = 0; i < 19; i++)
    clen_entries[i] = FF6_GZ_ENTRY(i, 0, FF6_GZ_LITERAL, 0);
  uint32_t clen_table[128];
  if (!ff6_gz_build(clen_table, 128, 7, lengths, 19, clen_entries, 0))
    return 0;

  const int total = hlit + hdist;
  for (int i = 0; i < total;) {
    const uint32_t entry = ff6_gz_decode(r, clen_table, 7);
    if (FF6_GZ_KIND(entry) != FF6_GZ_LITERAL)
      return 0;
    const unsigned symbol = FF6_GZ_VALUE(entry);
    if (symbol < 16) {
      lengths[i++] = (uint8_t)symbol;
      continue;
    }
    uint8_t value = 0;
    int repeat;
    if (symbol == 16) {
      if (!i)
        return 0;
      value = lengths[i - 1];
      repeat = 3 + (int)ff6_gz_take(r, 2);
    } else if (symbol == 17) {
      repeat = 3 + (int)ff6_gz_take(r, 3);
    } else {
      repeat = 11 + (int)ff6_gz_take(r, 7);
    }
    if (repeat > total - i)
      return 0;
    while (repeat-- > 0)
      lengths[i++] = value;
  }
  if (!lengths[256])
    return 0;

  uint32_t entries[288];
  for (int i = 0; i < 256; i++)
    entries[i] = FF6_GZ_ENTRY(i, 0, FF6_GZ_LITERAL, 0);
  entries[256] = FF6_GZ_ENTRY(0, 0, FF6_GZ_END, 0);
  for (int i = 0; i < 29; i++)
    entries[257 + i] = FF6_GZ_ENTRY(length_base[i], length_extra[i], FF6_GZ_LENGTH, 0);
  if (!ff6_gz_build(lit, FF6_GZ_LIT_ENTRIES, FF6_GZ_LIT_BITS, lengths, hlit, entries, 0))
    return 0;

  uint32_t dist_entries[30];
  for (int i = 0; i < 30; i++)
    dist_entries[i] = FF6_GZ_ENTRY(i, 0, FF6_GZ_LITERAL, 0);
  return ff6_gz_build(dist, FF6_GZ_DIST_ENTRIES, FF6_GZ_DIST_BITS, lengths + hlit, hdist,
                      dist_entries, 1);
}

static int ff6_gz_fixed_tables(uint32_t *lit, uint32_t *dist) {
  static const uint16_t length_base[31] = {3,  4,  5,  6,  7,  8,  9,  10,  11, 13, 15,
                                           17, 19, 23, 27, 31, 35, 43, 51,  59, 67, 83,
                                           99, 115, 131, 163, 195, 227, 258, 0, 0};
  static const uint8_t length_extra[31] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                           3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0, 0, 0};
  uint8_t lengths[288];
  uint32_t entries[288];
  for (int i = 0; i < 288; i++)
    lengths[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
  for (int i = 0; i < 256; i++)
    entries[i] = FF6_GZ_ENTRY(i, 0, FF6_GZ_LITERAL, 0);
  entries[256] = FF6_GZ_ENTRY(0, 0, FF6_GZ_END, 0);
  for (int i = 257; i < 288; i++)
    entries[i] = length_base[i - 257]
                     ? FF6_GZ_ENTRY(length_base[i - 257], length_extra[i - 257], FF6_GZ_LENGTH, 0)
                     : FF6_GZ_ENTRY(0, 0, FF6_GZ_INVALID, 0);
  if (!ff6_gz_build(lit, FF6_GZ_LIT_ENTRIES, FF6_GZ_LIT_BITS, lengths, 288, entries, 0))
    return 0;

  uint8_t dist_lengths[32];
  uint32_t dist_entries[32];
  for (int i = 0; i < 32; i++) {
    dist_lengths[i] = 5;
    dist_entries[i] = i < 30 ? FF6_GZ_ENTRY(i, 0, FF6_GZ_LITERAL, 0)
                             : FF6_GZ_ENTRY(0, 0, FF6_GZ_INVALID, 0);
  }
  return ff6_gz_build(dist, FF6_GZ_DIST_ENTRIES, FF6_GZ_DIST_BITS, dist_lengths, 32,
                      dist_entries, 0);
}

/* Returns 1 with exactly out_size bytes written and *in_used set; 0 otherwise, with
 * `out` holding anything. The stored CRC is not checked; the stored size is. */
static int ff6_gunzip(const uint8_t *in, size_t in_size, uint8_t *out, size_t out_size,
                      size_t *in_used) {
  if (in_size < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8 || (in[3] & 0xe0))
    return 0;
  const unsigned flags = in[3];
  size_t at = 10;
  if (flags & 4) {
    if (at + 2 > in_size)
      return 0;
    at += 2 + ((size_t)in[at] | ((size_t)in[at + 1] << 8));
  }
  for (unsigned flag = 8; flag <= 16; flag <<= 1) {
    if (flags & flag) {
      while (at < in_size && in[at])
        at++;
      at++;
    }
  }
  if (flags & 2)
    at += 2;
  if (at + 8 > in_size)
    return 0;

  /* The last eight bytes are the trailer: the deflate data can't reach them. */
  ff6_gz_reader r = {in + at, in + in_size - 8, 0, 0, 0};
  uint8_t *at_out = out;
  const uint8_t *out_end = out + out_size;
  uint32_t lit[FF6_GZ_LIT_ENTRIES], dist[FF6_GZ_DIST_ENTRIES];

  for (;;) {
    const uint32_t last = ff6_gz_take(&r, 1);
    const uint32_t type = ff6_gz_take(&r, 2);
    if (type == 0) {
      /* Back to a byte boundary, giving back the whole bytes already buffered. */
      const int partial = r.count & 7;
      r.bits >>= partial;
      r.count -= partial;
      while (r.count >= 8) {
        if (r.overrun)
          r.overrun--;
        else
          r.in--;
        r.count -= 8;
      }
      r.bits = 0;
      r.count = 0;
      if (r.overrun || r.in_end - r.in < 4)
        return 0;
      const size_t stored = (size_t)r.in[0] | ((size_t)r.in[1] << 8);
      const size_t check = (size_t)r.in[2] | ((size_t)r.in[3] << 8);
      r.in += 4;
      if ((stored ^ check) != 0xffff || stored > (size_t)(r.in_end - r.in) ||
          stored > (size_t)(out_end - at_out))
        return 0;
      memcpy(at_out, r.in, stored);
      r.in += stored;
      at_out += stored;
    } else if (type == 3) {
      return 0;
    } else {
      if (type == 1 ? !ff6_gz_fixed_tables(lit, dist) : !ff6_gz_tables(&r, lit, dist))
        return 0;
      if (!ff6_gz_symbols(&r, lit, dist, out, &at_out, out_end))
        return 0;
    }
    if (last)
      break;
  }

  /* Bytes taken from the buffer's future don't count as consumed. */
  while (r.count >= 8) {
    if (r.overrun)
      r.overrun--;
    else
      r.in--;
    r.count -= 8;
  }
  if (r.overrun || at_out != out_end)
    return 0;
  const uint8_t *trailer = r.in + 4;
  const uint32_t stored_size = (uint32_t)trailer[0] | ((uint32_t)trailer[1] << 8) |
                               ((uint32_t)trailer[2] << 16) | ((uint32_t)trailer[3] << 24);
  if (stored_size != (uint32_t)out_size)
    return 0;
  if (in_used)
    *in_used = (size_t)(r.in + 8 - in);
  return 1;
}

#endif
