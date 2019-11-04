# Copyright 2018 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
# libchrome is not used in Newblue. Disable this to avoid warnings from
# common.mk.
BASE_VER=0
include common.mk

OUT ?= $(CURDIR)/build-opt-local

libdir ?= /usr/lib
includedir ?= /usr/include

INSTALL ?= install

CPPFLAGS += -I. -DEXECUTABLE -D_GNU_SOURCE
CFLAGS += -Wno-unused-function
LDLIBS += -lpthread

.PHONY: install install_files test

common_OBJS = att.o \
              gatt.o \
              gatt-builtin.o \
              bt.o \
              btleHid.o \
              gattSvcBattery.o \
              gattSvcMiscInfo.o \
              hci.o \
              l2cap.o \
              log.o \
              mt.o \
              multiNotif.o \
              persist.o \
              sendQ.o \
              sg.o \
              sm.o \
              timer.o \
              uhid.o \
              uniq.o \
              uuid.o \
              vendorLib.o \
              workQueue.o

# Newblue Library - libnewblue.so
libnewblue_LIBS = -ldl -lrt
libnewblue_OBJS = $(common_OBJS) \
                  config.o
libnewblue_DEPS = CC_LIBRARY(hci_plugin/libbt-vendor.so)
# Headers exposed
libnewblue_HEADERS = att.h \
                     bt.h \
                     gatt.h \
                     gatt-builtin.h \
                     gattSvcBattery.h \
                     gattSvcMiscInfo.h \
                     btleHid.h \
                     hci.h \
                     l2cap.h \
                     multiNotif.h \
                     newblue-macros.h \
                     sg.h \
                     sm.h \
                     timer.h \
                     types.h \
                     uhid.h \
                     uniq.h \
                     uuid.h

CC_LIBRARY(libnewblue.so): $(libnewblue_OBJS) $(libnewblue_DEPS)
CC_LIBRARY(libnewblue.so): LDLIBS += $(libnewblue_LIBS)
clean: CLEAN(libnewblue.so)
all: CC_LIBRARY(libnewblue.so)

# Newblue Library for Unit Testing - libnewblue_for_unittest.pie.a
libnewblue_for_unittest_OBJS = $(common_OBJS) \
                               config_for_unittest.o
CC_STATIC_LIBRARY(libnewblue_for_unittest.pie.a): $(libnewblue_for_unittest_OBJS)
clean: CLEAN(libnewblue_for_unittest.pie.a)
test: CC_STATIC_LIBRARY(libnewblue_for_unittest.pie.a)

# Newblue Library Manual Test Program - new_blue_test
libnewblue_test_DEPS = CC_LIBRARY(libnewblue.so) $(libnewblue_DEPS)
CC_BINARY(new_blue_test): test.o log.o $(libnewblue_test_DEPS)
CC_BINARY(new_blue_test): LDLIBS += $(libnewblue_LIBS)
clean: CLEAN(new_blue_test)
all: CC_BINARY(new_blue_test)

install_files: all
	$(INSTALL) -D $(OUT)/libnewblue.so $(DESTDIR)$(libdir)/libnewblue.so
	$(INSTALL) -t $(DESTDIR)$(includedir)/newblue -D $(libnewblue_HEADERS)
	$(INSTALL) -D $(OUT)/hci_plugin/libbt-vendor.so $(DESTDIR)$(libdir)/libbt-vendor.so
install: install_files
