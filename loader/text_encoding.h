#ifndef FF6_TEXT_ENCODING_H
#define FF6_TEXT_ENCODING_H

#include <stddef.h>
#include <stdint.h>

/* Invalid UTF-8 consumes one byte and becomes U+FFFD. Check each continuation
 * before reading the next so a truncated string never reads past its NUL. */
static inline size_t ff6_decode_utf8(const char *src, uint32_t *codepoint) {
  const unsigned char *s = (const unsigned char *)src;
  uint32_t cp = s[0];
  size_t count;
  uint32_t minimum;

  if (cp < 0x80) {
    *codepoint = cp;
    return cp ? 1 : 0;
  }
  if (cp >= 0xC2 && cp <= 0xDF) {
    count = 2;
    minimum = 0x80;
    cp &= 0x1F;
  } else if (cp >= 0xE0 && cp <= 0xEF) {
    count = 3;
    minimum = 0x800;
    cp &= 0x0F;
  } else if (cp >= 0xF0 && cp <= 0xF4) {
    count = 4;
    minimum = 0x10000;
    cp &= 0x07;
  } else {
    *codepoint = 0xFFFD;
    return 1;
  }

  for (size_t i = 1; i < count; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *codepoint = 0xFFFD;
      return 1;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
    *codepoint = 0xFFFD;
    return 1;
  }
  *codepoint = cp;
  return count;
}

static inline size_t ff6_encode_utf8(char out[4], uint32_t cp) {
  if (cp < 0x80) {
    out[0] = (char)cp;
    return 1;
  }
  if (cp < 0x800) {
    out[0] = (char)(0xC0 | (cp >> 6));
    out[1] = (char)(0x80 | (cp & 0x3F));
    return 2;
  }
  if (cp < 0x10000) {
    out[0] = (char)(0xE0 | (cp >> 12));
    out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[2] = (char)(0x80 | (cp & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (cp >> 18));
  out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
  out[3] = (char)(0x80 | (cp & 0x3F));
  return 4;
}

/* Destination capacities include the terminator. Return the units/bytes
 * written without it, and never split a codepoint to make it fit. */
static inline size_t ff6_utf8_to_utf16(uint16_t *dst, size_t capacity,
                                      const char *src) {
  size_t written = 0;
  if (!capacity)
    return 0;
  while (src && *src) {
    uint32_t cp;
    size_t consumed = ff6_decode_utf8(src, &cp);
    size_t units = cp > 0xFFFF ? 2 : 1;
    if (units > capacity - 1 - written)
      break;
    if (units == 2) {
      cp -= 0x10000;
      dst[written++] = (uint16_t)(0xD800 | (cp >> 10));
      dst[written++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
    } else {
      dst[written++] = (uint16_t)cp;
    }
    src += consumed;
  }
  dst[written] = 0;
  return written;
}

static inline size_t ff6_utf16_to_utf8(char *dst, size_t capacity,
                                      const uint16_t *src, size_t src_units) {
  size_t written = 0;
  if (!capacity)
    return 0;
  for (size_t i = 0; src && i < src_units && src[i]; i++) {
    uint32_t cp = src[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < src_units &&
        src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (src[++i] - 0xDC00);
    } else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    char encoded[4];
    size_t bytes = ff6_encode_utf8(encoded, cp);
    if (bytes > capacity - 1 - written)
      break;
    for (size_t j = 0; j < bytes; j++)
      dst[written++] = encoded[j];
  }
  dst[written] = '\0';
  return written;
}

/* max_units uses Android/IME UTF-16 length semantics; a supplementary
 * codepoint uses two units. Source and destination must not overlap. */
static inline size_t ff6_copy_utf8(char *dst, size_t capacity, const char *src,
                                  size_t max_units) {
  size_t written = 0, used_units = 0;
  if (!capacity)
    return 0;
  while (src && *src) {
    uint32_t cp;
    size_t consumed = ff6_decode_utf8(src, &cp);
    size_t units = cp > 0xFFFF ? 2 : 1;
    char encoded[4];
    size_t bytes = ff6_encode_utf8(encoded, cp);
    if (units > max_units - used_units || bytes > capacity - 1 - written)
      break;
    for (size_t j = 0; j < bytes; j++)
      dst[written++] = encoded[j];
    used_units += units;
    src += consumed;
  }
  dst[written] = '\0';
  return written;
}

#endif
