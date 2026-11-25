/*
 * HTTP block device
 * 
 * Copyright (c) 2016-2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>

#include "cutils.h"
#include "virtio.h"
#include "fs_wget.h"
#include "list.h"
#include "fbuf.h"
#include "machine.h"
#include "snapshot.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

typedef enum {
    CBLOCK_LOADING,
    CBLOCK_LOADED,
} CachedBlockStateEnum;

typedef struct CachedBlock {
    struct list_head link;
    struct BlockDeviceHTTP *bf;
    unsigned int block_num;
    CachedBlockStateEnum state;
    BOOL prefetch; /* loading in the background, not for a guest request */
    FileBuffer fbuf;
} CachedBlock;

#define BLK_FMT "%sblk%09u.bin"
#define GROUP_FMT "%sgrp%09u.bin"
#define PREFETCH_GROUP_LEN_MAX 32

typedef struct {
    struct BlockDeviceHTTP *bf;
    int group_num;
    int n_block_num;
    CachedBlock *tab_block[PREFETCH_GROUP_LEN_MAX];
} PrefetchGroupRequest;

/* modified data is stored per cluster (smaller than cached blocks to
   avoid losing space) */
typedef struct Cluster {
    FileBuffer fbuf;
} Cluster;

typedef struct BlockDeviceHTTP {
    BlockDevice *bs;
    int max_cache_size_kb;
    char url[1024];
    char disk_id[1100];
    int prefetch_count;
    void (*start_cb)(void *opaque);
    void *start_opaque;
    
    int64_t nb_sectors;
    int block_size; /* in sectors, power of two */
    int nb_blocks;
    uint8_t *zero_map; /* bit set: the block is all zeros and not served */
    struct list_head cached_blocks; /* list of CachedBlock */
    int n_cached_blocks;
    int n_cached_blocks_max;

    /* write support */
    int sectors_per_cluster; /* power of two */
    Cluster **clusters; /* NULL if no written data */
    int n_clusters;
    int n_allocated_clusters;
    
    /* statistics */
    int64_t n_read_sectors;
    int64_t n_read_blocks;
    int64_t n_write_sectors;

    /* current read request */
    BOOL is_write;
    uint64_t sector_num;
    int cur_block_num;
    int sector_index, sector_count;
    BlockDeviceCompletionFunc *cb;
    void *opaque;
    uint8_t *io_buf;

    /* prefetch */
    int prefetch_group_len;
    /* with prefetch_group_len == 1, the "prefetch" blocks are loaded in
       the background in list order, at most prefetch_parallel at a time,
       while guest requests are served as soon as they come */
    int *prefetch_queue;
    int prefetch_len, prefetch_pos;
    int prefetch_parallel, prefetch_in_flight;

    /* snapshot: "snapshot" in blk.txt names a saved machine to restore
       instead of booting (relative to blk.txt). The background loads then
       wait until the frontend has restored it (or decided not to), so the
       two downloads do not compete. */
    char snapshot_url[1100];
    BOOL prefetch_started;
    /* after a restore the guest needs few blocks, but needs them now: the
       background loads then yield to it (block_net_start_prefetch()) */
    BOOL yield_to_guest;
    int64_t guest_wait_ms; /* last time a guest request waited for a block */
    BOOL pump_timer;
    /* the clusters came from a snapshot and are not persisted yet: they
       are, with the first write of the guest (until then the disk is the
       pristine image plus the snapshot, and the next visit can restore the
       snapshot again) */
    BOOL persist_on_write;
} BlockDeviceHTTP;

static void bf_update_block(CachedBlock *b, const uint8_t *data);
static void bf_init_done(BlockDeviceHTTP *bf);
static void bf_prefetch_pump(BlockDeviceHTTP *bf);
static int bf_save_writes(BlockDevice *bs, uint8_t *buf, int buf_size);
#ifdef __EMSCRIPTEN__
static void bf_persist_all_clusters(BlockDeviceHTTP *bf);
#endif
static int bf_load_writes(BlockDevice *bs, const uint8_t *buf, int len,
                          BOOL check_only);
