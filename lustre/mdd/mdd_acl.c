// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2013, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Lustre Access Control List.
 *
 * Author: Fan Yong <fanyong@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_SEC
#include <lu_object.h>
#include <lustre_acl.h>
#include <obd_support.h>
#include <lustre_idmap.h>
#include <md_object.h>
#include "mdd_internal.h"

#ifdef CONFIG_LUSTRE_FS_POSIX_ACL

static inline void lustre_posix_acl_le_to_cpu(posix_acl_xattr_entry *d,
					      posix_acl_xattr_entry *s)
{
	d->e_tag = le16_to_cpu(s->e_tag);
	d->e_perm = le16_to_cpu(s->e_perm);
	d->e_id = le32_to_cpu(s->e_id);
}

#if 0
static inline void lustre_posix_acl_cpu_to_le(posix_acl_xattr_entry *d,
					      posix_acl_xattr_entry *s)
{
	d->e_tag = cpu_to_le16(s->e_tag);
	d->e_perm = cpu_to_le16(s->e_perm);
	d->e_id = cpu_to_le32(s->e_id);
}
#endif

/*
 * Check permission based on POSIX ACL.
 */
int lustre_posix_acl_permission(struct lu_ucred *mu, const struct lu_attr *la,
				unsigned int may_mask,
				posix_acl_xattr_entry *entry, int count)
{
	posix_acl_xattr_entry *pa, *pe, *mask_obj;
	posix_acl_xattr_entry ae, me;
	__u16 acl_want;
	int found = 0;

	if (count <= 0)
		return -EACCES;

	/* There is implicit conversion between MAY_* modes and ACL_* modes.
	 * Don't bother explicitly converting them unless they actually change.
	 */
	if (0) {
		acl_want = (may_mask & MAY_READ  ? ACL_READ : 0) |
			   (may_mask & MAY_WRITE ? ACL_WRITE : 0) |
			   (may_mask & MAY_EXEC  ? ACL_EXECUTE : 0);
	} else {
		BUILD_BUG_ON(MAY_READ != ACL_READ);
		BUILD_BUG_ON(MAY_WRITE != ACL_WRITE);
		BUILD_BUG_ON(MAY_EXEC != ACL_EXECUTE);

		acl_want = may_mask;
	}

	for (pa = &entry[0], pe = &entry[count - 1]; pa <= pe; pa++) {
		lustre_posix_acl_le_to_cpu(&ae, pa);
		switch (ae.e_tag) {
		case ACL_USER_OBJ:
			/* (May have been checked already) */
			if (la->la_uid == mu->uc_fsuid)
				goto check_perm;
			break;
		case ACL_USER:
			if (ae.e_id == mu->uc_fsuid)
				goto mask;
			break;
		case ACL_GROUP_OBJ:
			if (lustre_in_group_p(mu, la->la_gid)) {
				found = 1;
				if ((ae.e_perm & acl_want) == acl_want)
					goto mask;
			}
			break;
		case ACL_GROUP:
			if (lustre_in_group_p(mu, ae.e_id)) {
				found = 1;
				if ((ae.e_perm & acl_want) == acl_want)
					goto mask;
			}
			break;
		case ACL_MASK:
			break;
		case ACL_OTHER:
			if (found)
				return -EACCES;
			goto check_perm;
		default:
			return -EIO;
}
	}
	return -EIO;

mask:
	for (mask_obj = pa + 1; mask_obj <= pe; mask_obj++) {
		lustre_posix_acl_le_to_cpu(&me, mask_obj);
		if (me.e_tag == ACL_MASK) {
			if ((ae.e_perm & me.e_perm & acl_want) == acl_want)
				return 0;

			return -EACCES;
		}
	}

check_perm:
	if ((ae.e_perm & acl_want) == acl_want)
		return 0;

	return -EACCES;
}

/*
 * Modify the ACL for the chmod.
 */
