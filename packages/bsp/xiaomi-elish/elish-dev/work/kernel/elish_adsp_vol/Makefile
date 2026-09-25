# SPDX-License-Identifier: GPL-2.0
#
# Out-of-tree ADM COPP master-gain module for Xiaomi Pad 5 Pro (sm8250/elish).
#
# On the device:
#   make -C /lib/modules/$(uname -r)/build M=$PWD modules
#   sudo insmod elish_adsp_vol.ko
#
# Or simply use ../build_and_test.sh.

obj-m := elish_adsp_vol.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install

.PHONY: all clean modules_install
