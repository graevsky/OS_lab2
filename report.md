# Отчет по лабораторной работе №2\. Операционные системы

## Раевский Григорий, P3321

## Репозиторий

https://github.com/graevsky/OS_lab2

## Вариант

- ОС: Linux
- Политика вытеснения: Random

## Задание

Для оптимизации работы с блочными устройствами в ОС существует кэш страниц с данными, которыми мы производим операции чтения и записи на диск. Такой кэш позволяет избежать высоких задержек при повторном доступе к данным, так как операция будет выполнена с данными в RAM, а не на диске (вспомним пирамиду памяти).

В данной лабораторной работе необходимо реализовать блочный кэш в пространстве пользователя в виде динамической библиотеки (dll или so). Политику вытеснения страниц и другие элементы задания необходимо получить у преподавателя.

При выполнении работы необходимо реализовать простой API для работы с файлами, предоставляющий пользователю следующие возможности:

- Открытие файла по заданному пути файла, доступного для чтения. Процедура возвращает некоторый хэндл на файл. Пример: `int lab2_open(const char *path)`.
- Закрытие файла по хэндлу. Пример: `int lab2_close(int fd)`.
- Чтение данных из файла. Пример: `ssize_t lab2_read(int fd, void buf[.count], size_t count)`.
- Запись данных в файл. Пример: `ssize_t lab2_write(int fd, const void buf[.count], size_t count)`.
- Перестановка позиции указателя на данные файла. Достаточно поддержать только абсолютные координаты. Пример: `​​​​​​​off_t lab2_lseek(int fd, off_t offset, int whence)`.
- Синхронизация данных из кэша с диском. Пример: `int lab2_fsync(int fd)`.

Операции с диском разработанного блочного кеша должны производиться в обход page cache используемой ОС.

В рамках проверки работоспособности разработанного блочного кэша необходимо адаптировать указанную преподавателем программу-загрузчик из ЛР 1, добавив использование кэша. Запустите программу и убедитесь, что она корректно работает. Сравните производительность до и после.

### Ограничения

- Программа (комплекс программ) должна быть реализован на языке C или C++.
- Если по выданному варианту задана политика вытеснения Optimal, то необходимо предоставить пользователю возможность подсказать page cache, когда будет совершен следующий доступ к данным. Это можно сделать либо добавив параметр в процедуры read и write (например, ssize_t lab2_read(int fd, void buf[.count], size_t count, access_hint_t hint)), либо добавив еще одну функцию в API (например, int lab2_advice(int fd, off_t offset, access_hint_t hint)). access_hint_t в данном случае – это абсолютное время или временной интервал, по которому разработанное API будет определять время последующего доступа к данным.
- Запрещено использовать высокоуровневые абстракции над системными вызовами. Необходимо использовать, в случае Unix, процедуры libc.

## Листинг

### cache

cache.h

```c
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
```

cache.c

```c
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
```

### lab2

lab2.h

```c
#ifndef LAB2_H
#define LAB2_H

#include <unistd.h>

int lab2_open(const char *path);
int lab2_close(int fd);

ssize_t lab2_read(int fd, void *buf, size_t count);
ssize_t lab2_write(int fd, const void *buf, size_t count);

off_t lab2_lseek(int fd, off_t offset, int whence);

int lab2_fsync(int fd);

#endif // LAB2_H
```

lab2.c

```c
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
```

### old loader

io_loader.c