#ifdef __EMSCRIPTEN__
/* provided in lib.js: keep written clusters in the browser (IndexedDB) so
   that they survive a page reload */
extern void block_persist_open(BlockDeviceHTTP *bf, const char *disk_id,
                               int cluster_bytes);
extern void block_persist_dirty(int cluster_num, int fbuf_handle);
/* provided in lib.js: tell the page what the disk image looks like, so it
   can report the download of its blocks */
extern void block_net_info(const char *url, int block_bytes, int n_blocks,
                           int n_zero_blocks, int n_prefetch_blocks);
#endif
static void bf_read_onload(void *opaque, int err, void *data, size_t size);
static void bf_init_onload(void *opaque, int err, void *data, size_t size);
static void bf_prefetch_group_onload(void *opaque, int err, void *data,
                                     size_t size);

static CachedBlock *bf_find_block(BlockDeviceHTTP *bf, unsigned int block_num)
{
    CachedBlock *b;
    struct list_head *el;
    
    list_for_each(el, &bf->cached_blocks) {
        b = list_entry(el, CachedBlock, link);
        if (b->block_num == block_num) {
            /* move to front */
            if (bf->cached_blocks.next != el) {
                list_del(&b->link);
                list_add(&b->link, &bf->cached_blocks);
            }
            return b;
        }
    }
    return NULL;
}

static void bf_free_block(BlockDeviceHTTP *bf, CachedBlock *b)
{
    bf->n_cached_blocks--;
    file_buffer_reset(&b->fbuf);
    list_del(&b->link);
    free(b);
}

static CachedBlock *bf_add_block(BlockDeviceHTTP *bf, unsigned int block_num)
{
    CachedBlock *b;
    if (bf->n_cached_blocks >= bf->n_cached_blocks_max) {
        struct list_head *el, *el1;
        /* start by looking at the least unused blocks */
        list_for_each_prev_safe(el, el1, &bf->cached_blocks) {
            b = list_entry(el, CachedBlock, link);
            if (b->state == CBLOCK_LOADED) {
                bf_free_block(bf, b);
                if (bf->n_cached_blocks < bf->n_cached_blocks_max)
                    break;
            }
        }
    }
    b = mallocz(sizeof(CachedBlock));
    b->bf = bf;
    b->block_num = block_num;
    b->state = CBLOCK_LOADING;
    file_buffer_init(&b->fbuf);
    file_buffer_resize(&b->fbuf, bf->block_size * 512);
    list_add(&b->link, &bf->cached_blocks);
    bf->n_cached_blocks++;
    return b;
}

static int64_t bf_get_sector_count(BlockDevice *bs)
{
    BlockDeviceHTTP *bf = bs->opaque;
    return bf->nb_sectors;
}

static CachedBlock *bf_start_load_block(BlockDevice *bs, int block_num)
{
    BlockDeviceHTTP *bf = bs->opaque;
    char filename[1024];
    CachedBlock *b;
    b = bf_add_block(bf, block_num);
    bf->n_read_blocks++;
    /* make a XHR to read the block */
#if 0
    printf("%u,\n", block_num);
#endif
#if 0
    printf("load_blk=%d cached=%d read=%d KB (%d KB) write=%d KB (%d KB)\n",
           block_num, bf->n_cached_blocks,
           (int)(bf->n_read_sectors / 2),
           (int)(bf->n_read_blocks * bf->block_size / 2),
           (int)(bf->n_write_sectors / 2),
           (int)(bf->n_allocated_clusters * bf->sectors_per_cluster / 2));
#endif
    snprintf(filename, sizeof(filename), BLK_FMT, bf->url, block_num);
    fs_wget(filename, NULL, NULL, b, bf_read_onload, TRUE);
    return b;
}

static BOOL bf_block_is_zero(BlockDeviceHTTP *bf, int block_num);

