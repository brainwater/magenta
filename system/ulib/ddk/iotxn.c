// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <ddk/iotxn.h>
#include <ddk/device.h>
#include <magenta/assert.h>
#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <sys/param.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <threads.h>

#define TRACE 1

#if TRACE
#define xprintf(fmt...) printf(fmt)
#else
#define xprintf(fmt...) \
    do {                \
    } while (0)
#endif

#define IOTXN_PFLAG_CONTIGUOUS (1 << 0)   // the vmo is contiguous
#define IOTXN_PFLAG_ALLOC      (1 << 1)   // the vmo is allocated by us
#define IOTXN_PFLAG_PHYSMAP    (1 << 2)   // we performed physmap() on this vmo
#define IOTXN_PFLAG_FREE       (1 << 3)   // this txn has been released

static list_node_t free_list = LIST_INITIAL_VALUE(free_list);
static mtx_t free_list_mutex = MTX_INIT;

// This assert will fail if we attempt to access the buffer of a cloned txn after it has been completed
#define ASSERT_BUFFER_VALID(priv) MX_DEBUG_ASSERT(!(priv->flags & IOTXN_FLAG_DEAD))

static uint32_t alloc_flags_to_pflags(uint32_t alloc_flags) {
    if (alloc_flags & IOTXN_ALLOC_CONTIGUOUS) {
        return IOTXN_PFLAG_CONTIGUOUS;
    }
    return 0;
}

static iotxn_t* find_in_free_list(uint32_t pflags, uint64_t data_size) {
    bool found = false;
    iotxn_t* txn = NULL;
    //xprintf("find_in_free_list pflags 0x%x data_size 0x%" PRIx64 "\n", pflags, data_size);
    mtx_lock(&free_list_mutex);
    list_for_every_entry (&free_list, txn, iotxn_t, node) {
        //xprintf("find_in_free_list for_every txn %p\n", txn);
        if ((txn->pflags & pflags) && (txn->vmo_length == data_size)) {
            found = true;
            break;
        }
    }
    if (found) {
        txn->pflags &= ~IOTXN_PFLAG_FREE;
        list_delete(&txn->node);
    }
    mtx_unlock(&free_list_mutex);
    //xprintf("find_in_free_list found %d txn %p\n", found, txn);
    return found ? txn : NULL;
}

void iotxn_pages_to_sg(mx_paddr_t* pages, iotxn_sg_t* sg, uint32_t len, uint32_t* sg_len) {
    if (len == 1) {
        sg->paddr = *pages;
        sg->length = PAGE_SIZE;
        *sg_len = 1;
        return;
    }

    mx_paddr_t last = pages[0];
    uint32_t sgl = 0;
    uint32_t sgi = 0;
    uint32_t pi = 0;
    for (pi = 0; pi < len - 1; pi++) {
        if (pages[pi] + PAGE_SIZE == pages[pi + 1]) {
            sgl += PAGE_SIZE;
        } else {
            sg[sgi].paddr = last;
            sg[sgi].length = sgl;
            sgi += 1;
            last = pages[pi + 1];
            sgl = 0;
        }
    }

    // fill in last entry
    sg[sgi].paddr = last;
    sg[sgi].length = sgl + PAGE_SIZE;

    *sg_len = sgi + 1;
}

// return the iotxn into the free list
static void iotxn_release_free_list(iotxn_t* txn) {
    mx_handle_t vmo_handle = txn->vmo_handle;
    uint64_t vmo_offset = txn->vmo_offset;
    uint64_t vmo_length = txn->vmo_length;
    iotxn_sg_t* sg = txn->sg;
    uint32_t sg_length = txn->sg_length;
    uint32_t pflags = txn->pflags;

    memset(txn, 0, sizeof(iotxn_t));

    if (pflags & IOTXN_PFLAG_ALLOC) {
        // if we allocated the vmo, keep it around
        txn->vmo_handle = vmo_handle;
        txn->vmo_offset = vmo_offset;
        txn->vmo_length = vmo_length;
        txn->sg = sg;
        txn->sg_length = sg_length;
        txn->pflags = pflags;
    } else if (pflags & IOTXN_PFLAG_PHYSMAP) {
        // only free the scatter list if we called physmap()
        if (sg != NULL) {
            free(sg);
        }
    }

    txn->pflags |= IOTXN_PFLAG_FREE;

    mtx_lock(&free_list_mutex);
    list_add_head(&free_list, &txn->node);
    mtx_unlock(&free_list_mutex);

    xprintf("iotxn_release_free_list released txn %p\n", txn);
}

static void iotxn_complete(iotxn_t* txn, mx_status_t status, mx_off_t actual) {
    xprintf("iotxn_complete txn %p\n", txn);
    txn->actual = actual;
    txn->status = status;
    if (txn->complete_cb) {
        txn->complete_cb(txn, txn->cookie);
    }
}