```c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>

#define BLOCK_SIZE 1024

void measure_r_lat(const char *filename, int repetitions)
{
    int fd = open(filename, O_RDONLY | __O_DIRECT);
    if (fd == -1)
    {
        fprintf(stderr, "Failed to open file %s\n", filename);
        return;
    }

    char *buff;
    if (posix_memalign((void **)&buff, BLOCK_SIZE, BLOCK_SIZE) != 0)
    {
        fprintf(stderr, "Failed to allocate aligned memory\n");
        close(fd);
        return;
    }

    struct timespec start, end;
    unsigned long long total_latency_ns = 0;

    for (int i = 0; i < repetitions; i++)
    {
        lseek(fd, 0, SEEK_SET);
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (read(fd, buff, BLOCK_SIZE) == -1)
        {
            fprintf(stderr, "Read error\n");
            free(buff);
            close(fd);
            return;
        }

        clock_gettime(CLOCK_MONOTONIC, &end);

        unsigned long long lat_ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        total_latency_ns += lat_ns;
    }

    close(fd);
    free(buff);

    unsigned long long average_latency_ns = total_latency_ns / repetitions;
    printf("Average read latency for %d repetitions: %llu ns\n", repetitions, average_latency_ns);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <file> <repetitions>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int repetitions = atoi(argv[2]);

    measure_r_lat(filename, repetitions);
    return 0;
}
```

### updated loader

loader.c

```c
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "lab2.h"

#define BLOCK_SIZE 1024

void measure_r_lat(const char *filename, int repetitions)
{
    int fd = lab2_open(filename);
    if (fd == -1)
    {
        fprintf(stderr, "Failed to open file %s\n", filename);
        return;
    }

    char *buff;
    if (posix_memalign((void **)&buff, BLOCK_SIZE, BLOCK_SIZE) != 0)
    {
        fprintf(stderr, "Failed to allocate aligned memory\n");
        lab2_close(fd);
        return;
    }

    struct timespec start, end;
    unsigned long long total_latency_ns = 0;

    for (int i = 0; i < repetitions; i++)
    {
        lab2_lseek(fd, 0, SEEK_SET);
        clock_gettime(CLOCK_MONOTONIC, &start);

        if (lab2_read(fd, buff, BLOCK_SIZE) == -1)
        {
            fprintf(stderr, "Read error\n");
            free(buff);
            lab2_close(fd);
            return;
        }

        clock_gettime(CLOCK_MONOTONIC, &end);

        unsigned long long lat_ns = (end.tv_sec - start.tv_sec) * 1e9 + (end.tv_nsec - start.tv_nsec);
        total_latency_ns += lat_ns;
    }

    lab2_close(fd);
    free(buff);

    unsigned long long average_latency_ns = total_latency_ns / repetitions;
    printf("Average read latency for %d repetitions: %llu ns\n", repetitions, average_latency_ns);
}

int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        fprintf(stderr, "Usage: %s <file> <repetitions>\n", argv[0]);
        return 1;
    }

    const char *filename = argv[1];
    int repetitions = atoi(argv[2]);

    measure_r_lat(filename, repetitions);
    return 0;
}
```

## Сравнение

20000 итераций

### old loader, 1

#### time

Запуск:

`time ./io_loader test.bin 20000`

Результат:

```
Average read latency for 20000 repetitions: 293813 ns

real    0m5.890s
user    0m0.020s
sys     0m1.279s
```

#### top

`top -p $(pgrep io_loader)`

```
%Cpu(s): 12.1 us, 12.1 sy, 0.0 ni, 54.5 id, 21.2 wa, 0.0 hi, 0.1 si, 0.0 st
MiB Mem: 7751.4 total, 4834.2 free, 2406.9 used, 828.7 buff/cache
MiB Swap: 0.0 total, 0.0 free, 0.0 used, 5344.5 avail Mem

PID   USER     PR NI VIRT RES  SHR  S %CPU  %MEM TIME+   COMMAND
55067 graevsky 20 0  0.0t 0.0t 0.0t D 20.0  0.0  0:02.00 io-lat-read
```

#### perf

`sudo perf stat -e task-clock,context-switches,cpu-migrations,page-faults ./io_loader test.bin 20000`

```
Performance counter stats for './io_loader test.bin 20000':

            897.17 msec task-clock                       #    0.150 CPUs utilized             
            19,985      context-switches                 #   22.276 K/sec                     
                11      cpu-migrations                   #   12.261 /sec                      
                62      page-faults                      #   69.107 /sec                      

       5.974215683 seconds time elapsed

       0.003793000 seconds user
       1.312437000 seconds sys
```

#### pidstat

`pidstat -w -p $(pgrep -f io_loader) 1`

