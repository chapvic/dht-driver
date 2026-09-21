obj-m += dht.o

ifdef KERNELRELEASE

# Kbuild section - only module targets

else
# User section

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
ARCH ?=
CROSS_COMPILE ?=

.PHONY: all modules install uninstall clean check help

all: check modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules_install
	@echo "Running depmod..."
	@depmod -a
	@echo "Done. Run: sudo modprobe dht"

uninstall:
	@MODBASE=$$(dirname $(KDIR)); 	MODROOT=$$(dirname $$MODBASE); 	for ext in .ko .ko.xz .ko.gz .ko.zst .ko.bz2 .ko.lz4; do \
	    found=$$(find $$MODROOT -name "dht$$ext" 2>/dev/null); \
	    if [ -n "$$found" ]; then \
	        echo "Removing: $$found"; 	        rm -f $$found; 	    fi; \
	done
	@echo "Running depmod..."
	@depmod -a
	@echo "Module uninstalled."

check:
	@KVER=$$(cat $(KDIR)/Makefile 2>/dev/null | grep '^VERSION' | awk '{print $$3}'); 	PATCH=$$(cat $(KDIR)/Makefile 2>/dev/null | grep '^PATCHLEVEL' | awk '{print $$3}'); 	SUB=$$(cat $(KDIR)/Makefile 2>/dev/null | grep '^SUBLEVEL' | awk '{print $$3}'); 	if [ -z "$$KVER" ]; then 	    echo "ERROR: Cannot read kernel version from $(KDIR)/Makefile"; 	    echo "Make sure kernel headers are installed: sudo apt install linux-headers-$(uname -r)"; 	    exit 1; 	fi; 	echo "Kernel version: $$KVER.$$PATCH.$$SUB"; 	if [ $$KVER -lt 5 ]; then 	    echo "ERROR: Kernel $$KVER.$$PATCH.$$SUB is too old. Minimum required: 5.0"; 	    exit 1; 	fi; 	if [ -n "$(ARCH)" ] && [ -z "$(CROSS_COMPILE)" ]; then \
	    echo "WARNING: ARCH=$(ARCH) but CROSS_COMPILE is not set"; \
	    echo "         Cross-compilation may fail. Set CROSS_COMPILE if needed."; \
	fi; 	if [ -z "$(ARCH)" ] && [ -z "$(CROSS_COMPILE)" ]; then \
	    echo "Native build for $(shell uname -m)"; \
	fi; 	echo "Build checks passed."

clean:
	rm -rf *.o *.ko *.mod *.mod.c *.mod.o .module-common.o Module.symvers modules.order .tmp_versions
	find ./ -type f -name '.*cmd' -delete

help:
	@echo "DHT driver build targets:"
	@echo "  make          - check + build dht.ko"
	@echo "  make modules  - build dht.ko only"
	@echo "  make install  - build + install to /lib/modules/"
	@echo "  make uninstall- remove dht.ko from /lib/modules/"
	@echo "  make check    - verify kernel version and headers"
	@echo "  make clean    - remove build artifacts"
	@echo "  make help     - this message"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR=         - kernel build dir (default: /lib/modules/$$(uname -r)/build)"
	@echo "  ARCH=         - target arch for cross-compile (e.g. aarch64)"
	@echo "  CROSS_COMPILE=- toolchain prefix (e.g. aarch64-linux-gnu-)"

endif
