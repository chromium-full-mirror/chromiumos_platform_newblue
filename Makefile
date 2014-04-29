TEST := new_blue_test

CFLAGS = -Wall -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -g  -I. -DEXECUTABLE

# for pthread_setname_np
CFLAGS += -D_GNU_SOURCE

OBJ =                       \
    a2dp.o                  \
    att.o                   \
    gatt.o                  \
    hci.o                   \
    l2cap.o                 \
    mt.o                    \
    multiNotif.o            \
    persist.o               \
    rfcomm.o                \
    sdp.o                   \
    sendQ.o                 \
    sg.o                    \
    sm.o                    \
    timer.o                 \
    uniq.o                  \
    vendorLib.o             \
    workQueue.o

OBJ +=                      \
    test.o

%.o: %.c
	gcc -o $@ $(CFLAGS) -c $<

$(TEST): $(OBJ) usb_vendor_plugin
	g++ -o $@ -g $(OBJ) -ldl -pthread -lrt

clean: usb_vendor_plugin_clean
	@rm -f *.o $(TEST)

all: $(TEST) usb_vendor_plugin

usb_vendor_plugin:
	$(MAKE) -C usb-hci

usb_vendor_plugin_clean:
	$(MAKE) -C usb-hci clean
