#include "../loader/asset_cache.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static int blocks, refuse_allocs;

static void *host_alloc(void *context, size_t size) {
  (void)context;
  if (refuse_allocs > 0) {
    refuse_allocs--;
    return NULL;
  }
  blocks++;
  return malloc(size);
}

static void host_release(void *context, void *block) {
  (void)context;
  blocks--;
  free(block);
}

static const ff6_asset_ops ops = {host_alloc, host_release, NULL};
static ff6_asset_cache cache;

static int cached(const char *name, unsigned char expected, size_t expected_size) {
  size_t size = 0;
  ff6_asset_entry *pin = NULL;
  const unsigned char *data = ff6_asset_pin(&cache, name, &size, &pin);
  if (!data)
    return 0;
  assert(size == expected_size);
  for (size_t i = 0; i < size; i++)
    assert(data[i] == expected);
  ff6_asset_unpin(&cache, pin);
  return 1;
}

static void put(const char *name, unsigned char value, size_t size) {
  unsigned char *data = malloc(size ? size : 1);
  memset(data, value, size);
  ff6_asset_put(&cache, name, data, size);
  /* The cache owns a copy: the engine frees or changes its own buffer. */
  memset(data, 0xff, size);
  free(data);
}

static void drain(void) {
  ff6_asset_lock(&cache);
  while (ff6_asset_evict_oldest(&cache)) {}
  ff6_asset_unlock(&cache);
  assert(blocks == 0 && cache.used == 0);
}

int main(void) {
  /* Round trip, misses and an empty asset. */
  ff6_asset_init(&cache, &ops, 1000);
  assert(!cached("a", 1, 10));
  put("a", 1, 10);
  put("empty", 0, 0);
  assert(cached("a", 1, 10));
  assert(cached("empty", 0, 0));
  assert(cache.hits == 2 && cache.misses == 1 && cache.hit_bytes == 10);

  /* A repeated name keeps the first copy and its accounting. */
  put("a", 2, 10);
  assert(cached("a", 1, 10));
  assert(cache.used == 10);
  drain();

  /* Least recently used goes first, and a lookup counts as use. */
  ff6_asset_init(&cache, &ops, 300);
  put("one", 1, 100);
  put("two", 2, 100);
  put("three", 3, 100);
  assert(cached("one", 1, 100));
  put("four", 4, 100);
  assert(cached("one", 1, 100));
  assert(!cached("two", 2, 100));
  assert(cached("three", 3, 100));
  assert(cached("four", 4, 100));
  assert(cache.used <= cache.budget && cache.evictions == 1);
  drain();

  /* More than half the budget is never kept, and evicts nothing trying. */
  ff6_asset_init(&cache, &ops, 300);
  put("small", 1, 100);
  put("huge", 2, 151);
  assert(!cached("huge", 2, 151));
  assert(cached("small", 1, 100));
  put("half", 3, 150);
  assert(cached("half", 3, 150));
  drain();

  /* A pinned entry survives eviction, and is released once unpinned. */
  ff6_asset_init(&cache, &ops, 200);
  put("pinned", 1, 100);
  size_t size;
  ff6_asset_entry *pin;
  const unsigned char *held = ff6_asset_pin(&cache, "pinned", &size, &pin);
  assert(held);
  put("other", 2, 100);
  put("another", 3, 100);
  assert(held[0] == 1 && held[99] == 1);
  assert(cached("pinned", 1, 100));
  ff6_asset_unpin(&cache, pin);
  put("more", 4, 100);
  put("again", 5, 100);
  assert(!cached("pinned", 1, 100));
  drain();

  /* Nothing evictable: the put gives up with all the state intact. */
  ff6_asset_init(&cache, &ops, 100);
  put("only", 1, 50);
  held = ff6_asset_pin(&cache, "only", &size, &pin);
  put("blocked", 2, 50);
  put("blocked2", 3, 50);
  assert(cached("only", 1, 50));
  assert(cache.used <= cache.budget);
  ff6_asset_unpin(&cache, pin);
  drain();

  /* The allocator refusing evicts until it can, or gives up. */
  ff6_asset_init(&cache, &ops, 1000);
  put("first", 1, 100);
  put("second", 2, 100);
  refuse_allocs = 1;
  put("third", 3, 100);
  assert(!cached("first", 1, 100));
  assert(cached("second", 2, 100));
  assert(cached("third", 3, 100));
  refuse_allocs = 10;
  put("fourth", 4, 100);
  assert(!cached("fourth", 4, 100));
  assert(cache.used == 0 && blocks == 0);
  refuse_allocs = 0;

  /* More names than entries: the table recycles, nothing leaks. */
  ff6_asset_init(&cache, &ops, 1 << 20);
  char name[32];
  for (int i = 0; i < FF6_ASSET_ENTRIES * 3; i++) {
    snprintf(name, sizeof(name), "file/%d.png", i);
    put(name, (unsigned char)i, 16);
    assert(cached(name, (unsigned char)i, 16));
  }
  assert(blocks == FF6_ASSET_ENTRIES);
  snprintf(name, sizeof(name), "file/%d.png", 0);
  assert(!cached(name, 0, 16));
  snprintf(name, sizeof(name), "file/%d.png", FF6_ASSET_ENTRIES * 3 - 1);
  assert(cached(name, (unsigned char)(FF6_ASSET_ENTRIES * 3 - 1), 16));
  drain();

  /* Disabled: every lookup misses and nothing stays. */
  ff6_asset_init(&cache, &ops, 0);
  put("nothing", 1, 10);
  put("empty", 0, 0);
  ff6_asset_lock(&cache);
  while (ff6_asset_evict_oldest(&cache)) {}
  ff6_asset_unlock(&cache);
  assert(!cached("nothing", 1, 10));
  assert(blocks == 0);

  puts("asset_cache: ok");
  return 0;
}
