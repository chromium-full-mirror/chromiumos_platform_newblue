TEST := new_blue_test

CFLAGS = -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -g  -I. -DEXECUTABLE

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

OBJ +=                      \
    test.o

.PHONY: install clean

%.o: %.c
	$(CC) -o $@ $(CFLAGS) -c $<

$(TEST): $(OBJ) usb_vendor_plugin hci_vendor_plugin
	$(CC) -o $@ -g $(OBJ) -ldl -lpthread -lrt

clean: usb_vendor_plugin_clean hci_vendor_plugin_clean
	@rm -f *.o $(TEST)

all: $(TEST) usb_vendor_plugin hci_vendor_plugin

usb_vendor_plugin:
	$(MAKE) -C usb-hci

usb_vendor_plugin_clean:
	$(MAKE) -C usb-hci clean

hci_vendor_plugin:
	$(MAKE) -C hci_plugin CC=$(CC)

hci0_vendor_plugin_clean:
	$(MAKE) -C hci_plugin clean

install: $(TEST)
	$(INSTALL) -t /usr/bin -m644 $^
