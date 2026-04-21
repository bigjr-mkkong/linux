CC      = gcc
CFLAGS  = -Wall -Wextra -O2

.PHONY: all clean

all: libpim.a example

libpim.a: pim_runtime.o
	ar rcs $@ $^

pim_runtime.o: pim_runtime.c pim_runtime.h
	$(CC) $(CFLAGS) -c -o $@ $<

example: example.c libpim.a
	$(CC) $(CFLAGS) -o $@ $< -L. -lpim -lpthread

clean:
	rm -f *.o *.a example
