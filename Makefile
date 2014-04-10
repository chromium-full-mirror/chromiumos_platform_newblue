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

$(TEST): $(OBJ)
	g++ -o $@ -g $(OBJ) -ldl -pthread -lrt

clean:
	@rm -f *.o $(TEST)

all: $(TEST)
