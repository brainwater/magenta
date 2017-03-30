// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <ddk/io-buffer.h>
#include <ddk/iotxn.h>
#include <ddk/device.h>
#include <magenta/assert.h>
#include <magenta/syscalls.h>
#include <sys/param.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <threads.h>

#define TRACE 0

#if TRACE
#define xprintf(fmt...) printf(fmt)
#else
#define xprintf(fmt...) \
    do {                \
    } while (0)
#endif

#define IOTXN_FLAG_CLONE (1 << 0)
#define IOTXN_FLAG_FREE  (1 << 1)   // for double-free checking
#define IOTXN_FLAG_DEAD  (1 << 2)   // buffer is no longer valid

typedef struct iotxn_priv iotxn_priv_t;

struct iotxn_priv {
    // data payload.
    io_buffer_t buffer;

    uint32_t flags;

    // payload size
    size_t data_size;
    // extra data, at the end of this ioxtn_t structure
    size_t extra_size;

    // number of times we have been cloned
    int clone_count;
    // the iotxn we were cloned from
    iotxn_priv_t* orig_txn;

    iotxn_t txn; // must be at the end for extra data, only valid if not a clone
};

#define get_priv(iotxn) containerof(iotxn, iotxn_priv_t, txn)

// This assert will fail if we attempt to access the buffer of a cloned txn after it has been completed
#define ASSERT_BUFFER_VALID(priv) MX_DEBUG_ASSERT(!(priv->flags & IOTXN_FLAG_DEAD))

static list_node_t free_list = LIST_INITIAL_VALUE(free_list);
static list_node_t clone_list = LIST_INITIAL_VALUE(clone_list); // free list for clones
static mtx_t free_list_mutex = MTX_INIT;
static mtx_t clone_list_mutex = MTX_INIT;

static void iotxn_complete(iotxn_t* txn, mx_status_t status, mx_off_t actual) {
    iotxn_priv_t* priv = get_priv(txn);
    if (priv->orig_txn) {
        priv->orig_txn->clone_count--;
        MX_DEBUG_ASSERT(priv->orig_txn->clone_count >= 0);
        // completing a cloned txn can result in the original txn being released
        // so after this point it is unsafe to reference our buffer
        // clearing priv->buffer will trigger ASSERT_BUFFER_VALID() if someone attempts
        // to access the buffer after it has been cleared
        memset(&priv->buffer, 0, sizeof(priv->buffer));
        priv->flags |= IOTXN_FLAG_DEAD;
    }

    txn->actual = actual;
    txn->status = status;
    if (txn->complete_cb) {
        txn->complete_cb(txn, txn->cookie);
    }
}

static void iotxn_copyfrom(iotxn_t* txn, void* data, size_t length, size_t offset) {
    iotxn_priv_t* priv = get_priv(txn);
    size_t count = MIN(length, priv->data_size - offset);
    ASSERT_BUFFER_VALID(priv);
    memcpy(data, io_buffer_virt(&priv->buffer) + offset, count);
}

static void iotxn_copyto(iotxn_t* txn, const void* data, size_t length, size_t offset) {
    iotxn_priv_t* priv = get_priv(txn);
    size_t count = MIN(length, priv->data_size - offset);
    ASSERT_BUFFER_VALID(priv);
    memcpy(io_buffer_virt(&priv->buffer) + offset, data, count);
}

static void iotxn_physmap(iotxn_t* txn, mx_paddr_t* addr) {
    iotxn_priv_t* priv = get_priv(txn);
    ASSERT_BUFFER_VALID(priv);
    *addr = io_buffer_phys(&priv->buffer);
}

static void iotxn_mmap(iotxn_t* txn, void** data) {
    iotxn_priv_t* priv = get_priv(txn);
    ASSERT_BUFFER_VALID(priv);
    *data = io_buffer_virt(&priv->buffer);
}

static iotxn_priv_t* iotxn_get_clone(size_t extra_size) {
    iotxn_priv_t* cpriv = NULL;
    iotxn_t* clone = NULL;

    // look in clone list first for something that fits
    bool found = false;

    mtx_lock(&clone_list_mutex);
    list_for_every_entry (&clone_list, clone, iotxn_t, node) {
        cpriv = get_priv(clone);
        if (cpriv->extra_size >= extra_size) {
            found = true;
            break;
        }
    }
    // found one that fits, skip allocation
    if (found) {
        list_delete(&clone->node);
        cpriv->flags &= ~IOTXN_FLAG_FREE;
        if (cpriv->extra_size) memset(&cpriv[1], 0, cpriv->extra_size);
        mtx_unlock(&clone_list_mutex);
        goto out;
    }
    mtx_unlock(&clone_list_mutex);

    // didn't find one that fits, allocate a new one
    cpriv = calloc(1, sizeof(iotxn_priv_t) + extra_size);
    if (!cpriv) {
        xprintf("iotxn: out of memory\n");
        return NULL;
    }

out:
    cpriv->flags |= IOTXN_FLAG_CLONE;
    return cpriv;
}