#define YIELD_MS 300 /* quiet time after a guest wait before loading more */

static int64_t bf_time_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

#ifdef __EMSCRIPTEN__
static void bf_prefetch_pump_timer(void *opaque)
{
    BlockDeviceHTTP *bf = opaque;
    bf->pump_timer = FALSE;
    bf_prefetch_pump(bf);
}
#endif

/* start background loads until prefetch_parallel are in flight. Blocks
   that are cached or loading already (because the guest asked for them)
   are skipped, and nothing is evicted from the cache to make room. */
static void bf_prefetch_pump(BlockDeviceHTTP *bf)
{
    CachedBlock *b;
    int block_num;

    if (!bf->prefetch_started)
        return;
#ifdef __EMSCRIPTEN__
    if (bf->yield_to_guest) {
        int64_t quiet;
        /* the guest waits for a block: its load restarts the pump */
        if (bf->cur_block_num >= 0)
            return;
        quiet = bf_time_ms() - bf->guest_wait_ms;
        if (quiet < YIELD_MS) {
            if (!bf->pump_timer) {
                bf->pump_timer = TRUE;
                emscripten_async_call(bf_prefetch_pump_timer, bf,
                                      YIELD_MS - quiet);
            }
            return;
        }
    }
#endif

    while (bf->prefetch_in_flight < bf->prefetch_parallel &&
           bf->prefetch_pos < bf->prefetch_len &&
           bf->n_cached_blocks < bf->n_cached_blocks_max) {
        block_num = bf->prefetch_queue[bf->prefetch_pos++];
        if (bf_block_is_zero(bf, block_num) || bf_find_block(bf, block_num))
            continue;
        b = bf_start_load_block(bf->bs, block_num);
        b->prefetch = TRUE;
        bf->prefetch_in_flight++;
    }
}

static void bf_start_load_prefetch_group(BlockDevice *bs, int group_num,
                                         const int *tab_block_num,
                                         int n_block_num)
{
    BlockDeviceHTTP *bf = bs->opaque;
    CachedBlock *b;
    PrefetchGroupRequest *req;
    char filename[1024];
    BOOL req_flag;
    int i;
    
    req_flag = FALSE;
    req = malloc(sizeof(*req));
    req->bf = bf;
    req->group_num = group_num;
    req->n_block_num = n_block_num;
    for(i = 0; i < n_block_num; i++) {
        b = bf_find_block(bf, tab_block_num[i]);
        if (!b) {
            b = bf_add_block(bf, tab_block_num[i]);
            req_flag = TRUE;
        } else {
            /* no need to read the block if it is already loading or
               loaded */
            b = NULL;
        }
        req->tab_block[i] = b;
    }

    if (req_flag) {
        snprintf(filename, sizeof(filename), GROUP_FMT, bf->url, group_num);
        //        printf("wget %s\n", filename);
        fs_wget(filename, NULL, NULL, req, bf_prefetch_group_onload, TRUE);
        /* XXX: should add request in a list to free it for clean exit */
    } else {
        free(req);
    }
}

static void bf_prefetch_group_onload(void *opaque, int err, void *data,
                                     size_t size)
{
    PrefetchGroupRequest *req = opaque;
    BlockDeviceHTTP *bf = req->bf;
    CachedBlock *b;
    int block_bytes, i;
    
    if (err < 0) {
        fprintf(stderr, "Could not load group %u\n", req->group_num);
        exit(1);
    }
    block_bytes = bf->block_size * 512;
    assert(size == block_bytes * req->n_block_num);
    for(i = 0; i < req->n_block_num; i++) {
        b = req->tab_block[i];
        if (b) {
            bf_update_block(b, (const uint8_t *)data + block_bytes * i);
        }
    }
    free(req);
}

static BOOL bf_block_is_zero(BlockDeviceHTTP *bf, int block_num)
{
    return bf->zero_map &&
        ((bf->zero_map[block_num >> 3] >> (block_num & 7)) & 1);
}

