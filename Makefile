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
briefs_fs-objs := briefs.o briefs_alloc.o briefs_journal.o crc32c.o briefs_ops.o briefs_trie.o briefs_trie_page.o briefs_super.o briefs_inode.o briefs_dir.o briefs_file.o briefs_extent.o briefs_btree.o briefs_debug.o briefs_sysfs.o briefs_proc.o briefs_export.o briefs_iomap.o briefs_xattr.o

# Build identifier: git revision of the source tree, surfaced via modinfo
# (/sys/module/briefs_fs/version) and the per-superblock sysfs/debugfs "build"
# attributes so the running module can be matched to the source it was built
# from.  Resolves to "unknown" outside a git tree (e.g. a plain tarball);
# --dirty marks uncommitted working-tree changes in the build.  $(src) is the
# kbuild external-module source dir, so the shell call only fires during the
# actual compile pass (where it is defined), not the top-level dispatch.
BRIEFS_GIT ?= $(if $(src),$(shell git -C $(src) describe --always --dirty 2>/dev/null))
ifeq ($(BRIEFS_GIT),)
BRIEFS_GIT := unknown
endif
ccflags-y += -DBRIEFS_BUILD_VERSION=\"$(BRIEFS_GIT)\"

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
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean

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
