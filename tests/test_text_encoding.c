#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../loader/text_encoding.h"

static void test_names_and_utf16_limits(void) {
  uint16_t units[32];
  char text[64];
  const char *name = "Caf\xC3\xA9 \xE3\x83\x86\xE3\x82\xA3\xE3\x83\x8A";
  assert(ff6_utf8_to_utf16(units, 32, name) == 8);
  assert(ff6_utf16_to_utf8(text, sizeof(text), units, 32) == strlen(name));
  assert(strcmp(text, name) == 0);

  assert(ff6_copy_utf8(text, sizeof(text), name, 4) == 5);
  assert(strcmp(text, "Caf\xC3\xA9") == 0);
  assert(ff6_copy_utf8(text, sizeof(text), "\xE3\x83\x86\xE3\x82\xA3\xE3\x83\x8A", 2) == 6);
  assert(strcmp(text, "\xE3\x83\x86\xE3\x82\xA3") == 0);

  const char *supplementary = "\xF0\x9F\x98\x80" "A";
  assert(ff6_utf8_to_utf16(units, 32, supplementary) == 3);
  assert(units[0] == 0xD83D && units[1] == 0xDE00 && units[2] == 'A');
  assert(ff6_utf16_to_utf8(text, sizeof(text), units, 32) == 5);
  assert(strcmp(text, supplementary) == 0);
  assert(ff6_copy_utf8(text, sizeof(text), supplementary, 1) == 0);
  assert(text[0] == '\0');
  assert(ff6_copy_utf8(text, sizeof(text), supplementary, 2) == 4);
  assert(strcmp(text, "\xF0\x9F\x98\x80") == 0);
  assert(ff6_copy_utf8(text, sizeof(text), supplementary, 3) == 5);
}

static void test_destination_bounds(void) {
  struct {
    char text[4];
    unsigned char guard[4];
  } bytes = {{0}, {0xA5, 0xA5, 0xA5, 0xA5}};
  struct {
    uint16_t text[2];
    uint16_t guard;
  } units = {{0}, 0xA55A};
  const uint16_t hiragana[] = {0x3042, 0x3044, 0};
  const uint16_t supplementary[] = {0xD83D, 0xDE00, 0};

  assert(ff6_utf16_to_utf8(bytes.text, sizeof(bytes.text), hiragana, 3) == 3);
  assert(strcmp(bytes.text, "\xE3\x81\x82") == 0);
  assert(ff6_utf16_to_utf8(bytes.text, sizeof(bytes.text), supplementary, 3) == 0);
  assert(bytes.text[0] == '\0');
  assert(ff6_copy_utf8(bytes.text, sizeof(bytes.text), "a\xE3\x81\x82", 10) == 1);
  assert(strcmp(bytes.text, "a") == 0);
  for (size_t i = 0; i < sizeof(bytes.guard); i++)
    assert(bytes.guard[i] == 0xA5);

  assert(ff6_utf8_to_utf16(units.text, 2, "\xF0\x9F\x98\x80") == 0);
  assert(units.text[0] == 0);
  assert(ff6_utf8_to_utf16(units.text, 2, "\xC3\xA9" "A") == 1);
  assert(units.text[0] == 0xE9 && units.text[1] == 0);
  assert(units.guard == 0xA55A);

  assert(ff6_utf16_to_utf8(bytes.text, 1, hiragana, 3) == 0);
  assert(bytes.text[0] == '\0');
  assert(ff6_utf8_to_utf16(units.text, 1, "abc") == 0);
  assert(units.text[0] == 0);
  assert(ff6_copy_utf8(bytes.text, 1, "abc", 10) == 0);
  assert(bytes.text[0] == '\0');
  assert(ff6_copy_utf8(bytes.text, sizeof(bytes.text), "abc", 0) == 0);
  assert(bytes.text[0] == '\0');

  assert(ff6_utf16_to_utf8(NULL, 0, hiragana, 3) == 0);
  assert(ff6_utf8_to_utf16(NULL, 0, "abc") == 0);
  assert(ff6_copy_utf8(NULL, 0, "abc", 10) == 0);
  assert(ff6_utf16_to_utf8(bytes.text, sizeof(bytes.text), NULL, 0) == 0);
  assert(ff6_utf8_to_utf16(units.text, 2, NULL) == 0);
  assert(ff6_copy_utf8(bytes.text, sizeof(bytes.text), NULL, 10) == 0);
}

static void test_malformed_input(void) {
  uint16_t units[16];
  char text[64];
  const char truncated[] = {(char)0xF0, (char)0x9F, 0};
  assert(ff6_utf8_to_utf16(units, 16, truncated) == 2);
  assert(units[0] == 0xFFFD && units[1] == 0xFFFD && units[2] == 0);

  const char *invalid[] = {"\xC0\xAF", "\xED\xA0\x80", "\xF4\x90\x80\x80", "\x80"};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
    size_t count = ff6_utf8_to_utf16(units, 16, invalid[i]);
    assert(count == strlen(invalid[i]));
    for (size_t j = 0; j < count; j++)
      assert(units[j] == 0xFFFD);
  }

  const uint16_t broken_pair[] = {0xD83D, 'A', 0xDE00, 0};
  assert(ff6_utf16_to_utf8(text, sizeof(text), broken_pair, 4) == 7);
  assert(strcmp(text, "\xEF\xBF\xBD" "A" "\xEF\xBF\xBD") == 0);
  /* No terminator is provided: the source-unit bound must stop the read. */
  const uint16_t lone_high[] = {0xD83D};
  assert(ff6_utf16_to_utf8(text, sizeof(text), lone_high, 1) == 3);
  assert(strcmp(text, "\xEF\xBF\xBD") == 0);
  assert(ff6_copy_utf8(text, sizeof(text), "\xFF" "A", 1) == 3);
  assert(strcmp(text, "\xEF\xBF\xBD") == 0);
}

static void test_full_ime_capacity(void) {
  enum { MAX_UNITS = 2048 };
  uint16_t input[MAX_UNITS + 1];
  struct {
    char text[3 * MAX_UNITS + 1];
    unsigned char guard;
  } output;
  for (size_t i = 0; i < MAX_UNITS; i++)
    input[i] = 0x3042;
  input[MAX_UNITS] = 0;
  output.guard = 0xA5;
  assert(ff6_utf16_to_utf8(output.text, sizeof(output.text), input, MAX_UNITS) == 3 * MAX_UNITS);
  assert(strlen(output.text) == 3 * MAX_UNITS);
  assert(output.guard == 0xA5);
  assert(ff6_utf8_to_utf16(input, MAX_UNITS + 1, output.text) == MAX_UNITS);
  assert(input[MAX_UNITS] == 0);
  for (size_t i = 0; i < MAX_UNITS; i++)
    assert(input[i] == 0x3042);

  /* The bridge's smaller buffer must still end at a whole character. */
  char bridge_text[256];
  assert(ff6_copy_utf8(bridge_text, sizeof(bridge_text), output.text, 99) == 255);
  assert(strlen(bridge_text) == 255);
  assert(ff6_utf8_to_utf16(input, MAX_UNITS + 1, bridge_text) == 85);
}

int main(void) {
  test_names_and_utf16_limits();
  test_destination_bounds();
  test_malformed_input();
  test_full_ime_capacity();
  puts("text encoding tests passed");
  return 0;
}
