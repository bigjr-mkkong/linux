# Uncomment the prefix below if cross-compiling for a RISC-V simulator
# PREFIX = riscv64-linux-gnu-
CC     = gcc
CFLAGS = -Wall -Wextra -O3 -static

.PHONY: all clean

# The final executable name
TARGET = benchmark

# Benchmark source files
SRCS = main.c matmul_bench.c kmean_bench.c
OBJS = $(SRCS:.c=.o)

# Default target
all: $(TARGET)

# Build the static PIM library
libpim.a: pim_runtime.o
	ar rcs $@ $^

# Compile the PIM runtime object
pim_runtime.o: pim_runtime.c pim_runtime.h
	$(PREFIX)$(CC) $(CFLAGS) -c -o $@ $<

# Generic rule to compile benchmark object files
%.o: %.c bench.h
	$(PREFIX)$(CC) $(CFLAGS) -c -o $@ $<

# Link everything together into the final executable
$(TARGET): $(OBJS) libpim.a
	$(PREFIX)$(CC) $(CFLAGS) -o $@ $(OBJS) -L. -lpim -lpthread

clean:
	rm -f *.o *.a $(TARGET) example
