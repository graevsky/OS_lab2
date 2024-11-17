#ifndef CACHE_H
#define CACHE_H

#include <pthread.h>
#include <unistd.h>

typedef struct
{
    int fd;               // file id
    off_t block_number;   // block id
    char *data;           // block data
    int dirty;            // dirty flag
    pthread_mutex_t lock; // sync mutex
} cache_entry_t;

typedef struct
{
    cache_entry_t *entries; // cache arr
    int cache_size;         // cache size
    int block_size;         // block size
    pthread_mutex_t lock;   // cache sync mutex
} cache_t;

int cache_init(cache_t *cache, int cache_size, int block_size);
void cache_destroy(cache_t *cache);
cache_entry_t *cache_lookup(cache_t *cache, int fd, off_t block_number);
cache_entry_t *cache_insert(cache_t *cache, int fd, off_t block_number, const char *data);
void cache_evict(cache_t *cache);

#endif // CACHE_H