static int bf_rw_async1(BlockDevice *bs, BOOL is_sync)
{
    BlockDeviceHTTP *bf = bs->opaque;
    int offset, block_num, n, cluster_num;
    CachedBlock *b;
    Cluster *c;
    
    for(;;) {
        n = bf->sector_count - bf->sector_index;
        if (n == 0)
            break;
        cluster_num = bf->sector_num / bf->sectors_per_cluster;
        c = bf->clusters[cluster_num];
        if (c) {
            offset = bf->sector_num % bf->sectors_per_cluster;
            n = min_int(n, bf->sectors_per_cluster - offset);
            if (bf->is_write) {
                file_buffer_write(&c->fbuf, offset * 512,
                                  bf->io_buf + bf->sector_index * 512, n * 512);
#ifdef __EMSCRIPTEN__
                if (bf->persist_on_write)
                    bf_persist_all_clusters(bf);
                block_persist_dirty(cluster_num, c->fbuf.handle);
#endif
            } else {
                file_buffer_read(&c->fbuf, offset * 512,
                                 bf->io_buf + bf->sector_index * 512, n * 512);
            }
            bf->sector_index += n;
            bf->sector_num += n;
        } else {
            /* stop at the end of this cluster: the next one may hold
               data the guest has written, which the block does not */
            offset = bf->sector_num % bf->sectors_per_cluster;
            n = min_int(n, bf->sectors_per_cluster - offset);
            block_num = bf->sector_num / bf->block_size;
            offset = bf->sector_num % bf->block_size;
            n = min_int(n, bf->block_size - offset);
            bf->cur_block_num = block_num;
            
            if (bf_block_is_zero(bf, block_num)) {
                /* known to be all zeros: no need to fetch or cache it */
                if (bf->is_write) {
                    int cluster_size = bf->sectors_per_cluster * 512;
                    uint8_t *zeros = mallocz(cluster_size);
                    c = mallocz(sizeof(Cluster));
                    file_buffer_init(&c->fbuf);
                    file_buffer_resize(&c->fbuf, cluster_size);
                    file_buffer_write(&c->fbuf, 0, zeros, cluster_size);
                    free(zeros);
                    bf->clusters[cluster_num] = c;
                    bf->n_allocated_clusters++;
                    continue; /* write to the allocated cluster */
                }
                memset(bf->io_buf + bf->sector_index * 512, 0, n * 512);
                bf->sector_index += n;
                bf->sector_num += n;
                bf->cur_block_num = -1;
                continue;
            }

            b = bf_find_block(bf, block_num);
            if (b) {
                if (b->state == CBLOCK_LOADING) {
                    /* wait until the block is loaded */
                    bf->guest_wait_ms = bf_time_ms();
                    return 1;
                } else {
                    if (bf->is_write) {
                        int cluster_size, cluster_offset;
                        uint8_t *buf;
                        /* allocate a new cluster */
                        c = mallocz(sizeof(Cluster));
                        cluster_size = bf->sectors_per_cluster * 512;
                        buf = malloc(cluster_size);
                        file_buffer_init(&c->fbuf);
                        file_buffer_resize(&c->fbuf, cluster_size);
                        bf->clusters[cluster_num] = c;
                        /* copy the cached block data to the cluster */
                        cluster_offset = (cluster_num * bf->sectors_per_cluster) &
                            (bf->block_size - 1);
                        file_buffer_read(&b->fbuf, cluster_offset * 512,
                                         buf, cluster_size);
                        file_buffer_write(&c->fbuf, 0, buf, cluster_size);
                        free(buf);
                        bf->n_allocated_clusters++;
                        continue; /* write to the allocated cluster */
                    } else {
                        file_buffer_read(&b->fbuf, offset * 512,
                                         bf->io_buf + bf->sector_index * 512, n * 512);
                    }
                    bf->sector_index += n;
                    bf->sector_num += n;
                }
            } else {
                bf->guest_wait_ms = bf_time_ms();
                bf_start_load_block(bs, block_num);
                return 1;
            }
            bf->cur_block_num = -1;
        }
    }

    if (!is_sync) {
        //        printf("end of request\n");
        /* end of request */
        bf->cb(bf->opaque, 0);
    } 
    return 0;
}

