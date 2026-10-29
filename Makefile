CC ?= cc
CPPFLAGS ?= -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDFLAGS ?=
LDLIBS ?= -pthread -lrt

COMMON = common.o

.PHONY: all clean test
all: smserver smspooler smclient

smserver: smserver.o ipc.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

smspooler: smspooler.o ipc.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

smclient: smclient.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

ipc.o: ipc.c ipc.h common.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

test: all
	python3 tests/integration.py

clean:
	rm -f smserver smspooler smclient *.o
