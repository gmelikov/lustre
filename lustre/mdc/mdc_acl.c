// SPDX-License-Identifier: GPL-2.0

/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#include <lustre_acl.h>

#include "mdc_internal.h"

int mdc_unpack_acl(struct req_capsule *pill, struct lustre_md *md)
{
	struct mdt_body	*body = md->body;
	struct posix_acl *acl;
	void *buf;
	int rc;

	/* for ACL, it's possible that FLACL is set but aclsize is zero.
	 * only when aclsize != 0 there's an actual segment for ACL
	 * in reply buffer.
	 */
	if (!body->mbo_aclsize) {
		md->posix_acl = NULL;
		return 0;
	}

	buf = req_capsule_server_sized_get(pill, &RMF_ACL, body->mbo_aclsize);
	if (!buf)
		return -EPROTO;

	acl = posix_acl_from_xattr(&init_user_ns, buf, body->mbo_aclsize);
	if (IS_ERR_OR_NULL(acl)) {
		rc = acl ? PTR_ERR(acl) : 0;
		CERROR("convert xattr to acl: %d\n", rc);
		return rc;
	}

	rc = posix_acl_valid(&init_user_ns, acl);
	if (rc) {
		CERROR("validate acl: %d\n", rc);
		posix_acl_release(acl);
		return rc;
	}

	md->posix_acl = acl;
	return 0;
}
