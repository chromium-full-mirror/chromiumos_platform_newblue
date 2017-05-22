CFLAGS = -Wall -g -I..

OBJ = vendor_lib.o

%.o: %.c
	$(CC) -fPIC -o $@ $(CFLAGS) -c $<

libbt-vendor.so: $(OBJ)
	$(CC) -fPIC -shared -o $@ -pthread -g $(OBJ)

clean:
	@rm -f *.o */*.o libbt-vendor.so

all: libbt-vendor.so
