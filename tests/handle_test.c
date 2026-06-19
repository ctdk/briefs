// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Exercise BrieFS export_operations (encode_fh / fh_to_dentry) directly via
 * name_to_handle_at + open_by_handle_at. No nfsd required.
 *
 * Run as root on a BrieFS mount:
 *   handle_test <mountpoint>
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <linux/types.h>

#ifndef SYS_name_to_handle_at
#define SYS_name_to_handle_at 303
#endif
#ifndef SYS_open_by_handle_at
#define SYS_open_by_handle_at 304
#endif
#ifndef AT_HANDLE_CONNECTABLE
#define AT_HANDLE_CONNECTABLE 0x40
#endif

/* (AT_HANDLE_CONNECTABLE is unsupported on kernels < 6.13, so the connectable
 * encoding path is exercised by nfsd, not by this userspace test.) */

static int passes = 0, fails = 0;
#define PASS(msg) do { printf("  PASS: %s\n", msg); passes++; } while (0)
#define FAIL(msg, e) do { printf("  FAIL: %s (%s)\n", msg, strerror(e)); fails++; } while (0)

/* Open a file handle for reading; returns fd >= 0 or -1. */
static int open_handle(int mountfd, struct file_handle *fh)
{
	return (int)syscall(SYS_open_by_handle_at, mountfd, fh, O_RDONLY);
}

/* Encode a path to a file_handle (caller frees). Returns 0 on success. */
static struct file_handle *encode(int dfd, const char *name, int *mount_id)
{
	size_t sz = 128;
	struct file_handle *fh = calloc(1, sizeof(*fh) + sz);
	if (!fh)
		return NULL;
	fh->handle_bytes = sz;
	int ret = (int)syscall(SYS_name_to_handle_at, dfd, name, fh, mount_id, 0);
	if (ret < 0 && errno == EOVERFLOW) {
		/* Kernel told us the real size; reallocate and retry. */
		sz = fh->handle_bytes;
		free(fh);
		fh = calloc(1, sizeof(*fh) + sz);
		if (!fh)
			return NULL;
		fh->handle_bytes = sz;
		ret = (int)syscall(SYS_name_to_handle_at, dfd, name, fh, mount_id, 0);
	}
	if (ret < 0) {
		free(fh);
		return NULL;
	}
	return fh;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <mountpoint>\n", argv[0]);
		return 2;
	}
	const char *mnt = argv[1];
	char path[4096];
	int dfd, mountfd, fd, mount_id;

	/* Prepare content. */
	snprintf(path, sizeof(path), "%s/hfile", mnt);
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0) { perror("open hfile"); return 2; }
	write(fd, "hello-handle", 11);
	close(fd);

	/* Make a subdirectory with a file in it (exercises connectable/dir handles). */
	snprintf(path, sizeof(path), "%s/hsub", mnt);
	if (mkdir(path, 0755) < 0 && errno != EEXIST) { perror("mkdir"); return 2; }
	snprintf(path, sizeof(path), "%s/hsub/inner", mnt);
	fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
	if (fd < 0) { perror("open inner"); return 2; }
	write(fd, "inner-content", 13);
	close(fd);

	dfd = open(mnt, O_RDONLY | O_DIRECTORY);
	if (dfd < 0) { perror("open mnt"); return 2; }
	mountfd = open(mnt, O_RDONLY | O_DIRECTORY); /* open_by_handle_at needs a mount fd */
	if (mountfd < 0) { perror("open mnt mountfd"); return 2; }

	/* 1. Encode a regular file handle. */
	struct file_handle *fh = encode(dfd, "hfile", &mount_id);
	if (!fh) { FAIL("encode hfile", errno); goto done; }
	PASS("encode regular file handle");
	printf("    handle_type=%d handle_bytes=%u\n", fh->handle_type, fh->handle_bytes);

	/* 2. Open-by-handle and verify content. */
	fd = open_handle(mountfd, fh);
	if (fd < 0) { FAIL("open_by_handle_at hfile", errno); goto done; }
	char buf[64] = {0};
	ssize_t n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n == 11 && memcmp(buf, "hello-handle", 11) == 0)
		PASS("read back file via handle");
	else
		FAIL("read back file via handle", n < 0 ? errno : EBADE);

	/* 3. Encode a directory handle (connectable: includes parent ino+gen). */
	struct file_handle *dh = encode(dfd, "hsub", &mount_id);
	if (!dh) { FAIL("encode hsub dir", errno); goto done; }
	PASS("encode directory handle");
	printf("    handle_type=%d handle_bytes=%u\n", dh->handle_type, dh->handle_bytes);

	/* 4. Open the dir by handle (O_RDONLY|O_DIRECTORY via open_by_handle_at). */
	fd = (int)syscall(SYS_open_by_handle_at, mountfd, dh, O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		FAIL("open_by_handle_at hsub dir", errno);
	else {
		close(fd);
		PASS("open directory via handle");
	}

	/* 5. Stale handle: delete hfile, then try the old handle — must fail.
	 *    sync + drop_caches forces eviction of the unlinked inode so its
	 *    on-disk block is zeroed (briefs_free_inode_num), making the stale
	 *    outcome deterministic rather than dependent on cache eviction
	 *    timing. Any rejection (ESTALE, EINVAL, ENOENT) counts as a pass. */
	snprintf(path, sizeof(path), "%s/hfile", mnt);
	if (unlink(path) < 0) { FAIL("unlink hfile", errno); goto done; }
	sync();
	int dcfd = open("/proc/sys/vm/drop_caches", O_WRONLY);
	if (dcfd >= 0) {
		(void)write(dcfd, "3", 1);
		close(dcfd);
	}
	sync();
	fd = open_handle(mountfd, fh);
	if (fd < 0)
		PASS("stale handle rejected (expected failure)");
	else {
		close(fd);
		FAIL("stale handle rejected", 0);
	}
	printf("    stale open_by_handle_at errno=%d (%s)\n", errno, strerror(errno));

done:
	printf("\n  handle_test: %d passed, %d failed\n", passes, fails);
	return fails ? 1 : 0;
}