static void bf_update_block(CachedBlock *b, const uint8_t *data)
{
    BlockDeviceHTTP *bf = b->bf;
    BlockDevice *bs = bf->bs;

    assert(b->state == CBLOCK_LOADING);
    file_buffer_write(&b->fbuf, 0, data, bf->block_size * 512);
    b->state = CBLOCK_LOADED;
    
    /* continue I/O read/write if necessary */
    if (b->block_num == bf->cur_block_num) {
        bf_rw_async1(bs, FALSE);
    }
}

static void bf_read_onload(void *opaque, int err, void *data, size_t size)
{
    CachedBlock *b = opaque;
    BlockDeviceHTTP *bf = b->bf;

    if (b->prefetch) {
        b->prefetch = FALSE;
        bf->prefetch_in_flight--;
    }
    if (err == -404) {
        /* a block that is not served is all zeros: this lets the server
           skip the empty parts of a disk image */
        void *zeros = mallocz(bf->block_size * 512);
        bf_update_block(b, zeros);
        free(zeros);
    } else if (err < 0) {
        fprintf(stderr, "Could not load block %u\n", b->block_num);
        exit(1);
    } else {
        assert(size == bf->block_size * 512);
        bf_update_block(b, data);
    }
    bf_prefetch_pump(bf);
}

static int bf_read_async(BlockDevice *bs,
                         uint64_t sector_num, uint8_t *buf, int n,
                         BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceHTTP *bf = bs->opaque;
    //    printf("bf_read_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
    bf->is_write = FALSE;
    bf->sector_num = sector_num;
    bf->io_buf = buf;
    bf->sector_count = n;
    bf->sector_index = 0;
    bf->cb = cb;
    bf->opaque = opaque;
    bf->n_read_sectors += n;
    return bf_rw_async1(bs, TRUE);
}

static int bf_write_async(BlockDevice *bs,
                          uint64_t sector_num, const uint8_t *buf, int n,
                          BlockDeviceCompletionFunc *cb, void *opaque)
{
    BlockDeviceHTTP *bf = bs->opaque;
    //    printf("bf_write_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
    bf->is_write = TRUE;
    bf->sector_num = sector_num;
    bf->io_buf = (uint8_t *)buf;
    bf->sector_count = n;
    bf->sector_index = 0;
    bf->cb = cb;
    bf->opaque = opaque;
    bf->n_write_sectors += n;
    return bf_rw_async1(bs, TRUE);
}

BlockDevice *block_device_init_http(const char *url,
                                    int max_cache_size_kb,
                                    void (*start_cb)(void *opaque),
                                    void *start_opaque)
{
    BlockDevice *bs;
    BlockDeviceHTTP *bf;
    char *p;

    bs = mallocz(sizeof(*bs));
    bf = mallocz(sizeof(*bf));
    strcpy(bf->url, url);
    /* get the path with the trailing '/' */
    p = strrchr(bf->url, '/');
    if (!p)
        p = bf->url;
    else
        p++;
    *p = '\0';

    init_list_head(&bf->cached_blocks);
    bf->max_cache_size_kb = max_cache_size_kb;
    bf->start_cb = start_cb;
    bf->start_opaque = start_opaque;
    bf->bs = bs;
    
    bs->opaque = bf;
    bs->get_sector_count = bf_get_sector_count;
    bs->read_async = bf_read_async;
    bs->write_async = bf_write_async;
    bs->save_writes = bf_save_writes;
    bs->load_writes = bf_load_writes;
    
    fs_wget(url, NULL, NULL, bs, bf_init_onload, TRUE);
    return bs;
}

