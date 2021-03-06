// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.


#include "dnode.h"
#include "devmgr.h"
#include "memfs-private.h"

#include <fs/vfs.h>

#include <magenta/listnode.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace memfs {

// create a new dnode and attach it to a vnode
mx_status_t dn_create(dnode_t** out, const char* name, size_t len, VnodeMemfs* vn) {
    mx_status_t status;
    if ((status = dn_allocate(out, name, len)) < 0) {
        return status;
    }
    dn_attach(*out, vn);
    return NO_ERROR;
}

mx_status_t dn_allocate(dnode_t** out, const char* name, size_t len) {
    if ((len > DN_NAME_MAX) || (len < 1)) {
        return ERR_INVALID_ARGS;
    }

    dnode_t* dn;
    if ((dn = static_cast<dnode_t*>(calloc(1, sizeof(dnode_t) + len + 1))) == nullptr) {
        return ERR_NO_MEMORY;
    }
    dn->flags = static_cast<uint32_t>(len);
    memcpy(dn->name, name, len);
    dn->name[len] = '\0';
    list_initialize(&dn->children);
    *out = dn;
    return NO_ERROR;
}

// Attach a vnode to a dnode
void dn_attach(dnode_t* dn, VnodeMemfs* vn) {
    dn->vnode = vn;
    if (vn != nullptr) {
        vn->RefAcquire();
        list_add_tail(&vn->dn_list_, &dn->vn_entry);
    }
}

void dn_delete(dnode_t* dn) {
    MX_DEBUG_ASSERT(list_is_empty(&dn->children));

    // detach from parent
    if (dn->parent) {
        list_delete(&dn->dn_entry);
        if (DNODE_IS_DIR(dn)) {
            dn->parent->vnode->link_count_--;
        }
        dn->parent = nullptr;
    }

    // detach from vnode
    if (dn->vnode) {
        list_delete(&dn->vn_entry);
        dn->vnode->link_count_--;
        dn->vnode->dnode_ = nullptr;
        dn->vnode->RefRelease();
        dn->vnode = nullptr;
    }

    free(dn);
}

void dn_add_child(dnode_t* parent, dnode_t* child) {
    if ((parent == nullptr) || (child == nullptr)) {
        printf("dn_add_child(%p,%p) bad args\n", parent, child);
        panic();
    }
    if (child->parent) {
        printf("dn_add_child: child %p already has parent %p\n", child, parent);
        panic();
    }
    if (child->dn_entry.prev || child->dn_entry.next) {
        printf("dn_add_child: child %p has non-empty dn_entry\n", child);
        panic();
    }

    child->parent = parent;
    child->vnode->link_count_++;
    if (child->vnode->dnode_) {
        // Child has '..' pointing back at parent.
        parent->vnode->link_count_++;
    }
    list_add_tail(&parent->children, &child->dn_entry);
}

mx_status_t dn_lookup(dnode_t* parent, dnode_t** out, const char* name, size_t len) {
    dnode_t* dn;
    if ((len == 1) && (name[0] == '.')) {
        *out = parent;
        return NO_ERROR;
    }
    if ((len == 2) && (name[0] == '.') && (name[1] == '.')) {
        *out = parent->parent;
        return NO_ERROR;
    }
    list_for_every_entry(&parent->children, dn, dnode_t, dn_entry) {
        if (DN_NAME_LEN(dn->flags) != len) {
            continue;
        }
        if (memcmp(dn->name, name, len) != 0) {
            continue;
        }
        *out = dn;
        return NO_ERROR;
    }
    return ERR_NOT_FOUND;
}

// return the (first) name matching this vnode
mx_status_t dn_lookup_name(const dnode_t* parent, const VnodeMemfs* vn, char* out, size_t out_len) {
    dnode_t* dn;
    list_for_every_entry(&parent->children, dn, dnode_t, dn_entry) {
        if (dn->vnode == vn) {
            mx_off_t len = DN_NAME_LEN(dn->flags);
            if (len > out_len-1) {
                len = out_len-1;
            }
            memcpy(out, dn->name, len);
            out[len] = '\0';
            return NO_ERROR;
        }
    }
    return ERR_NOT_FOUND;
}

// debug printout of file system tree
void dn_print_children(dnode_t* parent, int indent) {
    dnode_t* dn;
    if (indent > 5) return; // error
    list_for_every_entry(&parent->children, dn, dnode_t, dn_entry) {
        printf("%*.s %.*s\n", indent*4, " ", DN_NAME_LEN(dn->flags), dn->name);
        dn_print_children(dn, indent+1);
    }
}

mx_status_t dn_readdir(dnode_t* parent, void* cookie, void* data, size_t len) {
    vdircookie_t* c = static_cast<vdircookie_t*>(cookie);
    dnode_t* last = static_cast<dnode_t*>(c->p);
    size_t pos = 0;
    char* ptr = static_cast<char*>(data);
    bool search = (last != nullptr);
    mx_status_t r;
    dnode_t* dn;

    // Use 'c->p' to point to the last seen vnode.
    // Use 'c->n' to count the number of entries we've already returned.
    if (c->n == 0) {
        r = fs::vfs_fill_dirent(reinterpret_cast<vdirent_t*>(ptr + pos), len - pos, ".", 1,
                                VTYPE_TO_DTYPE(V_TYPE_DIR));
        if (r < 0) {
            return static_cast<mx_status_t>(pos);
        }
        pos += r;
        c->n++;
    }
    if (c->n == 1) {
        r = fs::vfs_fill_dirent(reinterpret_cast<vdirent_t*>(ptr + pos), len - pos, "..", 2,
                                VTYPE_TO_DTYPE(V_TYPE_DIR));
        if (r < 0) {
            return static_cast<mx_status_t>(pos);
        }
        pos += r;
        c->n++;
    }
    if (parent == NULL) {
        // This is the case for directories which have been deleted.
        return static_cast<mx_status_t>(pos);
    }

    list_for_every_entry(&parent->children, dn, dnode_t, dn_entry) {
        if (search) {
            if (dn == last) {
                search = false;
            }
        } else {
            uint32_t vtype = DNODE_IS_DIR(dn) ? V_TYPE_DIR : V_TYPE_FILE;
            r = fs::vfs_fill_dirent(reinterpret_cast<vdirent_t*>(ptr + pos), len - pos,
                                    dn->name, DN_NAME_LEN(dn->flags),
                                    VTYPE_TO_DTYPE(vtype));
            if (r < 0) {
                break;
            }
            last = dn;
            pos += r;
            c->n++;
        }
    }

    c->p = last;
    return static_cast<mx_status_t>(pos);
}

} // namespace memfs
