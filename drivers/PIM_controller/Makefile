obj-m += PIM_controller.o

ARCH ?= riscv

CROSS_COMPILE ?= riscv64-linux-gnu-

KDIR ?= ../../

all:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

clean:
	make -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) clean
