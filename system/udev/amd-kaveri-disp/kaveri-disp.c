// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/display.h>
#include <ddk/protocol/pci.h>
#include <hw/pci.h>

#include <assert.h>
#include <inttypes.h>
#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// simple framebuffer device to match against an AMD Kaveri R7 device already
// initialized from EFI
#define AMD_GFX_VID (0x1002)
#define AMD_KAVERI_R7_DID (0x130f)

typedef struct kaveri_disp_device {
    mx_device_t device;

    void* regs;
    uint64_t regs_size;
    mx_handle_t regs_handle;

    void* framebuffer;
    uint64_t framebuffer_size;
    mx_handle_t framebuffer_handle;

    mx_display_info_t info;
} kaveri_disp_device_t;

#define get_kaveri_disp_device(dev) containerof(dev, kaveri_disp_device_t, device)

// implement display protocol
static mx_status_t kaveri_disp_set_mode(mx_device_t* dev, mx_display_info_t* info) {
    return ERR_NOT_SUPPORTED;
}

static mx_status_t kaveri_disp_get_mode(mx_device_t* dev, mx_display_info_t* info) {
    assert(info);
    kaveri_disp_device_t* device = get_kaveri_disp_device(dev);

    memcpy(info, &device->info, sizeof(mx_display_info_t));
    return NO_ERROR;
}

static mx_status_t kaveri_disp_get_framebuffer(mx_device_t* dev, void** framebuffer) {
    assert(framebuffer);
    kaveri_disp_device_t* device = get_kaveri_disp_device(dev);

    (*framebuffer) = device->framebuffer;
    return NO_ERROR;
}

static mx_display_protocol_t kaveri_disp_display_proto = {
    .set_mode = kaveri_disp_set_mode,
    .get_mode = kaveri_disp_get_mode,
    .get_framebuffer = kaveri_disp_get_framebuffer,
};

// implement device protocol

static mx_status_t kaveri_disp_release(mx_device_t* dev) {
    kaveri_disp_device_t* device = get_kaveri_disp_device(dev);

    if (device->regs) {
        mx_handle_close(device->regs_handle);
        device->regs_handle = -1;
    }

    if (device->framebuffer) {
        mx_handle_close(device->framebuffer_handle);
        device->framebuffer_handle = -1;
    }

    return NO_ERROR;
}

static mx_protocol_device_t kaveri_disp_device_proto = {
    .release = kaveri_disp_release,
};

// implement driver object:

static mx_status_t kaveri_disp_bind(mx_driver_t* drv, mx_device_t* dev, void** cookie) {
    pci_protocol_t* pci;
    mx_pci_resource_t pci_res;
    mx_status_t status;

    if (device_get_protocol(dev, MX_PROTOCOL_PCI, (void**)&pci))
        return ERR_NOT_SUPPORTED;

    status = pci->claim_device(dev);
    if (status < 0)
        return status;

    // map resources and initialize the device
    kaveri_disp_device_t* device = calloc(1, sizeof(kaveri_disp_device_t));
    if (!device)
        return ERR_NO_MEMORY;

    // get the register window bar
    status = pci->get_bar(dev, 5, &pci_res);
    if (status != NO_ERROR) {
        printf("kaveri-disp: error %d getting bar 5\n", status);
        goto fail;
    }

    status = mx_vmo_set_cache_policy(pci_res.mmio_handle, MX_CACHE_POLICY_UNCACHED_DEVICE);
    if (status != NO_ERROR) {
        printf("kaveri-disp: error %d setting bar 5 cache policy\n", status);
        goto fail;
    }

    status = mx_vmar_map(mx_vmar_root_self(), 0, pci_res.mmio_handle, 0, pci_res.size,
            MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE | MX_VM_FLAG_MAP_RANGE,
            (uintptr_t*)&device->regs);
    if (status != NO_ERROR) {
        printf("kaveri-disp: error %d mapping bar 5\n", status);
        return status;
    }
    device->regs_size = pci_res.size;
    device->regs_handle = pci_res.mmio_handle;

    // get the framebuffer window bar
    status = pci->get_bar(dev, 0, &pci_res);
    if (status != NO_ERROR) {
        printf("kaveri-disp: error %d getting bar 0\n", status);
        goto fail;
    }

    status = mx_vmo_set_cache_policy(pci_res.mmio_handle, MX_CACHE_POLICY_WRITE_COMBINING);
    if (status != NO_ERROR) {
        printf("kaveri-disp: error %d setting bar 0 cache policy\n", status);
        goto fail;
    }

    status = mx_vmar_map(mx_vmar_root_self(), 0, pci_res.mmio_handle, 0, pci_res.size,
            MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE | MX_VM_FLAG_MAP_RANGE,
            (uintptr_t*)&device->framebuffer);
    if (status != NO_ERROR) {
        printf("kaveri-disp: error %d mapping bar 0\n", status);
        return status;
    }
    device->framebuffer_size = pci_res.size;
    device->framebuffer_handle = pci_res.mmio_handle;

    // create and add the display (char) device
    device_init(&device->device, drv, "amd_kaveri_disp", &kaveri_disp_device_proto);

    mx_display_info_t* di = &device->info;
    uint32_t format, width, height, stride;
    status = mx_bootloader_fb_get_info(&format, &width, &height, &stride);
    if (status == NO_ERROR) {
        di->format = format;
        di->width = width;
        di->height = height;
        di->stride = stride;
    } else {
        status = ERR_NOT_SUPPORTED;
        goto fail;
    }
    di->flags = MX_DISPLAY_FLAG_HW_FRAMEBUFFER;

    mx_set_framebuffer(get_root_resource(), device->framebuffer, device->framebuffer_size,
                       format, width, height, stride);

    device->device.protocol_id = MX_PROTOCOL_DISPLAY;
    device->device.protocol_ops = &kaveri_disp_display_proto;
    device_add(&device->device, dev);

    printf("initialized amd kaveri R7 display driver, reg=%p regsize=0x%" PRIx64 " fb=%p fbsize=0x%" PRIx64 "\n",
           device->regs, device->regs_size, device->framebuffer, device->framebuffer_size);
    printf("\twidth %u height %u stride %u format %u\n",
           device->info.width, device->info.height, device->info.stride, device->info.format);

    return NO_ERROR;
fail:
    free(device);
    return status;
}

mx_driver_t _driver_kaveri_disp = {
    .ops = {
        .bind = kaveri_disp_bind,
    },
};

// clang-format off
MAGENTA_DRIVER_BEGIN(_driver_kaveri_disp, "amd-kaveri-display", "magenta", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_PCI),
    BI_ABORT_IF(NE, BIND_PCI_VID, AMD_GFX_VID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, AMD_KAVERI_R7_DID),
MAGENTA_DRIVER_END(_driver_kaveri_disp)
