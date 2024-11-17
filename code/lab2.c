#include "lab2.h"
#include "cache.h"
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>

typedef struct
{
    int fd;    // file descriptor
    off_t pos; // current pos in file
} file_entry_t;

#define MAX_OPEN_FILES 128

static file_entry_t open_files[MAX_OPEN_FILES];

static pthread_mutex_t open_files_lock = PTHREAD_MUTEX_INITIALIZER;

static cache_t cache;

#define BLOCK_SIZE 1024 // 1 KB
#define CACHE_SIZE 128  // 128*1 KB = 128 KB cache

// cache init
__attribute__((constructor)) static void lab2_init()
{
    cache_init(&cache, CACHE_SIZE, BLOCK_SIZE);
    srand(time(NULL));

    for (int i = 0; i < MAX_OPEN_FILES; i++)
    {
        open_files[i].fd = -1;
        open_files[i].pos = 0;
    }
}

// cache cleanup
__attribute__((destructor)) static void lab2_cleanup()
{
    cache_destroy(&cache);
    for (int i = 0; i < MAX_OPEN_FILES; i++)
    {
        if (open_files[i].fd != -1)
        {
            close(open_files[i].fd);
        }
    }
}

// O(n) search :)
static int find_open_file(int fd)
{
    for (int i = 0; i < MAX_OPEN_FILES; i++)
    {
        if (open_files[i].fd == fd)
        {
            return i;
        }
    }
    return -1;
}

// Task funcs

// open file
int lab2_open(const char *path)
{
    int real_fd = open(path, O_RDWR | __O_DIRECT);
    if (real_fd == -1)
    {
        perror("open error");
        return -1;
    }
    pthread_mutex_lock(&open_files_lock);
    int idx = -1;
    for (int i = 0; i < MAX_OPEN_FILES; i++)
    {
        if (open_files[i].fd == -1)
        {
            open_files[i].fd = real_fd;
            open_files[i].pos = 0;
            idx = real_fd;
            break;
        }
    }

    pthread_mutex_unlock(&open_files_lock);

    if (idx == -1)
    {
        close(real_fd);
        fprintf(stderr, "too many open files...");
        return -1;
    }

    return idx;
}

// close file
int lab2_close(int fd)
{
    pthread_mutex_lock(&open_files_lock);
    int idx = find_open_file(fd);

    if (idx == -1)
    {
        pthread_mutex_unlock(&open_files_lock);
        errno = EBADF;
        return -1;
    }

    pthread_mutex_unlock(&open_files_lock);

    if (lab2_fsync(fd) != 0)
    {
        perror("lab2_fsync error");
    }

    pthread_mutex_lock(&open_files_lock);

    close(open_files[idx].fd);
    open_files[idx].fd = -1;
    open_files[idx].pos = 0;

    pthread_mutex_unlock(&open_files_lock);

    return 0;
}

// read from file
ssize_t lab2_read(int fd, void *buf, size_t count)
{
    pthread_mutex_lock(&open_files_lock);
    int idx = find_open_file(fd);

    if (idx == -1)
    {
        pthread_mutex_unlock(&open_files_lock);
        errno = EBADF;
        return -1;
    }

    off_t position = open_files[idx].pos;
    int real_fd = open_files[idx].fd;
    pthread_mutex_unlock(&open_files_lock);

    size_t bytes_read = 0;
    char *buffer = (char *)buf;

    while (bytes_read < count)
    {
        off_t block_number = position / BLOCK_SIZE;
        off_t block_offset = position % BLOCK_SIZE;
        size_t bytes_to_read = BLOCK_SIZE - block_offset;

        if (bytes_to_read > (count - bytes_read))
        {
            bytes_to_read = count - bytes_read;
        }

        // check block in cache
        cache_entry_t *entry = cache_lookup(&cache, real_fd, block_number);
        if (!entry)
        {
            // block not found in cache
            char *block_data;
            if (posix_memalign((void **)&block_data, BLOCK_SIZE, BLOCK_SIZE) != 0)
            {
                perror("posix_memalign error");
                return -1;
            }

            off_t offset = block_number * BLOCK_SIZE;
            ssize_t ret = pread(real_fd, block_data, BLOCK_SIZE, offset);
            if (ret == -1)
            {
                perror("pread error");
                free(block_data);
                return -1;
            }

            // insert block into cache
            entry = cache_insert(&cache, real_fd, block_number, block_data);
            free(block_data);
        }

        pthread_mutex_lock(&entry->lock);

        // copy from cache
        memcpy(buffer + bytes_read, entry->data + block_offset, bytes_to_read);

        pthread_mutex_unlock(&entry->lock);

        bytes_read += bytes_to_read;
        position += bytes_to_read;
    }

    pthread_mutex_lock(&open_files_lock);
    open_files[idx].pos = position;
    pthread_mutex_unlock(&open_files_lock);

    return bytes_read;
}