Количество переключений контекста:

- Вынужденные: 3077
- Невынужденные: 2.0 В секунду

#### iostat

`iostat -d -x sda 1`

```
r/s     rKb/s   w/s   wkB/s w_await f/s  f_await %util
3301.00 3301.00 11.00 92.00 0.55    6.00 0.50    91.10
```

### old loader, 2

`./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 &`

#### time

```
Average read latency for 20000 repetitions: 319602 ns

real    0m6.402s
user    0m0.008s
sys     0m1.343s
Average read latency for 20000 repetitions: 322064 ns

real    0m6.452s
user    0m0.008s
sys     0m1.331s
```

#### top

```
%Cpu(s): 4.8 us, 11.3 sy, 0.0 ni, 51.7 id, 28.5.1 wa, 0.0 hi, 3.7 si, 0.0 st
MiB Mem: 7751.4 total, 4305.6 free, 2064.9 used, 1696.0 buff/cache
MiB Swap: 0.0 total, 0.0 free, 0.0 used, 5686.6 avail Mem

PID   USER     PR NI VIRT RES  SHR  S %CPU  %MEM TIME+   COMMAND
15475 graevsky 20 0  0.0t 0.0t 0.0t D 20.3  0.0  0:00.61 io_loader
15474 graevsky 20 0  0.0t 0.0t 0.0t R 19.3  0.0  0:00.58 io_loader
```

#### perf

```
Average read latency for 20000 repetitions: 345023 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          1,082.14 msec task-clock                       #    0.157 CPUs utilized             
            19,786      context-switches                 #   18.284 K/sec                     
                25      cpu-migrations                   #   23.102 /sec                      
                63      page-faults                      #   58.218 /sec                      

       6.909766198 seconds time elapsed

       0.010627000 seconds user
       1.479958000 seconds sys


Average read latency for 20000 repetitions: 329953 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          1,071.21 msec task-clock                       #    0.162 CPUs utilized             
            19,820      context-switches                 #   18.502 K/sec                     
                 7      cpu-migrations                   #    6.535 /sec                      
                63      page-faults                      #   58.812 /sec                      

       6.608376406 seconds time elapsed

       0.004987000 seconds user
       1.476169000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

Количество переключений контекста:

- Вынужденные: 3049 и 2380
- Невынужденные: 0.0 и 1.00 В секунду

#### iostat

`iostat -d -x sda 1`

```
r/s     rKb/s   w/s  r_await  wkB/s w_await f/s  f_await %util
5835.00 5835.00 0.00 0.32     0.00  0.00    0.00 0.00    85.10
```

### old loader, 4

`./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 &`

#### time

```
Average read latency for 20000 repetitions: 382307 ns

real    0m7.655s
user    0m0.025s
sys     0m1.924s
Average read latency for 20000 repetitions: 359393 ns

real    0m7.198s
user    0m0.010s
sys     0m1.734s
Average read latency for 20000 repetitions: 403242 ns

real    0m8.078s
user    0m0.011s
sys     0m2.341s
Average read latency for 20000 repetitions: 396547 ns

real    0m7.941s
user    0m0.015s
sys     0m2.166s
```

#### top

```
%Cpu(s): 5.8 us, 19.8 sy, 0.0 ni, 12.4 id, 42.4 wa, 0.0 hi, 19.7 si, 0.0 st
MiB Mem: 7751.4 total, 4182.3 free, 2180.4 used, 1715.0 buff/cache
MiB Swap: 0.0 total, 0.0 free, 0.0 used, 5571.1 avail Mem

