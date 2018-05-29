# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
include common.mk

CC_LIBRARY(hci_plugin/libbt-vendor.so): hci_plugin/vendor_lib.o
CC_LIBRARY(hci_plugin/libbt-vendor.so): CPPFLAGS += -I.. -DFILEPATH="\"/dev/hci_le\""
clean: CLEAN(hci_plugin/libbt-vendor.so)
all: CC_LIBRARY(hci_plugin/libbt-vendor.so)