static ssize_t iotxn_copyfrom(iotxn_t* txn, void* data, size_t length, size_t offset) {
    length = MIN(txn->vmo_length - offset, length);
    size_t actual;
    mx_status_t status = mx_vmo_read(txn->vmo_handle, data, txn->vmo_offset + offset, length, &actual);
    xprintf("iotxn_copyfrom: txn %p vmo_offset 0x%" PRIx64 " offset 0x%zx length 0x%zx actual 0x%zx status %d\n", txn, txn->vmo_offset, offset, length, actual, status);
    return (status == NO_ERROR) ? (ssize_t)actual : status;
}

static ssize_t iotxn_copyto(iotxn_t* txn, const void* data, size_t length, size_t offset) {
    length = MIN(txn->vmo_length - offset, length);
    size_t actual;
    mx_status_t status = mx_vmo_write(txn->vmo_handle, data, txn->offset + offset, length, &actual);
    xprintf("iotxn_copyto: txn %p vmo_offset 0x%" PRIx64 " offset 0x%zx length 0x%zx actual 0x%zx status %d\n", txn, txn->vmo_offset, offset, length, actual, status);
    return (status == NO_ERROR) ? (ssize_t)actual : status;
}

#define ROUNDUP(a, b)   (((a) + ((b)-1)) & ~((b)-1))
#define ROUNDDOWN(a, b) ((a) & ~((b)-1))

static mx_status_t iotxn_physmap_contiguous(iotxn_t* txn) {
    iotxn_sg_t* sg = malloc(sizeof(iotxn_sg_t));
    if (sg == NULL) {
        xprintf("iotxn_physmap_contiguous: out of memory\n");
        return ERR_NO_MEMORY;
    }

    // commit pages and lookup physical addresses
    mx_status_t status = mx_vmo_op_range(txn->vmo_handle, MX_VMO_OP_COMMIT, txn->vmo_offset, txn->vmo_length, NULL, 0);
    if (status != NO_ERROR) {
        xprintf("iotxn_physmap_contiguous: error %d in commit\n", status);
        free(sg);
        return status;
    }

    // contiguous vmo so just lookup the first page
    uint64_t page_offset = ROUNDDOWN(txn->vmo_offset, PAGE_SIZE);
    status = mx_vmo_op_range(txn->vmo_handle, MX_VMO_OP_LOOKUP, page_offset, PAGE_SIZE, &sg->paddr, sizeof(mx_paddr_t));
    if (status) {
        xprintf("iotxn_physmap_contiguous: error %d in lookup\n", status);
        free(sg);
        return status;
    }

    sg->length = txn->vmo_length;
    sg->paddr += (txn->vmo_offset - page_offset);

    txn->sg = sg;
    txn->sg_length = 1;
    return NO_ERROR;
}

static mx_status_t iotxn_physmap_paged(iotxn_t* txn) {
    // MX_VMO_OP_LOOKUP returns whole pages, so take into account unaligned vmo
    // offset and lengths
    uint32_t offset_unaligned = txn->vmo_offset - ROUNDDOWN(txn->vmo_offset, PAGE_SIZE);
    uint32_t length_unaligned = ROUNDUP(txn->vmo_offset + txn->vmo_length, PAGE_SIZE) - (txn->vmo_offset + txn->vmo_length);
    uint32_t pages = txn->vmo_length / PAGE_SIZE;
    if (offset_unaligned) {
        pages += 1;
    }
    if (length_unaligned) {
        pages += 1;
    }
    iotxn_sg_t* sg = malloc(sizeof(iotxn_sg_t) * pages + sizeof(mx_paddr_t) * pages);
    if (sg == NULL) {
        xprintf("iotxn_physmap_paged: out of memory\n");
        return ERR_NO_MEMORY;
    }

    // commit pages and lookup physical addresses
    // assume that commited pages will never be auto-decommitted...
    mx_status_t status = mx_vmo_op_range(txn->vmo_handle, MX_VMO_OP_COMMIT, txn->vmo_offset, txn->vmo_length, NULL, 0);
    if (status != NO_ERROR) {
        xprintf("iotxn_physmap_paged: error %d in commit\n", status);
        free(sg);
        return status;
    }

    mx_paddr_t* paddrs = (mx_paddr_t*)&sg[pages];
    status = mx_vmo_op_range(txn->vmo_handle, MX_VMO_OP_LOOKUP, txn->vmo_offset, txn->vmo_length, paddrs, sizeof(mx_paddr_t) * pages);
    if (status != NO_ERROR) {
        xprintf("iotxn_physmap_paged: error %d in lookup\n", status);
        free(sg);
        return status;
    }

    // coalesce contiguous ranges
    uint32_t sg_len;
    iotxn_pages_to_sg(paddrs, sg, pages, &sg_len);

    // adjust for unaligned offset and length
    sg[0].paddr += offset_unaligned;
    sg[0].length = PAGE_SIZE - offset_unaligned;
    sg[sg_len - 1].length -= length_unaligned;

    txn->sg = sg;
    txn->sg_length = sg_len;
    return NO_ERROR;
}

