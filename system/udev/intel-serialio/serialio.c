// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/protocol/pci.h>
#include <hw/pci.h>

#include <magenta/listnode.h>

#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>

#include <stdio.h>

#include <intel-serialio/serialio.h>

static mx_status_t intel_serialio_bind(mx_driver_t* drv, mx_device_t* dev, void** cookie) {
    pci_protocol_t* pci;
    mx_pci_resource_t pci_res;
    const pci_config_t* pci_config;
    mx_handle_t config_handle = MX_HANDLE_INVALID;
    mx_status_t res;

    if (!drv || !dev) {
        return ERR_INVALID_ARGS;
    }

    if (device_get_protocol(dev, MX_PROTOCOL_PCI, (void**)&pci)) {
        return ERR_NOT_SUPPORTED;
    }

    // Get the device config
    res = pci->get_config_vmo(dev, &pci_res);
    if (res != NO_ERROR) {
        printf("serialio: error %d getting pci config\n", res);
        return res;
    }

    res = mx_vmar_map(mx_vmar_root_self(), 0, pci_res.mmio_handle, 0, pci_res.size,
            MX_VM_FLAG_PERM_READ, (uintptr_t*)&pci_config);
    if (res != NO_ERROR) {
        printf("serialio: error %d mapping pci config\n", res);
        return res;
    }
    config_handle = pci_res.mmio_handle;

    switch (pci_config->device_id) {
    case INTEL_WILDCAT_POINT_SERIALIO_DMA_DID:
        res = intel_serialio_bind_dma(drv, dev);
        break;
    case INTEL_WILDCAT_POINT_SERIALIO_I2C0_DID:
    case INTEL_WILDCAT_POINT_SERIALIO_I2C1_DID:
    case INTEL_SUNRISE_POINT_SERIALIO_I2C0_DID:
    case INTEL_SUNRISE_POINT_SERIALIO_I2C1_DID:
    case INTEL_SUNRISE_POINT_SERIALIO_I2C2_DID:
    case INTEL_SUNRISE_POINT_SERIALIO_I2C3_DID:
        res = intel_serialio_bind_i2c(drv, dev);
        break;
    case INTEL_WILDCAT_POINT_SERIALIO_SDIO_DID:
        res = intel_serialio_bind_sdio(drv, dev);
        break;
    case INTEL_WILDCAT_POINT_SERIALIO_SPI0_DID:
        res = intel_serialio_bind_spi(drv, dev);
        break;
    case INTEL_WILDCAT_POINT_SERIALIO_SPI1_DID:
        res = intel_serialio_bind_spi(drv, dev);
        break;
    case INTEL_WILDCAT_POINT_SERIALIO_UART0_DID:
        res = intel_serialio_bind_uart(drv, dev);
        break;
    case INTEL_WILDCAT_POINT_SERIALIO_UART1_DID:
        res = intel_serialio_bind_uart(drv, dev);
        break;
    default:
        res = ERR_NOT_SUPPORTED;
        break;
    }

    mx_handle_close(config_handle);
    return res;
}

mx_driver_t _intel_serialio = {
    .ops = {
        .bind = intel_serialio_bind,
    },
};

// clang-format off
MAGENTA_DRIVER_BEGIN(_intel_serialio, "intel-serialio", "magenta", "0.1", 14)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_PCI),
    BI_ABORT_IF(NE, BIND_PCI_VID, INTEL_VID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_DMA_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_I2C0_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_I2C1_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_SDIO_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_SPI0_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_SPI1_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_UART0_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_WILDCAT_POINT_SERIALIO_UART1_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_SUNRISE_POINT_SERIALIO_I2C0_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_SUNRISE_POINT_SERIALIO_I2C1_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_SUNRISE_POINT_SERIALIO_I2C2_DID),
    BI_MATCH_IF(EQ, BIND_PCI_DID, INTEL_SUNRISE_POINT_SERIALIO_I2C3_DID),
MAGENTA_DRIVER_END(_intel_serialio)