static void bf_init_onload(void *opaque, int err, void *data, size_t size)
{
    BlockDevice *bs = opaque;
    BlockDeviceHTTP *bf = bs->opaque;
    int block_size_kb;
    JSONValue cfg, array;
    
    if (err < 0) {
        fprintf(stderr, "Could not load block device file (err=%d)\n", -err);
        exit(1);
    }

    /* parse the disk image info */
    cfg = json_parse_value_len(data, size);
    if (json_is_error(cfg)) {
        vm_error("error: %s\n", json_get_error(cfg));
    config_error:
        json_free(cfg);
        exit(1);
    }

    if (vm_get_int(cfg, "block_size", &block_size_kb) < 0)
        goto config_error;
    bf->block_size = block_size_kb * 2;
    if (bf->block_size <= 0 ||
        (bf->block_size & (bf->block_size - 1)) != 0) {
        vm_error("invalid block_size\n");
        goto config_error;
    }
    if (vm_get_int(cfg, "n_block", &bf->nb_blocks) < 0)
        goto config_error;
    if (bf->nb_blocks <= 0) {
        vm_error("invalid n_block\n");
        goto config_error;
    }

    bf->nb_sectors = bf->block_size * (uint64_t)bf->nb_blocks;
    bf->n_cached_blocks = 0;
    bf->n_cached_blocks_max = max_int(1, bf->max_cache_size_kb / block_size_kb);
    bf->cur_block_num = -1; /* no request in progress */
    
    bf->sectors_per_cluster = 8; /* 4 KB */
    bf->n_clusters = (bf->nb_sectors + bf->sectors_per_cluster - 1) / bf->sectors_per_cluster;
    bf->clusters = mallocz(sizeof(bf->clusters[0]) * bf->n_clusters);

    if (vm_get_int_opt(cfg, "prefetch_group_len",
                       &bf->prefetch_group_len, 1) < 0)
        goto config_error;
    if (bf->prefetch_group_len > PREFETCH_GROUP_LEN_MAX) {
        vm_error("prefetch_group_len is too large");
        goto config_error;
    }
    
    /* optional list of [first block, count] ranges that are all zeros:
       they are not requested at all (otherwise a 404 also means zeros) */
    array = json_object_get(cfg, "zero");
    if (!json_is_undefined(array)) {
        int idx, first, count, j;
        JSONValue el;

        if (array.type != JSON_ARRAY) {
            vm_error("zero: expecting an array\n");
            goto config_error;
        }
        bf->zero_map = mallocz((bf->nb_blocks + 7) / 8);
        for(idx = 0; idx < array.u.array->len; idx++) {
            el = json_array_get(array, idx);
            if (el.type != JSON_ARRAY || el.u.array->len != 2 ||
                json_array_get(el, 0).type != JSON_INT ||
                json_array_get(el, 1).type != JSON_INT) {
                vm_error("zero: expecting [first, count] pairs\n");
                goto config_error;
            }
            first = json_array_get(el, 0).u.int32;
            count = json_array_get(el, 1).u.int32;
            if (first < 0 || count < 0 || first + count > bf->nb_blocks) {
                vm_error("zero: range out of bounds\n");
                goto config_error;
            }
            for(j = first; j < first + count; j++)
                bf->zero_map[j >> 3] |= 1 << (j & 7);
        }
    }

    if (vm_get_int_opt(cfg, "prefetch_parallel",
                       &bf->prefetch_parallel, 4) < 0)
        goto config_error;
    if (bf->prefetch_parallel < 1)
        bf->prefetch_parallel = 1;

    array = json_object_get(cfg, "prefetch");
    if (!json_is_undefined(array)) {
        int idx, prefetch_len, l, i;
        JSONValue el;
        int tab_block_num[PREFETCH_GROUP_LEN_MAX];
                          
        if (array.type != JSON_ARRAY) {
            vm_error("expecting an array\n");
            goto config_error;
        }
        prefetch_len = array.u.array->len;
        for(idx = 0; idx < prefetch_len; idx++) {
            el = json_array_get(array, idx);
            if (el.type != JSON_INT || el.u.int32 < 0 ||
                el.u.int32 >= bf->nb_blocks) {
                vm_error("prefetch: expecting block numbers\n");
                goto config_error;
            }
        }
        if (bf->prefetch_group_len == 1) {
            /* loaded in the background by bf_prefetch_pump() */
            bf->prefetch_queue = malloc(sizeof(int) * max_int(1, prefetch_len));
            for(idx = 0; idx < prefetch_len; idx++)
                bf->prefetch_queue[idx] = json_array_get(array, idx).u.int32;
            bf->prefetch_len = prefetch_len;
        } else {
            /* the groups are requested all at once */
            idx = 0;
            while (idx < prefetch_len) {
                l = min_int(prefetch_len - idx, bf->prefetch_group_len);
                for(i = 0; i < l; i++)
                    tab_block_num[i] = json_array_get(array, idx + i).u.int32;
                if (l == 1) {
                    if (!bf_find_block(bf, tab_block_num[0]))
                        bf_start_load_block(bs, tab_block_num[0]);
                } else {
                    bf_start_load_prefetch_group(bs,
                                                 idx / bf->prefetch_group_len,
                                                 tab_block_num, l);
                }
                idx += l;
            }
        }
    }
    {
        /* optional identity of the disk image: persisted writes are
           keyed by it so a new image does not inherit stale data */
        JSONValue id = json_object_get(cfg, "id");
        if (id.type == JSON_STR) {
            pstrcpy(bf->disk_id, sizeof(bf->disk_id), id.u.str->data);
        } else {
            snprintf(bf->disk_id, sizeof(bf->disk_id), "%sblk.txt", bf->url);
        }
    }
    {
        JSONValue snap = json_object_get(cfg, "snapshot");
        if (snap.type == JSON_STR) {
            snprintf(bf->snapshot_url, sizeof(bf->snapshot_url), "%s%s",
                     bf->url, snap.u.str->data);
        }
    }
    json_free(cfg);

#ifdef __EMSCRIPTEN__
    {
        int i, n_zero = 0;
        for(i = 0; i < bf->nb_blocks; i++)
            n_zero += bf_block_is_zero(bf, i);
        block_net_info(bf->url, bf->block_size * 512, bf->nb_blocks, n_zero,
                       bf->prefetch_len);
    }
#endif
    /* start the background loads: they run while the persisted clusters
       are restored and the machine boots (with a snapshot, once the
       frontend calls block_net_start_prefetch()) */
    if (!bf->snapshot_url[0]) {
        bf->prefetch_started = TRUE;
        bf_prefetch_pump(bf);
    }

#ifdef __EMSCRIPTEN__
    /* restore the clusters written in earlier sessions, then start */
    block_persist_open(bf, bf->disk_id, bf->sectors_per_cluster * 512);
#else
    bf_init_done(bf);
#endif
}