PID   USER     PR NI VIRT RES  SHR  S %CPU  %MEM TIME+   COMMAND
20601 graevsky 20 0  0.0t 0.0t 0.0t R 38.9  0.0  0:01.72 io_loader
20600 graevsky 20 0  0.0t 0.0t 0.0t D 28.6  0.0  0:01.47 io_loader
20599 graevsky 20 0  0.0t 0.0t 0.0t D 23.9  0.0  0:01.33 io_loader
20598 graevsky 20 0  0.0t 0.0t 0.0t D 22.9  0.0  0:01.34 io_loader
```

#### perf

```
Average read latency for 20000 repetitions: 380834 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          1,351.64 msec task-clock                       #    0.177 CPUs utilized             
            19,989      context-switches                 #   14.789 K/sec                     
                68      cpu-migrations                   #   50.309 /sec                      
                62      page-faults                      #   45.870 /sec                      

       7.626325532 seconds time elapsed

       0.011612000 seconds user
       1.469025000 seconds sys


Average read latency for 20000 repetitions: 388727 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          1,432.26 msec task-clock                       #    0.184 CPUs utilized             
            19,943      context-switches                 #   13.924 K/sec                     
                73      cpu-migrations                   #   50.968 /sec                      
                63      page-faults                      #   43.986 /sec                      

       7.783611581 seconds time elapsed

       0.016797000 seconds user
       1.544048000 seconds sys


Average read latency for 20000 repetitions: 381121 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          1,356.66 msec task-clock                       #    0.178 CPUs utilized             
            19,971      context-switches                 #   14.721 K/sec                     
                65      cpu-migrations                   #   47.912 /sec                      
                64      page-faults                      #   47.175 /sec                      

       7.632458204 seconds time elapsed

       0.008692000 seconds user
       1.474017000 seconds sys


Average read latency for 20000 repetitions: 389155 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          1,456.84 msec task-clock                       #    0.187 CPUs utilized             
            19,963      context-switches                 #   13.703 K/sec                     
                62      cpu-migrations                   #   42.558 /sec                      
                62      page-faults                      #   42.558 /sec                      

       7.792608625 seconds time elapsed

       0.009875000 seconds user
       1.607231000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

Количество переключений контекста:

- Вынужденные: 2432, 2437, 2380, 2478
- Невынужденные: 2, 3, 2, 1 В секунду

#### iostat

`iostat -d -x sda 1`

```
r/s      rKb/s    w/s  r_await  wkB/s w_await f/s  f_await %util
10698.00 10698.00 0.00 0.34     0.00  0.00    0.00 0.00    78.00
```

### old loader, 8

`./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 & ./io_loader ../code/test.bin 20000 &`

#### time

```
Average read latency for 20000 repetitions: 426247 ns

real    0m8.535s
user    0m0.017s
sys     0m1.883s
Average read latency for 20000 repetitions: 423037 ns

real    0m8.473s
user    0m0.017s
sys     0m1.915s
Average read latency for 20000 repetitions: 403242 ns

real    0m8.078s
user    0m0.011s
sys     0m2.341s
Average read latency for 20000 repetitions: 396547 ns

real    0m7.941s
user    0m0.015s
sys     0m2.166s
Average read latency for 20000 repetitions: 426443 ns

real    0m8.540s
user    0m0.032s
sys     0m1.874s
Average read latency for 20000 repetitions: 483775 ns

real    0m9.687s
user    0m0.011s
sys     0m2.972s
Average read latency for 20000 repetitions: 436442 ns

real    0m8.741s
user    0m0.015s
sys     0m2.028s
Average read latency for 20000 repetitions: 498105 ns

real    0m9.974s
user    0m0.016s
sys     0m3.294s
```

#### top

```
%Cpu(s): 2.5 us, 23.8 sy, 0.0 ni, 0.1 id, 20.9 wa, 0.0 hi, 52.7 si, 0.0 st
MiB Mem: 7751.4 total, 3267.3 free, 2202.9 used, 2655.7 buff/cache
MiB Swap: 0.0 total, 0.0 free, 0.0 used, 5548.5 avail Mem

PID   USER     PR NI VIRT RES  SHR  S %CPU  %MEM TIME+   COMMAND
36984 graevsky 20 0  0.0t 0.0t 0.0t R 38.0  0.0  0:01.59 io_loader
36985 graevsky 20 0  0.0t 0.0t 0.0t D 33.7  0.0  0:01.63 io_loader
36982 graevsky 20 0  0.0t 0.0t 0.0t D 28.3  0.0  0:01.30 io_loader
36980 graevsky 20 0  0.0t 0.0t 0.0t D 24.3  0.0  0:01.20 io_loader
36986 graevsky 20 0  0.0t 0.0t 0.0t D 24.0  0.0  0:01.20 io_loader
36979 graevsky 20 0  0.0t 0.0t 0.0t D 23.7  0.0  0:01.19 io_loader
36981 graevsky 20 0  0.0t 0.0t 0.0t D 23.7  0.0  0:01.15 io_loader
36983 graevsky 20 0  0.0t 0.0t 0.0t R 23.3  0.0  0:01.14 io_loader
```

