# Copyright 2017 The Chromium OS Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Hardcode these for minnie.
# TODO: make this configurable for all boards
prefix = /usr
exec_prefix = $(prefix)
bindir = $(exec_prefix)/bin
libdir = $(exec_prefix)/lib
includedir = $(prefix)/include

INSTALL = /usr/bin/install
CFLAGS = -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -g  -I. -DEXECUTABLE -fPIC
CPPFLAGS = -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -g  -I. -DEXECUTABLE -D_UNITTEST_ -DPREF_PATH=\"/tmp/bt.newblue.prefs\"

TEST := new_blue_test

# for pthread_setname_np
CFLAGS += -D_GNU_SOURCE

OBJ =                       \
    att.o                   \
    gatt.o                  \
    hci.o                   \
    l2cap.o                 \
    mt.o                    \
    multiNotif.o            \
    persist.o               \
    sendQ.o                 \
    sg.o                    \
    sm.o                    \
    timer.o                 \
    uniq.o                  \
    vendorLib.o             \
    uuid.o                  \
    gatt-builtin.o          \
    workQueue.o

OBJ_TEST =                  \
    test.o

LIBNEWBLUE = libnewblue.so

.PHONY: install clean

$(TEST): $(OBJ) $(OBJ_TEST) usb_vendor_plugin hci_vendor_plugin
	$(CC) $(CFLAGS) -o $@ -g $(OBJ) $(OBJ_TEST) -ldl -lpthread -lrt

$(LIBNEWBLUE): $(OBJ)
	$(CC) -shared -o $@ -pthread $(OBJ)

OBJ_UNITTEST = \
	tests/unittest.o \
	tests/sm_unittest.o \
	tests/persist_unittest.o

HEADERS = \
	att.h \
	bt.h \
	gatt.h \
	gatt-builtin.h \
	hci.h \
	l2cap.h \
	persist.h \
	sg.h \
	sm.h \
	timer.h \
	types.h \
	uniq.h

new_blue_unittest: $(OBJ_UNITTEST) $(OBJ)
	$(CXX) $(CPPFLAGS) -o $@ $^ -lgtest -ldl -lpthread

clean: usb_vendor_plugin_clean hci_vendor_plugin_clean
	@rm -f $(OBJ) $(OBJ_TEST) $(TEST) new_blue_unittest $(OBJ_UNITTEST)

all: $(TEST) usb_vendor_plugin hci_vendor_plugin new_blue_unittest

usb_vendor_plugin:
	$(MAKE) -C usb-hci

usb_vendor_plugin_clean:
	$(MAKE) -C usb-hci clean

hci_vendor_plugin:
	$(MAKE) -C hci_plugin CC=$(CC)

hci_vendor_plugin_clean:
	$(MAKE) -C hci_plugin clean

install: $(TEST) $(LIBNEWBLUE)
	$(INSTALL) --mode 755 -d $(DESTDIR)$(bindir)
	$(INSTALL) --mode 755 -t $(DESTDIR)$(bindir) $(TEST)
	$(INSTALL) --mode 755 -d $(DESTDIR)$(includedir)/newblue
	$(INSTALL) --mode 644 -t $(DESTDIR)$(includedir)/newblue $(HEADERS)
	$(INSTALL) --mode 755 -d $(DESTDIR)$(libdir)
	$(INSTALL) --mode 755 -t $(DESTDIR)$(libdir) $(LIBNEWBLUE)
	$(MAKE) libdir=$(libdir) -C hci_plugin install