static void bf_init_done(BlockDeviceHTTP *bf)
{
    if (bf->start_cb) {
        bf->start_cb(bf->start_opaque);
    }
}

/* called from JS while restoring persisted clusters */
void block_persist_set_cluster(BlockDeviceHTTP *bf, int cluster_num,
                               const uint8_t *data)
{
    Cluster *c;
    int cluster_size;

    if (cluster_num < 0 || cluster_num >= bf->n_clusters)
        return;
    cluster_size = bf->sectors_per_cluster * 512;
    c = bf->clusters[cluster_num];
    if (!c) {
        c = mallocz(sizeof(Cluster));
        file_buffer_init(&c->fbuf);
        file_buffer_resize(&c->fbuf, cluster_size);
        bf->clusters[cluster_num] = c;
        bf->n_allocated_clusters++;
    }
    file_buffer_write(&c->fbuf, 0, data, cluster_size);
}

/* called from JS once the persisted clusters are restored (or when
   persistence is unavailable) */
void block_persist_ready(BlockDeviceHTTP *bf)
{
    bf_init_done(bf);
}

/* Snapshots */

/* the snapshot to restore instead of booting, if blk.txt names one and no
   persisted writes were restored (they would not match the snapshot's
   memory); NULL otherwise */
const char *block_net_get_snapshot_url(BlockDevice *bs)
{
    BlockDeviceHTTP *bf = bs->opaque;
    if (!bf->snapshot_url[0] || bf->n_allocated_clusters != 0)
        return NULL;
    return bf->snapshot_url;
}

