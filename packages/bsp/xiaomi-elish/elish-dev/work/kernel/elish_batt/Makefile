obj-m += elish_batt_agg.o

# 平板本机编译（默认用本机内核头）
KDIR ?= /usr/src/linux-headers-$(shell uname -r)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
