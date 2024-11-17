#include "cache.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>

int cache_init(cache_t *cache, int cache_size, int block_size)
{
    cache->entries = malloc(sizeof(cache_entry_t) * cache_size);
    if (!cache->entries)
    {
        perror("malloc error");
        return -1;
    }

    cache->cache_size = cache_size;
    cache->block_size = block_size;
    pthread_mutex_init(&cache->lock, NULL);

    for (int i = 0; i < cache_size; i++)
    {
        cache->entries[i].fd = -1;
        cache->entries[i].block_number = -1;

        int ret = posix_memalign((void **)&cache->entries[i].data, block_size, block_size);
        if (ret != 0)
        {
            perror("posix_memalign error");
            for (int j = 0; j < i; j++)
            {
                free(cache->entries[j].data);
            }
            free(cache->entries);
            pthread_mutex_destroy(&cache->lock);
            return -1;
        }

        memset(cache->entries[i].data, 0, block_size);

        cache->entries[i].dirty = 0;
        pthread_mutex_init(&cache->entries[i].lock, NULL);
    }
    return 0;
}

void cache_destroy(cache_t *cache)
{
    for (int i = 0; i < cache->cache_size; i++)
    {
        pthread_mutex_destroy(&cache->entries[i].lock);
        free(cache->entries[i].data);
    }
    free(cache->entries);
    pthread_mutex_destroy(&cache->lock);
}

cache_entry_t *cache_lookup(cache_t *cache, int fd, off_t block_number)
{
    for (int i = 0; i < cache->cache_size; i++)
    {
        pthread_mutex_lock(&cache->entries[i].lock);

        if (cache->entries[i].fd == fd && cache->entries[i].block_number == block_number)
        {
            pthread_mutex_unlock(&cache->entries[i].lock);
            return &cache->entries[i];
        }

        pthread_mutex_unlock(&cache->entries[i].lock);
    }
    return NULL;
}

cache_entry_t *cache_insert(cache_t *cache, int fd, off_t block_number, const char *data)
{
    pthread_mutex_lock(&cache->lock);
    for (int i = 0; i < cache->cache_size; i++)
    {
        pthread_mutex_lock(&cache->entries[i].lock);

        if (cache->entries[i].fd == -1)
        {
            cache->entries[i].fd = fd;
            cache->entries[i].block_number = block_number;
            memcpy(cache->entries[i].data, data, cache->block_size);
            cache->entries[i].dirty = 0;
            pthread_mutex_unlock(&cache->entries[i].lock);
            pthread_mutex_unlock(&cache->lock);
            return &cache->entries[i];
        }
        pthread_mutex_unlock(&cache->entries[i].lock);
    }
    
    cache_evict(cache); // cache overflow -> eviction;
        
    pthread_mutex_unlock(&cache->lock);
    return cache_insert(cache, fd, block_number, data);
}

void cache_evict(cache_t *cache)
{
    int idx = rand() % cache->cache_size; // random idx for eviction

    pthread_mutex_lock(&cache->entries[idx].lock);

    if (cache->entries[idx].dirty)
    {
        off_t offset = cache->entries[idx].block_number * cache->block_size;
        lseek(cache->entries[idx].fd, offset, SEEK_SET);
        ssize_t written = write(cache->entries[idx].fd, cache->entries[idx].data, cache->block_size);

        if (written == -1)
        {
            perror("writing error for cache eviction");
            pthread_mutex_unlock(&cache->entries[idx].lock);
        }
    }

    cache->entries[idx].fd = -1;
    cache->entries[idx].block_number = -1;
    cache->entries[idx].dirty = 0;

    pthread_mutex_unlock(&cache->entries[idx].lock);
}