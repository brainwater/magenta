# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE_TYPE := driver

MODULE_STATIC_LIBS := ulib/ddk ulib/hexdump ulib/sync

MODULE_LIBS := ulib/driver ulib/magenta ulib/c

MODULE := $(LOCAL_DIR)-ax88772b

MODULE_SRCS := $(LOCAL_DIR)/asix-88772b.c

include make/module.mk


MODULE_TYPE := driver

MODULE_STATIC_LIBS := ulib/ddk ulib/hexdump ulib/sync

MODULE_LIBS := ulib/driver ulib/magenta ulib/c

MODULE := $(LOCAL_DIR)-ax88179

MODULE_SRCS := $(LOCAL_DIR)/asix-88179.c

include make/module.mk

MODULE_TYPE := driver

MODULE_STATIC_LIBS := ulib/ddk ulib/hexdump ulib/sync

MODULE_LIBS := ulib/driver ulib/magenta ulib/c ulib/mxio

MODULE := $(LOCAL_DIR)-lan9514

MODULE_SRCS := $(LOCAL_DIR)/smsc-lan9514.c

include make/module.mk