int lustre_posix_acl_chmod_masq(posix_acl_xattr_entry *entry, u32 mode,
				int count)
{
	posix_acl_xattr_entry *group_obj = NULL, *mask_obj = NULL, *pa, *pe;

	/* There is implicit conversion between S_IRWX modes and ACL_* modes.
	 * Don't bother explicitly converting them unless they actually change.
	 */
	BUILD_BUG_ON(S_IROTH != ACL_READ);
	BUILD_BUG_ON(S_IWOTH != ACL_WRITE);
	BUILD_BUG_ON(S_IXOTH != ACL_EXECUTE);

	for (pa = &entry[0], pe = &entry[count - 1]; pa <= pe; pa++) {
		switch (le16_to_cpu(pa->e_tag)) {
		case ACL_USER_OBJ:
			pa->e_perm = cpu_to_le16((mode & S_IRWXU) >> 6);
			break;
		case ACL_USER:
		case ACL_GROUP:
			break;
		case ACL_GROUP_OBJ:
			group_obj = pa;
			break;
		case ACL_MASK:
			mask_obj = pa;
			break;
		case ACL_OTHER:
			pa->e_perm = cpu_to_le16(mode & S_IRWXO);
			break;
		default:
			return -EIO;
		}
	}

	if (mask_obj) {
		mask_obj->e_perm = cpu_to_le16((mode & S_IRWXG) >> 3);
	} else {
		if (!group_obj)
			return -EIO;
		group_obj->e_perm = cpu_to_le16((mode & S_IRWXG) >> 3);
	}

	return 0;
}

/*
 * Returns 0 if the acl can be exactly represented in the traditional
 * file mode permission bits, or else 1. Returns -E... on error.
 */
int
lustre_posix_acl_equiv_mode(posix_acl_xattr_entry *entry, mode_t *mode_p,
			    int count)
{
	posix_acl_xattr_entry *pa, *pe;
	mode_t mode = 0;
	int not_equiv = 0;

	for (pa = &entry[0], pe = &entry[count - 1]; pa <= pe; pa++) {
		__u16 perm = le16_to_cpu(pa->e_perm);
		switch (le16_to_cpu(pa->e_tag)) {
			case ACL_USER_OBJ:
				mode |= (perm & S_IRWXO) << 6;
				break;
			case ACL_GROUP_OBJ:
				mode |= (perm & S_IRWXO) << 3;
				break;
			case ACL_OTHER:
				mode |= perm & S_IRWXO;
				break;
			case ACL_MASK:
				mode = (mode & ~S_IRWXG) |
					((perm & S_IRWXO) << 3);
				not_equiv = 1;
				break;
			case ACL_USER:
			case ACL_GROUP:
				not_equiv = 1;
				break;
			default:
				return -EINVAL;
		}
	}
	if (mode_p)
		*mode_p = (*mode_p & ~S_IRWXUGO) | mode;
	return not_equiv;
}

/*
 * Modify acl when creating a new object.
 */
int lustre_posix_acl_create_masq(posix_acl_xattr_entry *entry, u32 *pmode,
				 int count)
{
	posix_acl_xattr_entry *group_obj = NULL, *mask_obj = NULL, *pa, *pe;
	posix_acl_xattr_entry ae;
	u32 mode = *pmode;
	int not_equiv = 0;

	for (pa = &entry[0], pe = &entry[count - 1]; pa <= pe; pa++) {
		lustre_posix_acl_le_to_cpu(&ae, pa);
		switch (ae.e_tag) {
		case ACL_USER_OBJ:
			ae.e_perm &= (mode >> 6) | ~(0007);
			pa->e_perm = cpu_to_le16(ae.e_perm);
			mode &= (ae.e_perm << 6) | ~S_IRWXU;
			break;
		case ACL_USER:
		case ACL_GROUP:
			not_equiv = 1;
			break;
		case ACL_GROUP_OBJ:
			group_obj = pa;
			break;
		case ACL_OTHER:
			ae.e_perm &= mode | ~(0007);
			pa->e_perm = cpu_to_le16(ae.e_perm);
			mode &= ae.e_perm | ~(0007);
			break;
		case ACL_MASK:
			mask_obj = pa;
			not_equiv = 1;
			break;
		default:
			return -EIO;
		}
	}

	if (mask_obj) {
		ae.e_perm = le16_to_cpu(mask_obj->e_perm) &
					((mode >> 3) | ~(0007));
		mode &= (ae.e_perm << 3) | ~S_IRWXG;
		mask_obj->e_perm = cpu_to_le16(ae.e_perm);
	} else {
		if (!group_obj)
			return -EIO;
		ae.e_perm = le16_to_cpu(group_obj->e_perm) &
					((mode >> 3) | ~(0007));
		mode &= (ae.e_perm << 3) | ~S_IRWXG;
		group_obj->e_perm = cpu_to_le16(ae.e_perm);
	}

	*pmode = (*pmode & ~S_IRWXUGO) | mode;
	return not_equiv;
}
#endif