#### perf

```
Average read latency for 20000 repetitions: 570469 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          2,763.01 msec task-clock                       #    0.242 CPUs utilized             
            19,990      context-switches                 #    7.235 K/sec                     
                19      cpu-migrations                   #    6.877 /sec                      
                64      page-faults                      #   23.163 /sec                      

      11.433661498 seconds time elapsed

       0.010505000 seconds user
       2.860563000 seconds sys


Average read latency for 20000 repetitions: 562062 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          2,733.61 msec task-clock                       #    0.243 CPUs utilized             
            19,980      context-switches                 #    7.309 K/sec                     
                31      cpu-migrations                   #   11.340 /sec                      
                63      page-faults                      #   23.046 /sec                      

      11.251425381 seconds time elapsed

       0.018284000 seconds user
       2.804588000 seconds sys


Average read latency for 20000 repetitions: 611532 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          4,200.70 msec task-clock                       #    0.343 CPUs utilized             
            19,988      context-switches                 #    4.758 K/sec                     
                23      cpu-migrations                   #    5.475 /sec                      
                62      page-faults                      #   14.759 /sec                      

      12.241508000 seconds time elapsed

       0.018765000 seconds user
       4.324440000 seconds sys


Average read latency for 20000 repetitions: 590017 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          3,576.30 msec task-clock                       #    0.303 CPUs utilized             
            19,989      context-switches                 #    5.589 K/sec                     
                16      cpu-migrations                   #    4.474 /sec                      
                64      page-faults                      #   17.896 /sec                      

      11.811082476 seconds time elapsed

       0.017964000 seconds user
       3.680547000 seconds sys


Average read latency for 20000 repetitions: 545148 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          2,611.02 msec task-clock                       #    0.239 CPUs utilized             
            19,987      context-switches                 #    7.655 K/sec                     
                30      cpu-migrations                   #   11.490 /sec                      
                63      page-faults                      #   24.129 /sec                      

      10.914649966 seconds time elapsed

       0.016735000 seconds user
       2.682876000 seconds sys


Average read latency for 20000 repetitions: 546408 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          2,657.68 msec task-clock                       #    0.243 CPUs utilized             
            20,001      context-switches                 #    7.526 K/sec                     
                29      cpu-migrations                   #   10.912 /sec                      
                62      page-faults                      #   23.329 /sec                      

      10.935603420 seconds time elapsed

       0.008202000 seconds user
       2.727411000 seconds sys


Average read latency for 20000 repetitions: 548560 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          2,618.92 msec task-clock                       #    0.238 CPUs utilized             
            20,002      context-switches                 #    7.638 K/sec                     
                33      cpu-migrations                   #   12.601 /sec                      
                64      page-faults                      #   24.438 /sec                      

      10.986930219 seconds time elapsed

       0.027076000 seconds user
       2.686577000 seconds sys


Average read latency for 20000 repetitions: 598150 ns

 Performance counter stats for './io_loader ../code/test.bin 20000':

          3,584.29 msec task-clock                       #    0.299 CPUs utilized             
            19,989      context-switches                 #    5.577 K/sec                     
                36      cpu-migrations                   #   10.044 /sec                      
                63      page-faults                      #   17.577 /sec                      

      11.972595004 seconds time elapsed

       0.016687000 seconds user
       3.682812000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

Количество переключений контекста:

- Вынужденные: 856, 1021, 1427, 1194, 222, 1031, 1140, 1178
- Невынужденные: 3, 3, 5, 2, 3, 1, 1, 5 В секунду

#### iostat

`iostat -d -x sda 1`

```
r/s      rKb/s    w/s  r_await  wkB/s w_await f/s  f_await %util
14026.00 14026.00 0.00 0.51     0.00  0.00    0.00 0.00    61.10
```

### new loader, 1

#### time

```
Average read latency for 20000 repetitions: 70 ns

