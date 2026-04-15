PREFIX = riscv64-linux-gnu-
CC = gcc
CFLAGS += -Og -g

all : a.out
# SRC := sim.cpp

a.out: example.c
	$(PREFIX)$(CC) $< $(CFLAGS) -o $@

clean:
	rm -rf *.out