/* start the background loads; with yield_to_guest (after a restore) at
   most two at a time, and none starts while a guest request waits for a
   block or within YIELD_MS of one */
void block_net_start_prefetch(BlockDevice *bs, BOOL yield_to_guest)
{
    BlockDeviceHTTP *bf = bs->opaque;
    if (yield_to_guest) {
        bf->yield_to_guest = TRUE;
        bf->prefetch_parallel = min_int(bf->prefetch_parallel, 2);
    }
    if (!bf->prefetch_started) {
        bf->prefetch_started = TRUE;
        bf_prefetch_pump(bf);
    }
}

#ifdef __EMSCRIPTEN__
static void bf_persist_all_clusters(BlockDeviceHTTP *bf)
{
    int i;
    bf->persist_on_write = FALSE;
    for(i = 0; i < bf->n_clusters; i++) {
        if (bf->clusters[i])
            block_persist_dirty(i, bf->clusters[i]->fbuf.handle);
    }
}
#endif

/* the written clusters: cluster size, count, then (number, data) */
static int bf_save_writes(BlockDevice *bs, uint8_t *buf, int buf_size)
{
    BlockDeviceHTTP *bf = bs->opaque;
    SnapWriter w1, *w = &w1;
    int i, n, cluster_size;
    uint8_t *p;

    /* the virtio device refuses to save while a request is in progress */
    cluster_size = bf->sectors_per_cluster * 512;
    n = 0;
    for(i = 0; i < bf->n_clusters; i++) {
        if (bf->clusters[i])
            n++;
    }
    snap_writer_init(w, buf, buf_size);
    snap_put_u32(w, cluster_size);
    snap_put_u32(w, n);
    for(i = 0; i < bf->n_clusters; i++) {
        if (bf->clusters[i]) {
            snap_put_u32(w, i);
            p = snap_reserve(w, cluster_size);
            if (p)
                file_buffer_read(&bf->clusters[i]->fbuf, 0, p, cluster_size);
        }
    }
    return snap_writer_end(w);
}

static int bf_load_writes(BlockDevice *bs, const uint8_t *buf, int len,
                          BOOL check_only)
{
    BlockDeviceHTTP *bf = bs->opaque;
    SnapReader r1, *r = &r1;
    int i, n, num, cluster_size;
    const uint8_t *data;
    Cluster *c;

    cluster_size = bf->sectors_per_cluster * 512;
    /* only on top of the pristine image */
    if (bf->n_allocated_clusters != 0)
        return -1;
    snap_reader_init(r, buf, len);
    if (snap_get_u32(r) != cluster_size)
        return -1;
    n = snap_get_u32(r);
    for(i = 0; i < n; i++) {
        num = snap_get_u32(r);
        data = snap_get_data(r, cluster_size);
        if (r->error || num < 0 || num >= bf->n_clusters)
            return -1;
        if (check_only)
            continue;
        c = bf->clusters[num];
        if (!c) {
            c = mallocz(sizeof(Cluster));
            file_buffer_init(&c->fbuf);
            file_buffer_resize(&c->fbuf, cluster_size);
            bf->clusters[num] = c;
            bf->n_allocated_clusters++;
        }
        file_buffer_write(&c->fbuf, 0, data, cluster_size);
    }
    if (!snap_reader_done(r))
        return -1;
    if (!check_only && n > 0)
        bf->persist_on_write = TRUE;
    return 0;
}