// write to file
ssize_t lab2_write(int fd, const void *buf, size_t count)
{
    pthread_mutex_lock(&open_files_lock);
    int idx = find_open_file(fd);

    if (idx == -1)
    {
        pthread_mutex_unlock(&open_files_lock);
        errno = EBADF;
        return -1;
    }

    off_t position = open_files[idx].pos;
    int real_fd = open_files[idx].fd;
    pthread_mutex_unlock(&open_files_lock);

    size_t bytes_written = 0;
    const char *buffer = (const char *)buf;

    while (bytes_written < count)
    {
        off_t block_number = position / BLOCK_SIZE;
        off_t block_offset = position % BLOCK_SIZE;
        size_t bytes_to_write = BLOCK_SIZE - block_offset;

        if (bytes_to_write > (count - bytes_written))
        {
            bytes_to_write = count - bytes_written;
        }

        // check if block in cache
        cache_entry_t *entry = cache_lookup(&cache, real_fd, block_number);
        if (!entry)
        {
            char *block_data;

            if (posix_memalign((void **)&block_data, BLOCK_SIZE, BLOCK_SIZE) != 0)
            {
                perror("posix_memalign error");
                return -1;
            }

            off_t offset = block_number * BLOCK_SIZE;
            ssize_t ret = pread(real_fd, block_data, BLOCK_SIZE, offset);

            if (ret == -1)
            {
                perror("pread error");
                free(block_data);
                return -1;
            }

            // insert into cache
            entry = cache_insert(&cache, real_fd, block_number, block_data);
            free(block_data);
        }

        pthread_mutex_lock(&entry->lock);
        memcpy(entry->data + block_offset, buffer + bytes_written, bytes_to_write);
        entry->dirty = 1;
        pthread_mutex_unlock(&entry->lock);

        bytes_written += bytes_to_write;
        position += bytes_to_write;
    }

    pthread_mutex_lock(&open_files_lock);
    open_files[idx].pos = position;
    pthread_mutex_unlock(&open_files_lock);

    return bytes_written;
}

// change file pos
off_t lab2_lseek(int fd, off_t offset, int whence)
{
    pthread_mutex_lock(&open_files_lock);
    int idx = find_open_file(fd);

    if (idx == -1)
    {
        pthread_mutex_unlock(&open_files_lock);
        errno = EBADF;
        return -1;
    }

    off_t new_position;
    switch (whence)
    {
    case SEEK_SET:
        new_position = offset;
        break;
    case SEEK_CUR:
        new_position = open_files[idx].pos + offset;
        break;
    case SEEK_END:
    {
        off_t file_size = lseek(open_files[idx].fd, 0, SEEK_END);
        if (file_size == -1)
        {
            pthread_mutex_unlock(&open_files_lock);
            perror("lseek error for SEEK_END pos");
            return -1;
        }
        new_position = file_size + offset;
        break;
    }
    default:
        pthread_mutex_unlock(&open_files_lock);
        errno = EINVAL;
        return -1;
    }

    if (new_position < 0)
    {
        pthread_mutex_unlock(&open_files_lock);
        errno = EINVAL;
        return -1;
    }

    open_files[idx].pos = new_position;
    pthread_mutex_unlock(&open_files_lock);
    return new_position;
}

// data sync with drive
int lab2_fsync(int fd)
{
    pthread_mutex_lock(&open_files_lock);
    int real_fd = -1;
    int idx = find_open_file(fd);

    if (idx != -1)
    {
        real_fd = open_files[idx].fd;
    }

    pthread_mutex_unlock(&open_files_lock);

    if (real_fd == -1)
    {
        errno = EBADF;
        return -1;
    }

    // cache scan
    for (int i = 0; i < cache.cache_size; i++)
    {
        pthread_mutex_lock(&cache.entries[i].lock);

        if (cache.entries[i].fd == real_fd && cache.entries[i].dirty)
        {
            off_t offset = cache.entries[i].block_number * cache.block_size;
            ssize_t written = pwrite(real_fd, cache.entries[i].data, cache.block_size, offset);

            if (written == -1)
            {
                perror("pwrite error");
                pthread_mutex_unlock(&cache.entries[i].lock);
                return -1;
            }

            cache.entries[i].dirty = 0;
        }
        pthread_mutex_unlock(&cache.entries[i].lock);
    }

    if (fsync(real_fd) == -1)
    {
        perror("fsync error");
        return -1;
    }
    return 0;
}