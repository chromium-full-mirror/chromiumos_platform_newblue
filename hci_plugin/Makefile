CFLAGS = -Wall -g -I..

SRCS = vendor_lib.c

RESULTS = libbt-vendor-hci0.so libbt-vendor-splitter.so

all:  $(RESULTS)

libbt-vendor-splitter.so: $(SRCS)
	$(CC) -fPIC -shared -o $@ -pthread $(SRCS) $(CFLAGS) -DFILEPATH="\"/dev/hci0\""

libbt-vendor-hci0.so: $(SRCS)
	$(CC) -fPIC -shared -o $@ -pthread $(SRCS) $(CFLAGS) -DFILEPATH="\"/dev/hci_le\""

clean:
	@rm -f $(RESULTS)

