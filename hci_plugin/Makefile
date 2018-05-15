CFLAGS = -Wall -g -I..
INSTALL = /usr/bin/install

SRCS = vendor_lib.c

RESULTS = libbt-vendor-hci0.so libbt-vendor-splitter.so
LIBBTVENDOR = libbt-vendor.so

all:  $(RESULTS)

libbt-vendor-splitter.so: $(SRCS)
	$(CC) -fPIC -shared -o $@ -pthread $(SRCS) $(CFLAGS) -DFILEPATH="\"/dev/hci0\""

libbt-vendor-hci0.so: $(SRCS)
	$(CC) -fPIC -shared -o $@ -pthread $(SRCS) $(CFLAGS) -DFILEPATH="\"/dev/hci_le\""

$(LIBBTVENDOR): libbt-vendor-hci0.so
	cp $< $@

install: $(LIBBTVENDOR)
	$(INSTALL) --mode 755 -d $(DESTDIR)$(libdir)
	$(INSTALL) --mode 755 -t $(DESTDIR)$(libdir) $(LIBBTVENDOR)

clean:
	@rm -f $(RESULTS)

