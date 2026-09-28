obj-m += chsc5448.o

KDIR ?= /lib/modules/$(shell uname -r)/build

.PHONY: all modules_install
all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

modules_install:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules_install