real    0m0.007s
user    0m0.002s
sys     0m0.003s
```

#### top

```
слишком быстро(
```

#### perf

```
Average read latency for 20000 repetitions: 61 ns

 Performance counter stats for './loader test.bin 20000':

              2.89 msec task-clock                       #    0.737 CPUs utilized             
                 4      context-switches                 #    1.385 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               136      page-faults                      #   47.086 K/sec                     

       0.003919541 seconds time elapsed

       0.003236000 seconds user
       0.000000000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

`Слишком быстро ;c`

#### iostat

`iostat -d -x sda 1`

```
r/s  rKb/s w/s   wkB/s  w_await f/s   f_await %util
1.00 1.00  25.00 224.00 0.50    13.00 0.46    1.10
```

### new loader, 2

`./loader test.bin 20000 & ./loader test.bin 20000 &`

#### time

```
Average read latency for 20000 repetitions: 75 ns

real    0m0.004s
user    0m0.003s
sys     0m0.000s
Average read latency for 20000 repetitions: 58 ns

real  `  0m0.004s
user    0m0.000s
sys     0m0.003
```

#### top

```
Слишком быстро ;c
```

#### perf

```
Average read latency for 20000 repetitions: 75 ns

 Performance counter stats for './loader test.bin 20000':

              2.65 msec task-clock                       #    0.655 CPUs utilized             
                 4      context-switches                 #    1.508 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               138      page-faults                      #   52.026 K/sec                     

       0.004050117 seconds time elapsed

       0.001634000 seconds user
       0.001634000 seconds sys


Average read latency for 20000 repetitions: 71 ns

 Performance counter stats for './loader test.bin 20000':

              3.07 msec task-clock                       #    0.686 CPUs utilized             
                 2      context-switches                 #  651.674 /sec                      
                 1      cpu-migrations                   #  325.837 /sec                      
               136      page-faults                      #   44.314 K/sec                     

       0.004473028 seconds time elapsed

       0.002558000 seconds user
       0.000852000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

```
Слишком быстро ;c
```

#### iostat

`iostat -d -x sda 1`

```
r/s     rKb/s   w/s  r_await  wkB/s w_await f/s  f_await %util
2.00 2.00  21.00 148.00 0.48    8.00  0.38    0.80
```

### new loader, 4

`./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 &`

#### time

```
Average read latency for 20000 repetitions: 64 ns

real    0m0.004s
user    0m0.003s
sys     0m0.000s
Average read latency for 20000 repetitions: 84 ns

real    0m0.004s
user    0m0.003s
sys     0m0.000s
Average read latency for 20000 repetitions: 66 ns

real    0m0.004s
user    0m0.003s
sys     0m0.000s
Average read latency for 20000 repetitions: 63 ns

real    0m0.004s
user    0m0.003s
sys     0m0.000s
```

#### top

```
Слишком быстро ;c
```

#### perf

```
Average read latency for 20000 repetitions: 81 ns

 Performance counter stats for './loader test.bin 20000':

              2.56 msec task-clock                       #    0.504 CPUs utilized             
                 3      context-switches                 #    1.172 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               136      page-faults                      #   53.113 K/sec                     

       0.005081279 seconds time elapsed

       0.002940000 seconds user
       0.000000000 seconds sys


Average read latency for 20000 repetitions: 62 ns

 Performance counter stats for './loader test.bin 20000':

              2.94 msec task-clock                       #    0.629 CPUs utilized             
                 2      context-switches                 #  680.517 /sec                      
                 0      cpu-migrations                   #    0.000 /sec                      
               137      page-faults                      #   46.615 K/sec                     

       0.004670295 seconds time elapsed

       0.003197000 seconds user
       0.000000000 seconds sys


Average read latency for 20000 repetitions: 88 ns

 Performance counter stats for './loader test.bin 20000':

              2.67 msec task-clock                       #    0.502 CPUs utilized             
                 3      context-switches                 #    1.122 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               138      page-faults                      #   51.597 K/sec                     

       0.005330355 seconds time elapsed

       0.002075000 seconds user
       0.001037000 seconds sys


Average read latency for 20000 repetitions: 62 ns

 Performance counter stats for './loader test.bin 20000':

              2.57 msec task-clock                       #    0.568 CPUs utilized             
                 5      context-switches                 #    1.949 K/sec                     
                 1      cpu-migrations                   #  389.747 /sec                      
               138      page-faults                      #   53.785 K/sec                     

       0.004518131 seconds time elapsed

       0.003015000 seconds user
       0.000000000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

```
Слишком быстро ;c
```

#### iostat

`iostat -d -x sda 1`

```
r/s  rKb/s w/s   r_await wkB/s  w_await f/s   f_await %util
4.00 4.00  22.00 0.50    156.00 0.55    12.00  0.42   0.60
```

### new loader, 8

`./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 & ./loader test.bin 20000 &`

#### time

```
Average read latency for 20000 repetitions: 125 ns

real    0m0.009s
user    0m0.002s
sys     0m0.001s
Average read latency for 20000 repetitions: 146 ns

real    0m0.009s
user    0m0.002s
sys     0m0.001s
Average read latency for 20000 repetitions: 358 ns

real    0m0.011s
user    0m0.004s
sys     0m0.001s
Average read latency for 20000 repetitions: 246 ns

real    0m0.009s
user    0m0.002s
sys     0m0.001s
Average read latency for 20000 repetitions: 104 ns

real    0m0.013s
user    0m0.003s
sys     0m0.004s
Average read latency for 20000 repetitions: 253 ns

real    0m0.008s
user    0m0.003s
sys     0m0.000s
Average read latency for 20000 repetitions: 57 ns

real    0m0.008s
user    0m0.002s
sys     0m0.002s
Average read latency for 20000 repetitions: 134 ns

real    0m0.006s
user    0m0.003s
sys     0m0.000s
```

#### top

```
Слишком быстро ;c
```

#### perf

```
Average read latency for 20000 repetitions: 58 ns

 Performance counter stats for './loader test.bin 20000':

              2.56 msec task-clock                       #    0.455 CPUs utilized             
                 4      context-switches                 #    1.563 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               138      page-faults                      #   53.917 K/sec                     

       0.005621856 seconds time elapsed

       0.001410000 seconds user
       0.001410000 seconds sys


Average read latency for 20000 repetitions: 107 ns

 Performance counter stats for './loader test.bin 20000':

              2.69 msec task-clock                       #    0.440 CPUs utilized             
                 2      context-switches                 #  743.501 /sec                      
                 1      cpu-migrations                   #  371.750 /sec                      
               137      page-faults                      #   50.930 K/sec                     

       0.006111811 seconds time elapsed

       0.002888000 seconds user
       0.000000000 seconds sys


Average read latency for 20000 repetitions: 134 ns

 Performance counter stats for './loader test.bin 20000':

              2.75 msec task-clock                       #    0.368 CPUs utilized             
                 3      context-switches                 #    1.090 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               137      page-faults                      #   49.797 K/sec                     

       0.007471675 seconds time elapsed

       0.002004000 seconds user
       0.001002000 seconds sys


Average read latency for 20000 repetitions: 104 ns

 Performance counter stats for './loader test.bin 20000':

              3.11 msec task-clock                       #    0.445 CPUs utilized             
                 5      context-switches                 #    1.607 K/sec                     
                 1      cpu-migrations                   #  321.327 /sec                      
               137      page-faults                      #   44.022 K/sec                     

       0.007000303 seconds time elapsed

       0.002982000 seconds user
       0.000745000 seconds sys


Average read latency for 20000 repetitions: 182 ns

 Performance counter stats for './loader test.bin 20000':

              2.66 msec task-clock                       #    0.785 CPUs utilized             
                 5      context-switches                 #    1.882 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               137      page-faults                      #   51.572 K/sec                     

       0.003386194 seconds time elapsed

       0.002862000 seconds user
       0.000000000 seconds sys


Average read latency for 20000 repetitions: 107 ns

 Performance counter stats for './loader test.bin 20000':

              3.04 msec task-clock                       #    0.532 CPUs utilized             
                 5      context-switches                 #    1.643 K/sec                     
                 1      cpu-migrations                   #  328.563 /sec                      
               136      page-faults                      #   44.685 K/sec                     

       0.005725600 seconds time elapsed

       0.001714000 seconds user
       0.001714000 seconds sys


Average read latency for 20000 repetitions: 107 ns

 Performance counter stats for './loader test.bin 20000':

              2.76 msec task-clock                       #    0.484 CPUs utilized             
                 6      context-switches                 #    2.177 K/sec                     
                 2      cpu-migrations                   #  725.656 /sec                      
               138      page-faults                      #   50.070 K/sec                     

       0.005690237 seconds time elapsed

       0.001560000 seconds user
       0.001560000 seconds sys


Average read latency for 20000 repetitions: 73 ns

 Performance counter stats for './loader test.bin 20000':

              2.64 msec task-clock                       #    0.792 CPUs utilized             
                 4      context-switches                 #    1.516 K/sec                     
                 0      cpu-migrations                   #    0.000 /sec                      
               137      page-faults                      #   51.913 K/sec                     

       0.003332780 seconds time elapsed

       0.001439000 seconds user
       0.001439000 seconds sys
```

#### pidstat

`pidstat -w -p ALL 1`

```
Слишком быстро ;c
```

#### iostat

`iostat -d -x sda 1`

```
r/s  rKb/s w/s   r_await wkB/s  w_await f/s   f_await %util
8.00 8.00  24.00 0.38    144.00 0.42    14.00  0.36   1.30
```

## Выводы

### Анализ результатов

#### Старый загрузчик:

- _Время выполнения_ существенно зависит от числа параллельных процессов. При увеличении числа процессов с 1 до 8 время выполнения увеличивается незначительно, но среднее время чтения (Average read latency) возрастает от ~380,000 ns до ~570,000 ns.
- _CPU Utilization:_ При запуске 8 процессов, суммарное использование CPU составляет около 200%, что свидетельствует о высокой нагрузке на систему.
- _I/O статистика:_ Показатели r/s и %util высоки, что указывает на интенсивное использование дисковой подсистемы и возможное узкое место в производительности диска.
- _Переключения контекста:_ Увеличение количества вынужденных переключений контекста с ростом числа процессов, что негативно влияет на производительность.

#### Новый загрузчик:

- _Время выполнения_ значительно меньше по сравнению со старым загрузчиком. При запуске даже 8 процессов, время выполнения остается в пределах нескольких миллисекунд, а Average read latency не превышает 358 ns.
- _CPU Utilization:_ Низкое использование CPU даже при увеличении числа процессов, что свидетельствует о более эффективном использовании ресурсов.
- _I/O статистика:_ Показатели r/s и %util значительно ниже, что говорит о меньшей нагрузке на дисковую подсистему.
- _Переключения контекста:_ Незначительное количество переключений контекста, что положительно сказывается на производительности.

### Заключение

- Скорость работы: Новый загрузчик выполняет задачи на порядки быстрее, сокращая среднее время чтения с сотен тысяч наносекунд до десятков-сотен наносекунд.
- Эффективность использования ресурсов: Новый загрузчик более эффективно использует CPU и дисковую подсистему, что позволяет снизить нагрузку на систему и уменьшить время выполнения задач.
- Масштабируемость: При увеличении числа параллельных процессов, новый загрузчик сохраняет высокую производительность, в то время как у старого загрузчика наблюдается деградация производительности.
