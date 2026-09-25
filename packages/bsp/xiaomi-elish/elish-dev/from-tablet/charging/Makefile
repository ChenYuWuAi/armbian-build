obj-m += bq2597x_elish.o

KDIR ?= /home/axis/axis_rnd/work/kernel/build/linux-6.12.58
ARCH ?= arm64
CROSS_COMPILE ?= aarch64-linux-gnu-
CC ?= aarch64-linux-gnu-gcc-12

all:
	$(MAKE) -C $(KDIR) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) CC=$(CC) M=$(PWD) modules
