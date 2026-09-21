/* SPDX-License-Identifier: GPL-2.0-only OR MIT */
#ifndef _BRIEFS_COMPAT_ACL_H
#define _BRIEFS_COMPAT_ACL_H

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/posix_acl.h>
#include <linux/posix_acl_xattr.h>

struct user_namespace;

/*
 * 7.0 moved the buffer allocation into posix_acl_to_xattr(): it now
 * returns a GFP-allocated buffer (or NULL) and reports the size through
 * *sizep, instead of the old probe-then-convert two-call dance.  The
 * old call sites all did exactly what the new helper does internally,
 * so the wrapper just picks the form.
 *
 * The pre-7.0 arm also fixes a latent bug in the old call site: the
 * probe's return value was stored in a size_t, so "size < 0" was always
 * false and a conversion error would have been carried into kmalloc()
 * as a huge size, surfacing as -ENOMEM anyway.  With a signed int the
 * original error is returned directly.
 */
static inline int briefs_compat_acl_to_xattr(struct user_namespace *user_ns,
					     const struct posix_acl *acl,
					     void **valuep, size_t *sizep)
{
#if BRIEFS_HAS_ACL_TO_XATTR_ALLOC
	*valuep = posix_acl_to_xattr(user_ns, acl, sizep, GFP_KERNEL);
	return *valuep ? 0 : -ENOMEM;
#else
	int size = posix_acl_to_xattr(user_ns, acl, NULL, 0);
	void *value;

	if (size < 0)
		return size;
	value = kmalloc(size, GFP_KERNEL);
	if (!value)
		return -ENOMEM;
	*sizep = size;
	*valuep = value;
	return posix_acl_to_xattr(user_ns, acl, value, size);
#endif
}

#endif /* _BRIEFS_COMPAT_ACL_H */