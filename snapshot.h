/*
 * Helpers to save and restore machine state (snapshots)
 *
 * A writer with a NULL buffer only counts, so the same code gives the size
 * of a state and writes it. Values are little-endian. A reader flags an
 * error instead of reading past the end.
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
#ifndef SNAPSHOT_H
#define SNAPSHOT_H

#include <string.h>
#include "cutils.h"

typedef struct {
    uint8_t *buf; /* NULL: only count */
    int size;
    int pos;
    BOOL overflow;
} SnapWriter;

static inline void snap_writer_init(SnapWriter *w, uint8_t *buf, int size)
{
    w->buf = buf;
    w->size = size;
    w->pos = 0;
    w->overflow = FALSE;
}

/* room for len more bytes at w->buf + w->pos (NULL when only counting) */
static inline uint8_t *snap_reserve(SnapWriter *w, int len)
{
    uint8_t *p = NULL;
    if (w->buf) {
        if (w->pos + len <= w->size)
            p = w->buf + w->pos;
        else
            w->overflow = TRUE;
    }
    w->pos += len;
    return p;
}

static inline void snap_put_u32(SnapWriter *w, uint32_t v)
{
    uint8_t *p = snap_reserve(w, 4);
    if (p)
        put_le32(p, v);
}

static inline void snap_put_u64(SnapWriter *w, uint64_t v)
{
    uint8_t *p = snap_reserve(w, 8);
    if (p)
        put_le64(p, v);
}

static inline void snap_put_data(SnapWriter *w, const void *data, int len)
{
    uint8_t *p = snap_reserve(w, len);
    if (p)
        memcpy(p, data, len);
}

/* the size written (or needed), -1 if the buffer was too small */
static inline int snap_writer_end(SnapWriter *w)
{
    return w->overflow ? -1 : w->pos;
}

typedef struct {
    const uint8_t *buf;
    int len;
    int pos;
    BOOL error;
} SnapReader;

static inline void snap_reader_init(SnapReader *r, const uint8_t *buf, int len)
{
    r->buf = buf;
    r->len = len;
    r->pos = 0;
    r->error = FALSE;
}

static inline const uint8_t *snap_get_data(SnapReader *r, int len)
{
    const uint8_t *p;
    if (r->error || len < 0 || r->pos + len > r->len) {
        r->error = TRUE;
        return NULL;
    }
    p = r->buf + r->pos;
    r->pos += len;
    return p;
}

static inline uint32_t snap_get_u32(SnapReader *r)
{
    const uint8_t *p = snap_get_data(r, 4);
    return p ? get_le32(p) : 0;
}

static inline uint64_t snap_get_u64(SnapReader *r)
{
    const uint8_t *p = snap_get_data(r, 8);
    return p ? get_le64(p) : 0;
}

/* TRUE if everything was read without error */
static inline BOOL snap_reader_done(SnapReader *r)
{
    return !r->error && r->pos == r->len;
}

#endif /* SNAPSHOT_H */
