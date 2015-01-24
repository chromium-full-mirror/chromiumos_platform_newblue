CFLAGS = -Wall -g -I..

OBJ =               \
    bt_vendor_usb.o \
    usb.o           \
    usbhost.o

%.o: %.c
	gcc -fPIC -o $@ $(CFLAGS) -c $<

libbt-vendor.so: $(OBJ)
	gcc -fPIC -shared -o $@ -pthread -g $(OBJ)

clean:
	@rm -f *.o */*.o libbt-vendor.so

all: libbt-vendor.so
