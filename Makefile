obj-m := dht.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: all
	sudo cp dht.ko /lib/modules/$(shell uname -r)/extra/ 2>/dev/null || \
	sudo mkdir -p /lib/modules/$(shell uname -r)/extra && \
	sudo cp dht.ko /lib/modules/$(shell uname -r)/extra/
	sudo depmod -a
	@echo "Module installed. Run: sudo modprobe dht"

uninstall:
	sudo rm -f /lib/modules/$(shell uname -r)/extra/dht.ko
	sudo depmod -a
