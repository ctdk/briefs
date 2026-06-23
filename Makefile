# Makefile for BrieFS.
#
# Build happens in the Vagrant VM (kernel versions differ from host).
#   make test   - build module in VM, run test suite
#   make local  - build for current (host) kernel
#   make test-local - build locally + run suite on host
#
# Override SSH with a different command on the command line.

obj-m += briefs_fs.o

# Add C files as they appear.
briefs_fs-objs := briefs.o briefs_alloc.o briefs_journal.o crc32c.o briefs_ops.o briefs_trie.o briefs_trie_page.o briefs_super.o briefs_inode.o briefs_dir.o briefs_file.o briefs_extent.o briefs_btree.o briefs_debug.o briefs_sysfs.o briefs_proc.o briefs_export.o briefs_iomap.o

# Kernel build directory (change as needed)
KDIR ?= /lib/modules/$(shell uname -r)/build

# VM SSH command — used by `make test`
VAGRANT_KEY ?= $(PWD)/.vagrant/machines/default/libvirt/private_key
SSH ?= ssh vagrant@192.168.121.234 -p 22 \
	-o LogLevel=FATAL \
	-o Compression=yes \
	-o DSAAuthentication=yes \
	-o IdentitiesOnly=yes \
	-o StrictHostKeyChecking=no \
	-o UserKnownHostsFile=/dev/null \
	-o PubkeyAcceptedKeyTypes=+ssh-rsa \
	-o HostKeyAlgorithms=+ssh-rsa \
	-i $(VAGRANT_KEY)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

# Default: build in VM, run tests there
.PHONY: test test-only

test:
	$(SSH) "cd /vagrant && make clean && make"
	$(MAKE) test-only

test-only:
	$(SSH) "cd /vagrant && sudo bash tests/test-runner.sh"

# Build on host (for running test-local or manual testing)
local: all

test-local: all
	sudo bash tests/test-runner.sh

.PHONY: all clean test test-only local test-local