static void iotxn_release(iotxn_t* txn) {
    xprintf("iotxn_release: txn=%p\n", txn);
    iotxn_priv_t* priv = get_priv(txn);
    if (priv->flags & IOTXN_FLAG_FREE) {
        printf("double free in iotxn_release\n");
        abort();
    }

    MX_DEBUG_ASSERT(priv->clone_count == 0);

    if (priv->flags & IOTXN_FLAG_CLONE) {
        mtx_lock(&clone_list_mutex);
        list_add_tail(&clone_list, &txn->node);
        priv->flags |= IOTXN_FLAG_FREE;
        mtx_unlock(&clone_list_mutex);
    } else {
        mtx_lock(&free_list_mutex);
        list_add_tail(&free_list, &txn->node);
        priv->flags |= IOTXN_FLAG_FREE;
        mtx_unlock(&free_list_mutex);
    }
    priv->flags &= ~IOTXN_FLAG_DEAD;
}

static mx_status_t iotxn_clone(iotxn_t* txn, iotxn_t** out, size_t extra_size) {
    iotxn_priv_t* priv = get_priv(txn);
    iotxn_priv_t* cpriv = iotxn_get_clone(extra_size);
    if (!cpriv) return ERR_NO_MEMORY;

    // Here we directly copy txn's io_buffer rather than cloning it, to reduce overhead
    // of duplicating and remapping the VMO. This is safe to do as long as we guarantee
    // that the clone will be completed before the source txn.
    memcpy(&cpriv->buffer, &priv->buffer, sizeof(cpriv->buffer));
    cpriv->data_size = priv->data_size;
    memcpy(&cpriv->txn, txn, sizeof(iotxn_t));
    cpriv->txn.complete_cb = NULL; // clear the complete cb

    cpriv->orig_txn = priv;
    priv->clone_count++;

    *out = &cpriv->txn;
    return NO_ERROR;
}

static void iotxn_cacheop(iotxn_t* txn, uint32_t op, size_t offset, size_t length) {
    iotxn_priv_t* priv = get_priv(txn);
    ASSERT_BUFFER_VALID(priv);
    io_buffer_cache_op(&priv->buffer, op, offset, length);
}

static iotxn_ops_t ops = {
    .complete = iotxn_complete,
    .copyfrom = iotxn_copyfrom,
    .copyto = iotxn_copyto,
    .physmap = iotxn_physmap,
    .mmap = iotxn_mmap,
    .clone = iotxn_clone,
    .release = iotxn_release,
    .cacheop = iotxn_cacheop,
};

mx_status_t iotxn_alloc(iotxn_t** out, uint32_t flags, size_t data_size, size_t extra_size) {
    xprintf("iotxn_alloc: flags=0x%x data_size=0x%zx extra_size=0x%zx\n", flags, data_size, extra_size);
    iotxn_t* txn = NULL;
    iotxn_priv_t* priv = NULL;
    // look in free list first for something that fits
    bool found = false;

    mtx_lock(&free_list_mutex);
    list_for_every_entry (&free_list, txn, iotxn_t, node) {
        priv = get_priv(txn);
        if (priv->buffer.size >= data_size && priv->extra_size >= extra_size) {
            found = true;
            break;
        }
    }
    // found one that fits, skip allocation
    if (found) {
        list_delete(&txn->node);
        memset(&txn, 0, sizeof(iotxn_t));
        memset(io_buffer_virt(&priv->buffer), 0, priv->buffer.size);
        priv->flags &= ~IOTXN_FLAG_FREE;
        mtx_unlock(&free_list_mutex);
        goto out;
    }
    mtx_unlock(&free_list_mutex);

    // didn't find one that fits, allocate a new one
    priv = calloc(1, sizeof(iotxn_priv_t) + extra_size);
    if (!priv) return ERR_NO_MEMORY;
    if (data_size > 0) {
        mx_status_t status = io_buffer_init(&priv->buffer, data_size, IO_BUFFER_RW);
        if (status != NO_ERROR) {
            free(priv);
            return status;
        }
    }

    // layout is iotxn_priv_t | extra_size
    priv->extra_size = extra_size;
out:
    priv->data_size = data_size;
    priv->txn.ops = &ops;
    *out = &priv->txn;
    xprintf("iotxn_alloc: found=%d txn=%p buffer_size=0x%zx\n", found, &priv->txn, priv->buffer.size);
    return NO_ERROR;
}

mx_status_t iotxn_alloc_vmo(iotxn_t** out, mx_handle_t vmo_handle, size_t data_size,
                            mx_off_t data_offset, size_t extra_size) {
    iotxn_priv_t* priv = iotxn_get_clone(extra_size);
    if (!priv) return ERR_NO_MEMORY;

    io_buffer_init_vmo(&priv->buffer, vmo_handle, data_offset, IO_BUFFER_RW);
    priv->data_size = data_size;

    priv->txn.ops = &ops;
    *out = &priv->txn;
    return NO_ERROR;
}

void iotxn_queue(mx_device_t* dev, iotxn_t* txn) {
    dev->ops->iotxn_queue(dev, txn);
}

// temporary hack
void iotxn_get_vmo(iotxn_t* txn, mx_handle_t* out_handle, mx_off_t* out_offset) {
    iotxn_priv_t* priv = get_priv(txn);
    *out_handle = priv->buffer.vmo_handle;
    *out_offset = priv->buffer.offset;
}
