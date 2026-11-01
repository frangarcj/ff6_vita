#ifndef FF6_ASSET_CACHE_H
#define FF6_ASSET_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Keeps decoded assets in memory by name: the archive is read-only, so a name
 * always means the same bytes, and the engine loads the same files every time a
 * menu opens or a battle starts. Least recently used entries go first. Copies run
 * outside the lock, on a pinned entry that eviction leaves alone, so a thread
 * waiting for the lock never waits for a copy. */

#define FF6_ASSET_ENTRIES 2048
#define FF6_ASSET_BUCKETS 1024

typedef struct {
  void *(*alloc)(void *context, size_t size);
  void (*release)(void *context, void *block);
  void *context;
} ff6_asset_ops;

typedef struct {
  char *name; /* Start of the block; the data follows the terminator. */
  const void *data;
  size_t size;
  uint32_t hash;
  uint32_t tick;
  int next; /* Next entry in the bucket, or next free entry; -1 ends either. */
  int pins;
} ff6_asset_entry;

typedef struct {
  const ff6_asset_ops *ops;
  size_t budget, used;
  int lock;
  uint32_t tick;
  int free_head;
  unsigned hits, misses, evictions;
  uint64_t hit_bytes, stored_bytes;
  int buckets[FF6_ASSET_BUCKETS];
  ff6_asset_entry entries[FF6_ASSET_ENTRIES];
} ff6_asset_cache;

static inline void ff6_asset_lock(ff6_asset_cache *cache) {
  while (__atomic_exchange_n(&cache->lock, 1, __ATOMIC_ACQUIRE)) {}
}

static inline void ff6_asset_unlock(ff6_asset_cache *cache) {
  __atomic_store_n(&cache->lock, 0, __ATOMIC_RELEASE);
}

static inline uint32_t ff6_asset_hash(const char *name) {
  uint32_t hash = 2166136261u;
  for (; *name; name++)
    hash = (hash ^ (uint8_t)*name) * 16777619u;
  return hash;
}

/* A budget of zero leaves the cache disabled: every lookup misses, nothing stays. */
static inline void ff6_asset_init(ff6_asset_cache *cache, const ff6_asset_ops *ops,
                                  size_t budget) {
  memset(cache, 0, sizeof(*cache));
  cache->ops = ops;
  cache->budget = budget;
  for (int i = 0; i < FF6_ASSET_BUCKETS; i++)
    cache->buckets[i] = -1;
  for (int i = 0; i < FF6_ASSET_ENTRIES; i++)
    cache->entries[i].next = i + 1 < FF6_ASSET_ENTRIES ? i + 1 : -1;
}

/* Call with the lock held. */
static inline ff6_asset_entry *ff6_asset_find(ff6_asset_cache *cache, const char *name,
                                              uint32_t hash) {
  for (int i = cache->buckets[hash % FF6_ASSET_BUCKETS]; i >= 0; i = cache->entries[i].next) {
    ff6_asset_entry *entry = &cache->entries[i];
    if (entry->hash == hash && !strcmp(entry->name, name))
      return entry;
  }
  return NULL;
}

/* Call with the lock held. Returns 0 when everything left is pinned. */
static inline int ff6_asset_evict_oldest(ff6_asset_cache *cache) {
  int oldest = -1;
  uint32_t oldest_age = 0;
  for (int b = 0; b < FF6_ASSET_BUCKETS; b++) {
    for (int i = cache->buckets[b]; i >= 0; i = cache->entries[i].next) {
      const ff6_asset_entry *entry = &cache->entries[i];
      const uint32_t age = cache->tick - entry->tick;
      if (!entry->pins && (oldest < 0 || age > oldest_age)) {
        oldest = i;
        oldest_age = age;
      }
    }
  }
  if (oldest < 0)
    return 0;
  ff6_asset_entry *entry = &cache->entries[oldest];
  int *link = &cache->buckets[entry->hash % FF6_ASSET_BUCKETS];
  while (*link != oldest)
    link = &cache->entries[*link].next;
  *link = entry->next;
  cache->used -= entry->size;
  cache->ops->release(cache->ops->context, entry->name);
  entry->name = NULL;
  entry->next = cache->free_head;
  cache->free_head = oldest;
  cache->evictions++;
  return 1;
}

/* The entry's bytes, valid until ff6_asset_unpin; NULL when it isn't cached. */
static inline const void *ff6_asset_pin(ff6_asset_cache *cache, const char *name,
                                        size_t *size, ff6_asset_entry **pin) {
  const uint32_t hash = ff6_asset_hash(name);
  const void *data = NULL;
  ff6_asset_lock(cache);
  ff6_asset_entry *entry = ff6_asset_find(cache, name, hash);
  if (entry) {
    entry->pins++;
    entry->tick = ++cache->tick;
    cache->hits++;
    cache->hit_bytes += entry->size;
    data = entry->data;
    *size = entry->size;
    *pin = entry;
  } else {
    cache->misses++;
  }
  ff6_asset_unlock(cache);
  return data;
}

static inline void ff6_asset_unpin(ff6_asset_cache *cache, ff6_asset_entry *pin) {
  ff6_asset_lock(cache);
  pin->pins--;
  ff6_asset_unlock(cache);
}

/* Keeps a copy of the bytes. Quietly does nothing if they can't fit. */
static inline void ff6_asset_put(ff6_asset_cache *cache, const char *name, const void *data,
                                 size_t size) {
  /* One asset may not push out a whole scene's worth of others. */
  if (size > cache->budget / 2)
    return;
  const size_t name_size = strlen(name) + 1;
  const uint32_t hash = ff6_asset_hash(name);

  ff6_asset_lock(cache);
  if (ff6_asset_find(cache, name, hash)) {
    ff6_asset_unlock(cache);
    return;
  }
  while (cache->used + size > cache->budget || cache->free_head < 0) {
    if (!ff6_asset_evict_oldest(cache)) {
      ff6_asset_unlock(cache);
      return;
    }
  }
  /* The budget is what was asked for; the allocator may still have less in one
   * piece. */
  char *block;
  while (!(block = (char *)cache->ops->alloc(cache->ops->context, name_size + size))) {
    if (!ff6_asset_evict_oldest(cache)) {
      ff6_asset_unlock(cache);
      return;
    }
  }
  /* Claim the room now, so that another thread's put respects it during the copy. */
  cache->used += size;
  ff6_asset_unlock(cache);

  memcpy(block, name, name_size);
  memcpy(block + name_size, data, size);

  ff6_asset_lock(cache);
  if (ff6_asset_find(cache, name, hash) || cache->free_head < 0) {
    cache->used -= size;
    cache->ops->release(cache->ops->context, block);
    ff6_asset_unlock(cache);
    return;
  }
  const int index = cache->free_head;
  ff6_asset_entry *entry = &cache->entries[index];
  cache->free_head = entry->next;
  entry->name = block;
  entry->data = block + name_size;
  entry->size = size;
  entry->hash = hash;
  entry->tick = ++cache->tick;
  entry->pins = 0;
  entry->next = cache->buckets[hash % FF6_ASSET_BUCKETS];
  cache->buckets[hash % FF6_ASSET_BUCKETS] = index;
  cache->stored_bytes += size;
  ff6_asset_unlock(cache);
}

#endif
