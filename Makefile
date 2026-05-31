# Makefile for BrieFS. Currently very simple, hopefully will get more better
# later on.
#

obj-m += briefs.o

# Add C files as they appear.
briefs-objs := briefs_alloc.o briefs_journal.o crc32c.o

# Kernel build directory (change as needed)
KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

.PHONY: all clean