static mx_status_t iotxn_physmap(iotxn_t* txn, mx_paddr_t* addr) {
    if (!(txn->pflags & IOTXN_PFLAG_CONTIGUOUS)) {
        return ERR_INVALID_ARGS;
    }
    mx_status_t status = iotxn_physmap_contiguous(txn);
    if (status == NO_ERROR) {
        *addr = txn->sg->paddr;
    }
    return status;
}

static mx_status_t iotxn_physmap_sg(iotxn_t* txn, iotxn_sg_t** sg_out, uint32_t *sg_len) {
    if (txn->sg != NULL) {
        *sg_out = txn->sg;
        *sg_len = txn->sg_length;
        return NO_ERROR;
    }

    if (txn->vmo_length == 0) {
        return ERR_INVALID_ARGS;
    }

    mx_status_t status;
    if (txn->pflags & IOTXN_PFLAG_CONTIGUOUS) {
        status = iotxn_physmap_contiguous(txn);
    } else {
        status = iotxn_physmap_paged(txn);
    }

    if (status == NO_ERROR) {
        *sg_out = txn->sg;
        *sg_len = txn->sg_length;
        txn->pflags |= IOTXN_PFLAG_PHYSMAP;
    }
    return status;
}

static mx_status_t iotxn_mmap(iotxn_t* txn, void** data) {
    xprintf("iotxn_mmap: txn %p\n", txn);
    return mx_vmar_map(mx_vmar_root_self(), 0, txn->vmo_handle, txn->vmo_offset, txn->vmo_length, MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE, (uintptr_t*)(data));
}

mx_status_t iotxn_clone(iotxn_t* txn, iotxn_t** out) {
    xprintf("iotxn_clone txn %p\n", txn);
    // look in clone list first
    // TODO if out is set, init into out
    iotxn_t* clone = find_in_free_list(0, 0);
    if (clone == NULL) {
        clone = calloc(1, sizeof(iotxn_t));
        if (clone == NULL) {
            return ERR_NO_MEMORY;
        }
    }

    memcpy(clone, txn, sizeof(iotxn_t));
    // the only relevant pflag for a clone is the contiguous bit
    clone->pflags = txn->pflags & IOTXN_PFLAG_CONTIGUOUS;
    clone->complete_cb = NULL;
    clone->release_cb = iotxn_release_free_list;

    *out = clone;
    return NO_ERROR;
}

static void iotxn_release(iotxn_t* txn) {
    if (txn->release_cb) {
        txn->release_cb(txn);
    }
}

static void iotxn_cacheop(iotxn_t* txn, uint32_t op, size_t offset, size_t length) {
    mx_vmo_op_range(txn->vmo_handle, op, txn->vmo_offset + offset, length, NULL, 0);
}

static iotxn_ops_t ops = {
    .complete = iotxn_complete,
    .copyfrom = iotxn_copyfrom,
    .copyto = iotxn_copyto,
    .physmap = iotxn_physmap,
    .physmap_sg = iotxn_physmap_sg,
    .mmap = iotxn_mmap,
    .clone = iotxn_clone,
    .release = iotxn_release,
    .cacheop = iotxn_cacheop,
};

mx_status_t iotxn_alloc(iotxn_t** out, uint32_t alloc_flags, uint64_t data_size) {
    //xprintf("iotxn_alloc: alloc_flags 0x%x data_size 0x%" PRIx64 "\n", alloc_flags, data_size);

    // look in free list first for a iotxn with data_size
    iotxn_t* txn = find_in_free_list(alloc_flags_to_pflags(alloc_flags), data_size);
    if (txn != NULL) {
        //xprintf("iotxn_alloc: found iotxn with size 0x%" PRIx64 " in free list\n", data_size);
        goto out;
    }

    // didn't find one that fits, allocate a new one
    txn = calloc(1, sizeof(iotxn_t));
    if (!txn) {
        return ERR_NO_MEMORY;
    }
    if (data_size > 0) {
        mx_status_t status;
        if (alloc_flags & IOTXN_ALLOC_CONTIGUOUS) {
            status = mx_vmo_create_contiguous(get_root_resource(), data_size, 0, &txn->vmo_handle);
            txn->pflags |= IOTXN_PFLAG_CONTIGUOUS;
        } else {
            status = mx_vmo_create(data_size, 0, &txn->vmo_handle);
        }
        if (status != NO_ERROR) {
            xprintf("iotxn_alloc: error %d in mx_vmo_create, flags 0x%x\n", status, alloc_flags);
            free(txn);
            return status;
        }
        txn->vmo_offset = 0;
        txn->vmo_length = data_size;
        txn->pflags |= IOTXN_PFLAG_ALLOC;
        txn->release_cb = iotxn_release_free_list;
    }

out:
    MX_DEBUG_ASSERT(txn != NULL);
    MX_DEBUG_ASSERT(!(txn->pflags & IOTXN_PFLAG_FREE));
    txn->ops = &ops;
    *out = txn;
    return NO_ERROR;
}

void iotxn_queue(mx_device_t* dev, iotxn_t* txn) {
    dev->ops->iotxn_queue(dev, txn);
}
