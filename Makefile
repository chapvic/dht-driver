# Makefile for DHT11/DHT22/AM2302 Temperature and Humidity Sensor Driver
# Version: 2.0
#
# Targets:
#   make            - Run pre-build checks, then build the module
#   make check      - Pre-build checks only
#   make modules    - Build without checks
#   make install    - Build, install to /lib/modules/$(uname -r)/, run depmod
#   make uninstall  - Search and remove dht.ko from all module directories
#   make clean      - Remove build artifacts
#   make help       - Show available targets and variables

obj-m += dht.o

# Kernel build directory
KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

# Kernel version extraction (3 methods with fallback)
# Method 1: official make kernelversion
KERNEL_FULL_VER := $(shell $(MAKE) -C $(KDIR) -s kernelversion 2>/dev/null)
# Method 2: grep from kernel Makefile
KMAJOR_GREP := $(shell grep -m1 '^VERSION' $(KDIR)/Makefile 2>/dev/null | tr -d ' \t' | cut -d= -f2)
KMINOR_GREP := $(shell grep -m1 '^PATCHLEVEL' $(KDIR)/Makefile 2>/dev/null | tr -d ' \t' | cut -d= -f2)
# Method 3: fallback to uname -r
UNAME_R := $(shell uname -r)
KMAJOR_UNAME := $(shell echo $(UNAME_R) | cut -d. -f1)
KMINOR_UNAME := $(shell echo $(UNAME_R) | cut -d. -f2)
# Select best available
KMAJOR := $(or $(KMAJOR_GREP),$(KMAJOR_UNAME))
KMINOR := $(or $(KMINOR_GREP),$(KMINOR_UNAME))

# Build flags
ccflags-y := -DDEBUG

.PHONY: all check modules install uninstall clean help

all: check modules

check:
	@echo "=== Pre-build checks ==="
	@echo "Kernel build dir: $(KDIR)"
	@if [ ! -d "$(KDIR)" ]; then \
		echo "ERROR: Kernel build directory not found: $(KDIR)"; \
		echo "Install kernel headers: sudo apt install linux-headers-$(shell uname -r)"; \
		exit 1; \
	fi
	@if [ ! -f "$(KDIR)/Makefile" ]; then \
		echo "ERROR: $(KDIR)/Makefile not found"; \
		echo "Kernel headers may be incomplete or corrupted"; \
		exit 1; \
	fi
	@if [ -z "$(KMAJOR)" ]; then \
		echo "ERROR: Cannot determine kernel version from $(KDIR)/Makefile"; \
		echo "Kernel Makefile may be missing or corrupted"; \
		exit 1; \
	fi
	@echo "Kernel version: $(KMAJOR).$(KMINOR)"
	@if [ $(KMAJOR) -lt 5 ]; then \
		echo "ERROR: Kernel 5.0+ required (found $(KMAJOR).$(KMINOR))"; \
		exit 1; \
	fi
	@if [ ! -f "$(KDIR)/include/generated/autoconf.h" ]; then \
		echo "WARNING: Kernel build not prepared (autoconf.h missing)"; \
		echo "Running: make -C $(KDIR) modules_prepare"; \
		$(MAKE) -C $(KDIR) modules_prepare 2>/dev/null || \
		echo "WARNING: modules_prepare failed, build may fail"; \
		echo "Kernel build prepared successfully"; \
	fi
	@echo "Pre-build checks passed"

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) modules

install: all
	$(MAKE) -C $(KDIR) M=$(PWD) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) 		INSTALL_MOD_PATH=$(INSTALL_MOD_PATH) modules_install
	@depmod -a $(shell uname -r) 2>/dev/null || true
	@echo "Module installed to /lib/modules/$(shell uname -r)/"

uninstall:
	@MODPATH=$$(find /lib/modules/$$(uname -r) -name "dht.ko*" -type f 2>/dev/null); \
	if [ -z "$$MODPATH" ]; then \
		echo "dht.ko not found in any module directory"; \
	else \
		for f in $$MODPATH; do \
		echo "Removing: $$f"; \
		rm -f "$$f"; \
		done; \
		depmod -a $$(uname -r) 2>/dev/null || true; \
		echo "Module uninstalled"; \
	fi

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order

help:
	@echo "DHT Sensor Driver Build System"
	@echo ""
	@echo "Targets:"
	@echo "  make            - Run pre-build checks, then build the module (default)"
	@echo "  make check      - Pre-build checks only"
	@echo "  make modules    - Build without checks"
	@echo "  make install    - Build, install to /lib/modules/, run depmod"
	@echo "  make uninstall  - Search and remove dht.ko from all directories"
	@echo "  make clean      - Remove build artifacts"
	@echo "  make help       - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  KDIR=           - Kernel build directory (default: /lib/modules/$$(uname -r)/build)"
	@echo "  ARCH=           - Target architecture (for cross-compilation)"
	@echo "  CROSS_COMPILE=  - Cross-compiler prefix (for cross-compilation)"
	@echo "  INSTALL_MOD_PATH= - Root filesystem for installation (for cross-compilation)"
