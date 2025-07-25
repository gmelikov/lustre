// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Directory code for lustre client.
 */

#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/version.h>
#include <linux/security.h>
#include <linux/user_namespace.h>
#include <linux/uidgid.h>
#include <linux/uaccess.h>
#include <linux/buffer_head.h>   // for wait_on_buffer
#include <linux/pagevec.h>

#define DEBUG_SUBSYSTEM S_LLITE

#include <obd_support.h>
#include <obd_class.h>
#include <uapi/linux/lustre/lustre_ioctl.h>
#include <lustre_lib.h>
#include <lustre_dlm.h>
#include <lustre_compat.h>
#include <lustre_fid.h>
#include <lustre_kernelcomm.h>
#include <lustre_swab.h>
#include <lustre_quota.h>

#include "llite_internal.h"

/**
 * ll_get_dir_page() - Get directory page for a given directory inode
 *
 * (new) readdir implementation overview.
 *
 * Original lustre readdir implementation cached exact copy of raw directory
 * pages on the client. These pages were indexed in client page cache by
 * logical offset in the directory file. This design, while very simple and
 * intuitive had some inherent problems:
 *
 *     . it implies that byte offset to the directory entry serves as a
 *     telldir(3)/seekdir(3) cookie, but that offset is not stable: in
 *     ext3/htree directory entries may move due to splits, and more
 *     importantly,
 *
 *     . it is incompatible with the design of split directories for cmd3,
 *     that assumes that names are distributed across nodes based on their
 *     hash, and so readdir should be done in hash order.
 *
 * New readdir implementation does readdir in hash order, and uses hash of a
 * file name as a telldir/seekdir cookie. This led to number of complications:
 *
 *     . hash is not unique, so it cannot be used to index cached directory
 *     pages on the client (note, that it requires a whole pageful of hash
 *     collided entries to cause two pages to have identical hashes);
 *
 *     . hash is not unique, so it cannot, strictly speaking, be used as an
 *     entry cookie. ext3/htree has the same problem and lustre implementation
 *     mimics their solution: seekdir(hash) positions directory at the first
 *     entry with the given hash.
 *
 * Client side.
 *
 * 0. caching
 *
 * Client caches directory pages using hash of the first entry as an index. As
 * noted above hash is not unique, so this solution doesn't work as is:
 * special processing is needed for "page hash chains" (i.e., sequences of
 * pages filled with entries all having the same hash value).
 *
 * First, such chains have to be detected. To this end, server returns to the
 * client the hash of the first entry on the page next to one returned. When
 * client detects that this hash is the same as hash of the first entry on the
 * returned page, page hash collision has to be handled. Pages in the
 * hash chain, except first one, are termed "overflow pages".
 *
 * Proposed (unimplimented) solution to index uniqueness problem is to
 * not cache overflow pages.  Instead, when page hash collision is
 * detected, all overflow pages from emerging chain should be
 * immediately requested from the server and placed in a special data
 * structure.  This data structure can be used by ll_readdir() to
 * process entries from overflow pages.  When readdir invocation
 * finishes, overflow pages are discarded.  If page hash collision chain
 * weren't completely processed, next call to readdir will again detect
 * page hash collision, again read overflow pages in, process next
 * portion of entries and again discard the pages.  This is not as
 * wasteful as it looks, because, given reasonable hash, page hash
 * collisions are extremely rare.
 *
 * 1. directory positioning
 *
 * When seekdir(hash) is called.
 *
 * seekdir() sets the location in the directory stream from which the next
 * readdir() call will start. mdc_page_locate() is used to find page with
 * starting hash and will issue RPC to fetch that page. If there is a hash
 * collision the concerned page is removed.
 *
 *
 * Server.
 *
 * identification of and access to overflow pages
 *
 * page format
 *
 * Page in MDS_READPAGE RPC is packed in LU_PAGE_SIZE, and each page contains
 * a header lu_dirpage which describes the start/end hash, and whether this
 * page is empty (contains no dir entry) or hash collide with next page.
 * After client receives reply, several pages will be integrated into dir page
 * in PAGE_SIZE (if PAGE_SIZE greater than LU_PAGE_SIZE), and the
 * lu_dirpage for this integrated page will be adjusted. See
 * mdc_adjust_dirpages().
 *
 *
 * @dir: pointer to the directory(inode) for which page is being fetched
 * @op_data: pointer to the md operation structure
 * @offset: Offset witchin page
 * @partial_readdir_rc: Used only on partial reads
 *
 * Return:
 * * %Success - pointer to the page structure
 * * %Failure - Error pointer (pointed by rc)
 */
struct page *ll_get_dir_page(struct inode *dir, struct md_op_data *op_data,
			     __u64 offset, bool hash64, int *partial_readdir_rc)
{
	struct md_readdir_info mrinfo = {
					.mr_blocking_ast = ll_md_blocking_ast };
	struct page *page;
	unsigned long idx = hash_x_index(offset, hash64);
	int rc;

	/* check page first */
	page = find_get_page(dir->i_mapping, idx);
	if (page) {
		wait_on_page_locked(page);
		if (PageUptodate(page))
			RETURN(page);
		put_page(page);
	}

	rc = md_read_page(ll_i2mdexp(dir), op_data, &mrinfo, offset, &page);
	if (rc != 0)
		return ERR_PTR(rc);

	if (partial_readdir_rc && mrinfo.mr_partial_readdir_rc)
		*partial_readdir_rc = mrinfo.mr_partial_readdir_rc;

	return page;
}

void ll_release_page(struct inode *inode, struct page *page,
		     bool remove)
{
	/* Always remove the page for striped dir, because the page is
	 * built from temporarily in LMV layer
	 */
	if (inode && ll_dir_striped(inode)) {
		__free_page(page);
		return;
	}

	if (remove) {
		lock_page(page);
		if (likely(page->mapping != NULL))
			cfs_delete_from_page_cache(page);
		unlock_page(page);
	}
	put_page(page);
}

#ifdef HAVE_DIR_CONTEXT
int ll_dir_read(struct inode *inode, __u64 *ppos, struct md_op_data *op_data,
		struct dir_context *ctx, int *partial_readdir_rc)
{
#else
int ll_dir_read(struct inode *inode, __u64 *ppos, struct md_op_data *op_data,
		void *cookie, filldir_t filldir, int *partial_readdir_rc)
{
#endif
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	__u64 pos = *ppos;
	bool is_api32 = ll_need_32bit_api(sbi);
	bool is_hash64 = test_bit(LL_SBI_64BIT_HASH, sbi->ll_flags);
	struct page *page;
	bool done = false;
	struct llcrypt_str lltr = LLTR_INIT(NULL, 0);
	int rc = 0;

	ENTRY;

	if (IS_ENCRYPTED(inode)) {
		rc = llcrypt_fname_alloc_buffer(inode, NAME_MAX, &lltr);
		if (rc < 0)
			RETURN(rc);
	}

	page = ll_get_dir_page(inode, op_data, pos, is_hash64,
				partial_readdir_rc);

	while (rc == 0 && !done) {
		void *kaddr = NULL;
		struct lu_dirpage *dp;
		struct lu_dirent  *ent;
		__u64 hash;
		__u64 next;

		if (IS_ERR(page)) {
			rc = PTR_ERR(page);
			break;
		}

		hash = MDS_DIR_END_OFF;
		kaddr = kmap(page);
		dp = kaddr;
		for (ent = lu_dirent_start(dp); ent != NULL && !done;
		     ent = lu_dirent_next(ent)) {
			__u16          type;
			int            namelen;
			struct lu_fid  fid;
			__u64          lhash;
			__u64          ino;

			hash = le64_to_cpu(ent->lde_hash);
			if (hash < pos) /* Skip until we find target hash */
				continue;

			namelen = le16_to_cpu(ent->lde_namelen);
			if (namelen == 0) /* Skip dummy record */
				continue;

			if (is_api32 && is_hash64)
				lhash = hash >> 32;
			else
				lhash = hash;
			fid_le_to_cpu(&fid, &ent->lde_fid);
			ino = cl_fid_build_ino(&fid, is_api32);
			type = S_DT(lu_dirent_type_get(ent));
			/* For ll_nfs_get_name_filldir(), it will try to access
			 * 'ent' through 'lde_name', so the parameter 'name'
			 * for 'filldir()' must be part of the 'ent'.
			 */
#ifdef HAVE_DIR_CONTEXT
			ctx->pos = lhash;
			if (!IS_ENCRYPTED(inode)) {
				done = !dir_emit(ctx, ent->lde_name, namelen,
						 ino, type);
			} else {
				/* Directory is encrypted */
				int save_len = lltr.len;
				struct llcrypt_str de_name =
					LLTR_INIT(ent->lde_name, namelen);

				rc = ll_fname_disk_to_usr(inode, 0, 0, &de_name,
							  &lltr, &fid);
				de_name = lltr;
				lltr.len = save_len;
				if (rc) {
					done = 1;
					break;
				}
				done = !dir_emit(ctx, de_name.name, de_name.len,
						 ino, type);
			}
#else
			/* HAVE_DIR_CONTEXT is defined from kernel 3.11, whereas
			 * IS_ENCRYPTED is brought by kernel 4.14.
			 * So there is no need to handle encryption case here.
			 */
			done = filldir(cookie, ent->lde_name, namelen, lhash,
				       ino, type);
#endif
		}

		if (done) {
			pos = hash;
			if (kaddr) {
				kunmap(kmap_to_page(kaddr));
				kaddr = NULL;
			}
			ll_release_page(inode, page, false);
			break;
		}

		next = le64_to_cpu(dp->ldp_hash_end);
		pos = next;
		if (pos == MDS_DIR_END_OFF) {
			/* End of directory reached. */
			done = 1;
			if (kaddr) {
				kunmap(kmap_to_page(kaddr));
				kaddr = NULL;
			}
			ll_release_page(inode, page, false);
		} else {
			u32 flags = le32_to_cpu(dp->ldp_flags);

			/* Normal case: continue to the next page.*/
			if (kaddr) {
				kunmap(kmap_to_page(kaddr));
				kaddr = NULL;
			}
			ll_release_page(inode, page, flags & LDF_COLLIDE);
			next = pos;
			page = ll_get_dir_page(inode, op_data, pos,
					       is_hash64, partial_readdir_rc);
		}
	}
#ifdef HAVE_DIR_CONTEXT
	ctx->pos = pos;
#else
	*ppos = pos;
#endif
	llcrypt_fname_free_buffer(&lltr);
	RETURN(rc);
}

#ifdef HAVE_DIR_CONTEXT
static int ll_iterate(struct file *filp, struct dir_context *ctx)
#else
static int ll_readdir(struct file *filp, void *cookie, filldir_t filldir)
#endif
{
	struct inode *inode = file_inode(filp);
	struct ll_file_data *lfd = filp->private_data;
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	bool hash64 = test_bit(LL_SBI_64BIT_HASH, sbi->ll_flags);
	int api32 = ll_need_32bit_api(sbi);
	struct md_op_data *op_data;
	struct lu_fid pfid = { 0 };
	ktime_t kstart = ktime_get();
	/* result of possible partial readdir */
	int partial_readdir_rc = 0;
	__u64 pos;
	int rc;

	ENTRY;

	LASSERT(lfd != NULL);
	pos = lfd->lfd_pos;

	CDEBUG(D_VFSTRACE,
	       "VFS Op:inode="DFID"(%p) pos/size%lu/%llu 32bit_api %d\n",
	       PFID(ll_inode2fid(inode)),
	       inode, (unsigned long)pos, i_size_read(inode), api32);

	if (IS_ENCRYPTED(inode)) {
		rc = llcrypt_prepare_readdir(inode);
		if (rc && rc != -ENOKEY)
			GOTO(out, rc);
	}

	if (pos == MDS_DIR_END_OFF)
		/* end-of-file. */
		GOTO(out, rc = 0);

	if (unlikely(ll_dir_striped(inode))) {
		struct dentry *parent = dget_parent(file_dentry(filp));
		struct inode *i_dir = d_inode(parent);

		/* Only needed for striped dir to fill ..see lmv_read_page() */
		if (i_dir) {
			struct obd_export *exp = ll_i2mdexp(i_dir);
			enum mds_ibits_locks ibits = MDS_INODELOCK_LOOKUP;

			if (ll_have_md_lock(exp, i_dir, &ibits, LCK_MODE_MIN,0))
				pfid = *ll_inode2fid(i_dir);
		}
		dput(parent);

		/* If it can not find in cache, do lookup on the master obj */
		if (fid_is_zero(&pfid)) {
			rc = ll_dir_get_parent_fid(inode, &pfid);
			if (rc != 0)
				RETURN(rc);
		}
	}

	op_data = ll_prep_md_op_data(NULL, inode, inode, NULL, 0, 0,
				     LUSTRE_OPC_ANY, inode);
	if (IS_ERR(op_data))
		GOTO(out, rc = PTR_ERR(op_data));

	/* foreign dirs are browsed out of Lustre */
	if (unlikely(lmv_dir_foreign(op_data->op_lso1))) {
		ll_finish_md_op_data(op_data);
		RETURN(-ENODATA);
	}

	op_data->op_fid3 = pfid;

#ifdef HAVE_DIR_CONTEXT
	ctx->pos = pos;
	rc = ll_dir_read(inode, &pos, op_data, ctx, &partial_readdir_rc);
	pos = ctx->pos;
#else
	rc = ll_dir_read(inode, &pos, op_data, cookie, filldir,
			 &partial_readdir_rc);
#endif
	lfd->lfd_pos = pos;
	if (!lfd->fd_partial_readdir_rc)
		lfd->fd_partial_readdir_rc = partial_readdir_rc;

	if (pos == MDS_DIR_END_OFF) {
		if (api32)
			pos = LL_DIR_END_OFF_32BIT;
		else
			pos = LL_DIR_END_OFF;
	} else {
		if (api32 && hash64)
			pos = pos >> 32;
	}
#ifdef HAVE_DIR_CONTEXT
	ctx->pos = pos;
#else
	filp->f_pos = pos;
#endif
	ll_finish_md_op_data(op_data);

out:
	if (!rc)
		ll_stats_ops_tally(sbi, LPROC_LL_READDIR,
				   ktime_us_delta(ktime_get(), kstart));

	RETURN(rc);
}

/**
 * ll_dir_setdirstripe() - Create striped directory with specified stripe(@lump)
 *
 * @dparent: the parent of the directory.
 * @lump: the specified stripes.
 * @len: length of @lump
 * @dirname: the name of the directory.
 * @mode: the specified mode of the directory.
 * @createonly: if true, setstripe create only, don't restripe if target exists
 *
 * Return:
 * * %0 if striped directory is being created successfully or <0 if the
 * creation is failed
 */
static int ll_dir_setdirstripe(struct dentry *dparent, struct lmv_user_md *lump,
			       size_t len, const char *dirname, umode_t mode,
			       bool createonly)
{
	struct inode *parent = dparent->d_inode;
	struct ptlrpc_request *request = NULL;
	struct md_op_data *op_data;
	struct ll_sb_info *sbi = ll_i2sbi(parent);
	struct inode *inode = NULL;
	struct dentry dentry = {
		.d_parent = dparent,
		.d_name = {
			.name = dirname,
			.len = strlen(dirname),
			.hash = ll_full_name_hash(dparent, dirname,
						  strlen(dirname)),
		},
		.d_sb = dparent->d_sb,
	};
	bool encrypt = false;
	int hash_flags;
	int err;

	ENTRY;
	if (unlikely(!lmv_user_magic_supported(lump->lum_magic)))
		RETURN(-EINVAL);

	if (lump->lum_magic != LMV_MAGIC_FOREIGN) {
		CDEBUG(D_VFSTRACE,
		       "VFS Op:inode="DFID"(%p) name="DNAME" stripe_offset=%d stripe_count=%u, hash_type=%x\n",
		       PFID(ll_inode2fid(parent)), parent,
		       encode_fn_dentry(&dentry), (int)lump->lum_stripe_offset,
		       lump->lum_stripe_count, lump->lum_hash_type);
	} else {
		struct lmv_foreign_md *lfm = (struct lmv_foreign_md *)lump;

		CDEBUG(D_VFSTRACE,
		       "VFS Op:inode="DFID"(%p) name "DNAME" foreign, length %u, value '"DNAME"'\n",
		       PFID(ll_inode2fid(parent)), parent,
		       encode_fn_dentry(&dentry), lfm->lfm_length,
		       lfm->lfm_length, lfm->lfm_value);
	}

	if (lump->lum_stripe_count > 1 &&
	    !(exp_connect_flags(sbi->ll_md_exp) & OBD_CONNECT_DIR_STRIPE))
		RETURN(-EINVAL);

	if (IS_DEADDIR(parent) &&
	    !CFS_FAIL_CHECK(OBD_FAIL_LLITE_NO_CHECK_DEAD))
		RETURN(-ENOENT);

	/* MDS < 2.14 doesn't support 'crush' hash type, and cannot handle
	 * unknown hash if client doesn't set a valid one. switch to fnv_1a_64.
	 */
	if (CFS_FAIL_CHECK(OBD_FAIL_LMV_UNKNOWN_STRIPE)) {
		lump->lum_hash_type = cfs_fail_val;
	} else if (!(exp_connect_flags2(sbi->ll_md_exp) & OBD_CONNECT2_CRUSH)) {
		enum lmv_hash_type type = lump->lum_hash_type &
					  LMV_HASH_TYPE_MASK;

		if (type >= LMV_HASH_TYPE_CRUSH ||
		    type == LMV_HASH_TYPE_UNKNOWN)
			lump->lum_hash_type = (lump->lum_hash_type ^ type) |
					      LMV_HASH_TYPE_FNV_1A_64;
	}

	hash_flags = lump->lum_hash_type & ~LMV_HASH_TYPE_MASK;
	if (hash_flags & ~LMV_HASH_FLAG_KNOWN)
		RETURN(-EINVAL);

	if (unlikely(!lmv_user_magic_supported(cpu_to_le32(lump->lum_magic))))
		lustre_swab_lmv_user_md(lump);

	if (!IS_POSIXACL(parent) || !exp_connect_umask(ll_i2mdexp(parent)))
		mode &= ~current_umask();
	mode = (mode & (S_IRWXUGO | S_ISVTX)) | S_IFDIR;
	op_data = ll_prep_md_op_data(NULL, parent, NULL, dirname,
				     strlen(dirname), mode, LUSTRE_OPC_MKDIR,
				     lump);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	op_data->op_dir_depth = ll_i2info(parent)->lli_inherit_depth ?:
				ll_i2info(parent)->lli_dir_depth;

	if (ll_sbi_has_encrypt(sbi) &&
	    (IS_ENCRYPTED(parent) ||
	     unlikely(ll_sb_has_test_dummy_encryption(parent->i_sb)))) {
		err = llcrypt_prepare_readdir(parent);
		if (err)
			GOTO(out_op_data, err);
		if (!llcrypt_has_encryption_key(parent))
			GOTO(out_op_data, err = -ENOKEY);
		encrypt = true;
	}

	if (test_bit(LL_SBI_FILE_SECCTX, sbi->ll_flags)) {
		/* selinux_dentry_init_security() uses dentry->d_parent and name
		 * to determine the security context for the file. So our fake
		 * dentry should be real enough for this purpose.
		 */
		err = ll_dentry_init_security(&dentry, mode, &dentry.d_name,
					      &op_data->op_file_secctx_name,
					      &op_data->op_file_secctx_name_size,
					      &op_data->op_file_secctx,
					      &op_data->op_file_secctx_size,
					      &op_data->op_file_secctx_slot);
		if (err < 0)
			GOTO(out_op_data, err);
	}

	if (encrypt) {
		err = llcrypt_inherit_context(parent, NULL, op_data, false);
		if (err)
			GOTO(out_op_data, err);
	}

	op_data->op_cli_flags |= CLI_SET_MEA;
	if (createonly)
		op_data->op_bias |= MDS_SETSTRIPE_CREATE;

	err = md_create(sbi->ll_md_exp, op_data, lump, len, mode,
			from_kuid(&init_user_ns, current_fsuid()),
			from_kgid(&init_user_ns, current_fsgid()),
			current_cap(), 0, &request);
	if (err)
		GOTO(out_request, err);

	CFS_FAIL_TIMEOUT(OBD_FAIL_LLITE_SETDIRSTRIPE_PAUSE, cfs_fail_val);

	err = ll_prep_inode(&inode, &request->rq_pill, parent->i_sb, NULL);
	if (err)
		GOTO(out_inode, err);

	dentry.d_inode = inode;

	if (test_bit(LL_SBI_FILE_SECCTX, sbi->ll_flags))
		err = ll_inode_notifysecctx(inode, op_data->op_file_secctx,
					    op_data->op_file_secctx_size);
	else
		err = ll_inode_init_security(&dentry, inode, parent);

	if (err)
		GOTO(out_inode, err);

	if (encrypt) {
		err = ll_set_encflags(inode, op_data->op_file_encctx,
				      op_data->op_file_encctx_size, false);
		if (err)
			GOTO(out_inode, err);
	}

out_inode:
	iput(inode);
out_request:
	ptlrpc_req_put(request);
out_op_data:
	ll_finish_md_op_data(op_data);

	return err;
}

int ll_dir_setstripe(struct inode *inode, struct lov_user_md *lump,
		     int set_default)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct md_op_data *op_data;
	struct ptlrpc_request *req = NULL;
	int rc = 0;
	int lum_size;

	ENTRY;
	if (lump != NULL) {
		switch (lump->lmm_magic) {
		case LOV_USER_MAGIC_V1:
			lum_size = sizeof(struct lov_user_md_v1);
			break;
		case LOV_USER_MAGIC_V3:
			lum_size = sizeof(struct lov_user_md_v3);
			break;
		case LOV_USER_MAGIC_COMP_V1:
			lum_size = ((struct lov_comp_md_v1 *)lump)->lcm_size;
			break;
		case LMV_USER_MAGIC: {
			struct lmv_user_md *lmv = (struct lmv_user_md *)lump;

			/* MDS < 2.14 doesn't support 'crush' hash type, and
			 * cannot handle unknown hash if client doesn't set a
			 * valid one. switch to fnv_1a_64.
			 */
			if (!(exp_connect_flags2(sbi->ll_md_exp) &
			      OBD_CONNECT2_CRUSH)) {
				enum lmv_hash_type type = lmv->lum_hash_type &
							  LMV_HASH_TYPE_MASK;

				if (type >= LMV_HASH_TYPE_CRUSH ||
				    type == LMV_HASH_TYPE_UNKNOWN)
					lmv->lum_hash_type =
						(lmv->lum_hash_type ^ type) |
						LMV_HASH_TYPE_FNV_1A_64;
			}
			if (lmv->lum_magic != cpu_to_le32(LMV_USER_MAGIC))
				lustre_swab_lmv_user_md(lmv);
			lum_size = sizeof(*lmv);
			break;
		}
		case LOV_USER_MAGIC_SPECIFIC: {
			struct lov_user_md_v3 *v3 =
				(struct lov_user_md_v3 *)lump;
			if (v3->lmm_stripe_count > LOV_MAX_STRIPE_COUNT)
				RETURN(-EINVAL);
			lum_size = lov_user_md_size(v3->lmm_stripe_count,
						    LOV_USER_MAGIC_SPECIFIC);
			break;
		}
		default:
			CDEBUG(D_IOCTL,
			       "bad userland LOV MAGIC: %#08x != %#08x nor %#08x\n",
			       lump->lmm_magic, LOV_USER_MAGIC_V1,
			       LOV_USER_MAGIC_V3);
			RETURN(-EINVAL);
		}

		/* This is coming from userspace, so should be in
		 * local endian.  But the MDS would like it in little
		 * endian, so we swab it before we send it.
		 */
		if ((__swab32(lump->lmm_magic) & le32_to_cpu(LOV_MAGIC_MASK)) ==
		    le32_to_cpu(LOV_MAGIC_MAGIC))
			lustre_swab_lov_user_md(lump, 0);
	} else {
		lum_size = sizeof(struct lov_user_md_v1);
	}

	op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	/* swabbing is done in lov_setstripe() on server side */
	rc = md_setattr(sbi->ll_md_exp, op_data, lump, lum_size, &req);
	ll_finish_md_op_data(op_data);
	ptlrpc_req_put(req);
	if (rc)
		RETURN(rc);

	RETURN(rc);
}

/* get default LMV from client cache */
static int ll_dir_get_default_lmv(struct inode *inode, struct lmv_user_md *lum)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	const struct lmv_stripe_md *lsm;
	bool fs_dmv_got = false;
	int rc = -ENODATA;

	ENTRY;
retry:
	if (lli->lli_def_lsm_obj) {
		down_read(&lli->lli_lsm_sem);
		lsm = &lli->lli_def_lsm_obj->lso_lsm;
		if (lsm) {
			lum->lum_magic = lsm->lsm_md_magic;
			lum->lum_stripe_count = lsm->lsm_md_stripe_count;
			lum->lum_stripe_offset = lsm->lsm_md_master_mdt_index;
			lum->lum_hash_type = lsm->lsm_md_hash_type;
			lum->lum_max_inherit = lsm->lsm_md_max_inherit;
			lum->lum_max_inherit_rr = lsm->lsm_md_max_inherit_rr;
			rc = 0;
		}
		up_read(&lli->lli_lsm_sem);
	}

	if (rc == -ENODATA && !is_root_inode(inode) && !fs_dmv_got) {
		lli = ll_i2info(inode->i_sb->s_root->d_inode);
		fs_dmv_got = true;
		goto retry;
	}

	if (!rc && fs_dmv_got) {
		lli = ll_i2info(inode);
		if (lum->lum_max_inherit != LMV_INHERIT_UNLIMITED) {
			if (lum->lum_max_inherit == LMV_INHERIT_NONE ||
			    lum->lum_max_inherit < LMV_INHERIT_END ||
			    lum->lum_max_inherit > LMV_INHERIT_MAX ||
			    lum->lum_max_inherit <= lli->lli_dir_depth)
				GOTO(out, rc = -ENODATA);

			lum->lum_max_inherit -= lli->lli_dir_depth;
		}

		if (lum->lum_max_inherit_rr != LMV_INHERIT_RR_UNLIMITED) {
			if (lum->lum_max_inherit_rr == LMV_INHERIT_NONE ||
			    lum->lum_max_inherit_rr < LMV_INHERIT_RR_END ||
			    lum->lum_max_inherit_rr > LMV_INHERIT_RR_MAX ||
			    lum->lum_max_inherit_rr <= lli->lli_dir_depth)
				lum->lum_max_inherit_rr = LMV_INHERIT_RR_NONE;

			if (lum->lum_max_inherit_rr > lli->lli_dir_depth)
				lum->lum_max_inherit_rr -= lli->lli_dir_depth;
		}
	}
out:
	RETURN(rc);
}

int ll_dir_get_default_layout(struct inode *inode, void **plmm, int *plmm_size,
			      struct ptlrpc_request **request, u64 valid,
			      enum get_default_layout_type type)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct mdt_body   *body;
	struct lov_mds_md *lmm = NULL;
	struct ptlrpc_request *req = NULL;
	int lmm_size = OBD_MAX_DEFAULT_EA_SIZE;
	struct md_op_data *op_data;
	struct lu_fid fid;
	int rc;

	ENTRY;

	op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, lmm_size,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	op_data->op_valid = valid | OBD_MD_FLEASIZE | OBD_MD_FLDIREA;

	if (type == GET_DEFAULT_LAYOUT_ROOT) {
		lu_root_fid(&op_data->op_fid1);
		fid = op_data->op_fid1;
	} else {
		fid = *ll_inode2fid(inode);
	}

	rc = md_getattr(sbi->ll_md_exp, op_data, &req);
	ll_finish_md_op_data(op_data);
	if (rc < 0) {
		CDEBUG(D_INFO, "md_getattr failed on inode "DFID": rc %d\n",
		       PFID(&fid), rc);
		GOTO(out, rc);
	}

	body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
	LASSERT(body != NULL);

	lmm_size = body->mbo_eadatasize;

	if (!(body->mbo_valid & (OBD_MD_FLEASIZE | OBD_MD_FLDIREA)) ||
	    lmm_size == 0) {
		GOTO(out, rc = -ENODATA);
	}

	lmm = req_capsule_server_sized_get(&req->rq_pill,
					   &RMF_MDT_MD, lmm_size);
	LASSERT(lmm != NULL);

	/* This is coming from the MDS, so is probably in
	 * little endian.  We convert it to host endian before
	 * passing it to userspace.
	 */
	/* We don't swab objects for directories */
	switch (le32_to_cpu(lmm->lmm_magic)) {
	case LOV_MAGIC_V1:
	case LOV_MAGIC_V3:
	case LOV_MAGIC_COMP_V1:
	case LOV_USER_MAGIC_SPECIFIC:
		if (LOV_MAGIC != cpu_to_le32(LOV_MAGIC))
			lustre_swab_lov_user_md((struct lov_user_md *)lmm, 0);
		break;
	case LMV_MAGIC_V1:
		if (LMV_MAGIC != cpu_to_le32(LMV_MAGIC))
			lustre_swab_lmv_mds_md((union lmv_mds_md *)lmm);
		break;
	case LMV_USER_MAGIC:
		if (LMV_USER_MAGIC != cpu_to_le32(LMV_USER_MAGIC))
			lustre_swab_lmv_user_md((struct lmv_user_md *)lmm);
		break;
	case LMV_MAGIC_FOREIGN: {
		struct lmv_foreign_md *lfm = (struct lmv_foreign_md *)lmm;

		if (LMV_MAGIC_FOREIGN != cpu_to_le32(LMV_MAGIC_FOREIGN)) {
			__swab32s(&lfm->lfm_magic);
			__swab32s(&lfm->lfm_length);
			__swab32s(&lfm->lfm_type);
			__swab32s(&lfm->lfm_flags);
		}
		break;
	}
	default:
		rc = -EPROTO;
		CERROR("%s: unknown magic: %lX: rc = %d\n", sbi->ll_fsname,
		       (unsigned long)lmm->lmm_magic, rc);
	}
out:
	*plmm = lmm;
	*plmm_size = lmm_size;
	*request = req;
	return rc;
}

/**
 * ll_dir_getstripe_default() - Get default layout (striping information) for
 * directory
 *
 * This function will be used to get default LOV/LMV/Default LMV
 * @valid will be used to indicate which stripe it will retrieve.
 * If the directory does not have its own default layout, then the
 * function will request the default layout from root FID.
 *	OBD_MD_MEA		LMV stripe EA
 *	OBD_MD_DEFAULT_MEA	Default LMV stripe EA
 *	otherwise		Default LOV EA.
 * Each time, it can only retrieve 1 stripe EA
 *
 * @inode: inode for which layout is to be get
 * @plmm: Returns address of valid layout metadata (struct lov_mds_md)
 * @plmm_size: Returns size of the layout metadata
 * @request: Returns ptlrpc_request struct which gets the layout
 * @root_request: Returns ptlrpc_request struct which get the layout (for root
 * access)
 * @valid: indicate which stripe it will retrieve
 *
 * Return:
 * * %0: Success
 * * %-ERRNO: Failure
 */
int ll_dir_getstripe_default(struct inode *inode, void **plmm, int *plmm_size,
			     struct ptlrpc_request **request,
			     struct ptlrpc_request **root_request,
			     u64 valid)
{
	struct ptlrpc_request *req = NULL;
	struct ptlrpc_request *root_req = NULL;
	struct lov_mds_md *lmm = NULL;
	int lmm_size = 0;
	int rc = 0;

	ENTRY;
	rc = ll_dir_get_default_layout(inode, (void **)&lmm, &lmm_size,
				       &req, valid, 0);
	if (rc == -ENODATA && !fid_is_root(ll_inode2fid(inode)) &&
	    !(valid & OBD_MD_MEA) && root_request != NULL) {
		int rc2 = ll_dir_get_default_layout(inode, (void **)&lmm,
						    &lmm_size, &root_req, valid,
						    GET_DEFAULT_LAYOUT_ROOT);
		if (rc2 == 0)
			rc = 0;
	}

	*plmm = lmm;
	*plmm_size = lmm_size;
	*request = req;
	if (root_request != NULL)
		*root_request = root_req;

	RETURN(rc);
}

/**
 * ll_dir_getstripe() - Wrapper function to ll_dir_get_default_layout
 *
 * This function will be used to get default LOV/LMV/Default LMV
 * @valid will be used to indicate which stripe it will retrieve
 *	OBD_MD_MEA		LMV stripe EA
 *	OBD_MD_DEFAULT_MEA	Default LMV stripe EA
 *	otherwise		Default LOV EA.
 * Each time, it can only retrieve 1 stripe EA
 *
 * @inode: inode for which layout is to be get
 * @plmm: Returns address of valid layout metadata (struct lov_mds_md)
 * @plmm_size: Returns size of the layout metadata
 * @request: Returns ptlrpc_request struct which gets the layout
 * @valid: indicate which stripe it will retrieve
 *
 * Return:
 * * %0: Success
 * * %-ERRNO: Failure
 */
int ll_dir_getstripe(struct inode *inode, void **plmm, int *plmm_size,
		     struct ptlrpc_request **request, u64 valid)
{
	struct ptlrpc_request *req = NULL;
	struct lov_mds_md *lmm = NULL;
	int lmm_size = 0;
	int rc = 0;

	ENTRY;
	rc = ll_dir_get_default_layout(inode, (void **)&lmm, &lmm_size,
				       &req, valid, 0);

	*plmm = lmm;
	*plmm_size = lmm_size;
	*request = req;

	RETURN(rc);
}

int ll_get_mdt_idx_by_fid(struct ll_sb_info *sbi, const struct lu_fid *fid)
{
	struct md_op_data *op_data;
	int rc;
	int mdt_index;

	ENTRY;
	OBD_ALLOC_PTR(op_data);
	if (op_data == NULL)
		RETURN(-ENOMEM);

	op_data->op_flags |= MF_GET_MDT_IDX;
	op_data->op_fid1 = *fid;
	rc = md_getattr(sbi->ll_md_exp, op_data, NULL);
	mdt_index = op_data->op_mds;
	OBD_FREE_PTR(op_data);
	if (rc < 0)
		RETURN(rc);

	RETURN(mdt_index);
}

/*
 *  Get MDT index for the inode.
 */
int ll_get_mdt_idx(struct inode *inode)
{
	return ll_get_mdt_idx_by_fid(ll_i2sbi(inode), ll_inode2fid(inode));
}

/*
 * ll_ioc_copy_start() - Generic handler to do any pre-copy work.
 *
 * It sends a first hsm_progress (with extent length == 0) to coordinator as a
 * first information for it that real work has started.
 *
 * Moreover, for a ARCHIVE request, it will sample the file data version and
 * store it in \a copy.
 *
 * Return:
 * * %0 On success or <0 on failure
 */
static int ll_ioc_copy_start(struct super_block *sb, struct hsm_copy *copy)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	struct hsm_progress_kernel hpk;
	int rc = 0;
	int rc2;

	ENTRY;
	/* Forge a hsm_progress based on data from copy. */
	hpk.hpk_fid = copy->hc_hai.hai_fid;
	hpk.hpk_cookie = copy->hc_hai.hai_cookie;
	hpk.hpk_extent.offset = copy->hc_hai.hai_extent.offset;
	hpk.hpk_extent.length = 0;
	hpk.hpk_flags = 0;
	hpk.hpk_errval = 0;
	hpk.hpk_data_version = 0;


	/* For archive request, we need to read the current file version. */
	if (copy->hc_hai.hai_action == HSMA_ARCHIVE) {
		struct inode	*inode;
		__u64		 data_version = 0;

		/* Get inode for this fid */
		inode = search_inode_for_lustre(sb, &copy->hc_hai.hai_fid);
		if (IS_ERR(inode)) {
			hpk.hpk_flags |= HP_FLAG_RETRY;
			/* hpk_errval is >= 0 */
			hpk.hpk_errval = -PTR_ERR(inode);
			GOTO(progress, rc = PTR_ERR(inode));
		}

		/* Read current file data version */
		rc = ll_data_version(inode, &data_version, LL_DV_RD_FLUSH);
		iput(inode);
		if (rc != 0) {
			CDEBUG(D_HSM, "Could not read file data version of "
				      DFID" (rc = %d). Archive request ("
				      "%#llx) could not be done.\n",
				      PFID(&copy->hc_hai.hai_fid), rc,
				      copy->hc_hai.hai_cookie);
			hpk.hpk_flags |= HP_FLAG_RETRY;
			/* hpk_errval must be >= 0 */
			hpk.hpk_errval = -rc;
			GOTO(progress, rc);
		}

		/* Store in the hsm_copy for later copytool use.
		 * Always modified even if no lsm.
		 */
		copy->hc_data_version = data_version;
	}

progress:
	/* On error, the request should be considered as completed */
	if (hpk.hpk_errval > 0)
		hpk.hpk_flags |= HP_FLAG_COMPLETED;

	rc2 = obd_iocontrol(LL_IOC_HSM_PROGRESS, sbi->ll_md_exp, sizeof(hpk),
			    &hpk, NULL);

	/* Return first error */
	RETURN(rc != 0 ? rc : rc2);
}

/*
 * ll_ioc_copy_end() - Generic handler to do any post-copy work.
 *
 * It will send the last hsm_progress update to coordinator to inform it
 * that copy is finished and whether it was successful or not.
 *
 * Moreover,
 * - for ARCHIVE request, it will sample the file data version and compare it
 *   with the version saved in ll_ioc_copy_start(). If they do not match, copy
 *   will be considered as failed.
 * - for RESTORE request, it will sample the file data version and send it to
 *   coordinator which is useful if the file was imported as 'released'.
 *
 * Return:
 * * %0 On success or <0 on failure
 */
static int ll_ioc_copy_end(struct super_block *sb, struct hsm_copy *copy)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	struct hsm_progress_kernel hpk;
	int rc = 0;
	int rc2;

	ENTRY;
	/* If you modify the logic here, also check llapi_hsm_copy_end(). */
	/* Take care: copy->hc_hai.hai_action, len, gid and data are not
	 * initialized if copy_end was called with copy == NULL.
	 */

	/* Forge a hsm_progress based on data from copy. */
	hpk.hpk_fid = copy->hc_hai.hai_fid;
	hpk.hpk_cookie = copy->hc_hai.hai_cookie;
	hpk.hpk_extent = copy->hc_hai.hai_extent;
	hpk.hpk_flags = copy->hc_flags | HP_FLAG_COMPLETED;
	hpk.hpk_errval = copy->hc_errval;
	hpk.hpk_data_version = 0;

	/* For archive request, we need to check the file data was not changed.
	 *
	 * For restore request, we need to send the file data version, this is
	 * useful when the file was created using hsm_import.
	 */
	if (((copy->hc_hai.hai_action == HSMA_ARCHIVE) ||
	     (copy->hc_hai.hai_action == HSMA_RESTORE)) &&
	    (copy->hc_errval == 0)) {
		struct inode	*inode;
		__u64		 data_version = 0;

		/* Get lsm for this fid */
		inode = search_inode_for_lustre(sb, &copy->hc_hai.hai_fid);
		if (IS_ERR(inode)) {
			hpk.hpk_flags |= HP_FLAG_RETRY;
			/* hpk_errval must be >= 0 */
			hpk.hpk_errval = -PTR_ERR(inode);
			GOTO(progress, rc = PTR_ERR(inode));
		}

		rc = ll_data_version(inode, &data_version, LL_DV_RD_FLUSH);
		iput(inode);
		if (rc) {
			CDEBUG(D_HSM,
			       "Could not read file data version. Request could not be confirmed.\n");
			if (hpk.hpk_errval == 0)
				hpk.hpk_errval = -rc;
			GOTO(progress, rc);
		}

		/* Store in the hsm_copy for later copytool use.
		 * Always modified even if no lsm.
		 */
		hpk.hpk_data_version = data_version;

		/* File could have been stripped during archiving, so we need
		 * to check anyway.
		 */
		if ((copy->hc_hai.hai_action == HSMA_ARCHIVE) &&
		    (copy->hc_data_version != data_version)) {
			CDEBUG(D_HSM, "File data version mismatched. "
			      "File content was changed during archiving. "
			       DFID", start:%#llx current:%#llx\n",
			       PFID(&copy->hc_hai.hai_fid),
			       copy->hc_data_version, data_version);
			/* File was changed, send error to cdt. Do not ask for
			 * retry because if a file is modified frequently,
			 * the cdt will loop on retried archive requests.
			 * The policy engine will ask for a new archive later
			 * when the file will not be modified for some tunable
			 * time
			 */
			hpk.hpk_flags &= ~HP_FLAG_RETRY;
			rc = -EBUSY;
			/* hpk_errval must be >= 0 */
			hpk.hpk_errval = -rc;
			GOTO(progress, rc);
		}

	}

progress:
	rc2 = obd_iocontrol(LL_IOC_HSM_PROGRESS, sbi->ll_md_exp, sizeof(hpk),
			    &hpk, NULL);

	/* Return first error */
	RETURN(rc != 0 ? rc : rc2);
}


static int copy_and_ct_start(int cmd, struct obd_export *exp,
			     const struct lustre_kernelcomm __user *data)
{
	struct lustre_kernelcomm *lk;
	struct lustre_kernelcomm *tmp;
	size_t size = sizeof(*lk);
	size_t new_size;
	int i;
	int rc;

	/* copy data from userspace to get numbers of archive_id */
	OBD_ALLOC(lk, size);
	if (lk == NULL)
		return -ENOMEM;

	if (copy_from_user(lk, data, size))
		GOTO(out_lk, rc = -EFAULT);

	if (lk->lk_flags & LK_FLG_STOP)
		goto do_ioctl;

	if (!(lk->lk_flags & LK_FLG_DATANR)) {
		__u32 archive_mask = lk->lk_data_count;
		int count;

		/* old hsm agent to old MDS */
		if (!exp_connect_archive_id_array(exp))
			goto do_ioctl;

		/* old hsm agent to new MDS */
		lk->lk_flags |= LK_FLG_DATANR;

		if (archive_mask == 0)
			goto do_ioctl;

		count = hweight32(archive_mask);
		new_size = offsetof(struct lustre_kernelcomm, lk_data[count]);
		OBD_ALLOC(tmp, new_size);
		if (tmp == NULL)
			GOTO(out_lk, rc = -ENOMEM);

		memcpy(tmp, lk, size);
		tmp->lk_data_count = count;
		OBD_FREE(lk, size);
		lk = tmp;
		size = new_size;

		count = 0;
		for (i = 0; i < sizeof(archive_mask) * 8; i++) {
			if (BIT(i) & archive_mask) {
				lk->lk_data[count] = i + 1;
				count++;
			}
		}
		goto do_ioctl;
	}

	/* new hsm agent to new mds */
	if (lk->lk_data_count > 0) {
		new_size = offsetof(struct lustre_kernelcomm,
				    lk_data[lk->lk_data_count]);
		OBD_ALLOC(tmp, new_size);
		if (tmp == NULL)
			GOTO(out_lk, rc = -ENOMEM);

		OBD_FREE(lk, size);
		lk = tmp;
		size = new_size;

		if (copy_from_user(lk, data, size))
			GOTO(out_lk, rc = -EFAULT);
	}

	/* new hsm agent to old MDS */
	if (!exp_connect_archive_id_array(exp)) {
		__u32 archives = 0;

		if (lk->lk_data_count > LL_HSM_ORIGIN_MAX_ARCHIVE)
			GOTO(out_lk, rc = -EINVAL);

		for (i = 0; i < lk->lk_data_count; i++) {
			if (lk->lk_data[i] > LL_HSM_ORIGIN_MAX_ARCHIVE) {
				rc = -EINVAL;
				CERROR("%s: archive id %d requested but only [0 - %zu] supported: rc = %d\n",
				       exp->exp_obd->obd_name, lk->lk_data[i],
				       LL_HSM_ORIGIN_MAX_ARCHIVE, rc);
				GOTO(out_lk, rc);
			}

			if (lk->lk_data[i] == 0) {
				archives = 0;
				break;
			}

			archives |= (1 << (lk->lk_data[i] - 1));
		}
		lk->lk_flags &= ~LK_FLG_DATANR;
		lk->lk_data_count = archives;
	}
do_ioctl:
	rc = obd_iocontrol(cmd, exp, size, lk, NULL);
out_lk:
	OBD_FREE(lk, size);
	return rc;
}

static int check_owner(int type, int id)
{
	switch (type) {
	case USRQUOTA:
		if (!uid_eq(current_euid(), make_kuid(&init_user_ns, id)))
			return -EPERM;
		break;
	case GRPQUOTA:
		if (!in_egroup_p(make_kgid(&init_user_ns, id)))
			return -EPERM;
		break;
	case PRJQUOTA:
		break;
	}
	return 0;
}

struct kmem_cache *quota_iter_slab;
static DEFINE_MUTEX(quotactl_iter_lock);

struct ll_quotactl_iter_list {
	__u64		 lqil_mark;	 /* iter identifier */
	__u32		 lqil_flags;	 /* what has been done */
	pid_t		 lqil_pid;	 /* debug calling task */
	time64_t	 lqil_iter_time; /* the time to iter */
	struct list_head lqil_sbi_list;	 /* list on ll_sb_info */
	struct list_head lqil_quotactl_iter_list; /* list of quota iters */
};

void ll_quota_iter_check_and_cleanup(struct ll_sb_info *sbi, bool check)
{
	struct if_quotactl_iter *iter_rec = NULL;
	struct ll_quotactl_iter_list *tmp, *ll_iter = NULL;

	if (!check)
		mutex_lock(&quotactl_iter_lock);

	list_for_each_entry_safe(ll_iter, tmp, &sbi->ll_all_quota_list,
				 lqil_sbi_list) {
		if (check &&
		    ll_iter->lqil_iter_time > (ktime_get_seconds() - 86400))
			continue;

		while ((iter_rec = list_first_entry_or_null(
					&ll_iter->lqil_quotactl_iter_list,
					struct if_quotactl_iter,
					qci_link)) != NULL) {
			list_del_init(&iter_rec->qci_link);
			OBD_SLAB_FREE_PTR(iter_rec, quota_iter_slab);
		}

		list_del_init(&ll_iter->lqil_sbi_list);
		OBD_FREE_PTR(ll_iter);
	}

	if (!check)
		mutex_unlock(&quotactl_iter_lock);
}

/* iterate the quota usage from all QSDs */
static int quotactl_iter_acct(struct list_head *quota_list, void *buffer,
			      __u64 size, __u64 *count, __u32 qtype, bool is_md)
{
	struct if_quotactl_iter *tmp, *iter = NULL;
	struct lquota_acct_rec *acct;
	__u64 qid, cur = 0;
	int rc = 0;

	ENTRY;

	while (cur < size) {
		if ((size - cur) <
		    (sizeof(qid) + sizeof(*acct))) {
			rc = -EPROTO;
			break;
		}

		qid = *((__u64 *)(buffer + cur));
		cur += sizeof(qid);
		acct = (struct lquota_acct_rec *)(buffer + cur);
		cur += sizeof(*acct);

		iter = NULL;
		list_for_each_entry(tmp, quota_list, qci_link) {
			if (tmp->qci_qc.qc_id == (__u32)qid) {
				iter = tmp;
				break;
			}
		}

		if (iter == NULL) {
			CDEBUG(D_QUOTA, "can't find the iter record for %llu\n",
			       qid);

			if (qid != 0)
				continue;

			OBD_SLAB_ALLOC_PTR(iter, quota_iter_slab);
			if (iter == NULL) {
				rc = -ENOMEM;
				break;
			}

			INIT_LIST_HEAD(&iter->qci_link);
			iter->qci_qc.qc_id = 0;
			iter->qci_qc.qc_type = qtype;
			(*count)++;

			list_add(&iter->qci_link, quota_list);
		}

		if (is_md) {
			iter->qci_qc.qc_dqblk.dqb_valid |= QIF_INODES;
			iter->qci_qc.qc_dqblk.dqb_curinodes += acct->ispace;
			iter->qci_qc.qc_dqblk.dqb_curspace += acct->bspace;
		} else {
			iter->qci_qc.qc_dqblk.dqb_valid |= QIF_SPACE;
			iter->qci_qc.qc_dqblk.dqb_curspace += acct->bspace;
		}
	}

	RETURN(rc);
}

/* iterate all quota settings from QMT */
static int quotactl_iter_glb(struct list_head *quota_list, void *buffer,
			     __u64 size, __u64 *count, __u32 qtype, bool is_md)
{
	struct if_quotactl_iter *tmp, *iter = NULL;
	struct lquota_glb_rec *glb;
	__u64 qid, cur = 0;
	bool inserted = false;
	int rc = 0;

	ENTRY;

	while (cur < size) {
		if ((size - cur) <
		    (sizeof(qid) + sizeof(*glb))) {
			rc = -EPROTO;
			break;
		}

		qid = *((__u64 *)(buffer + cur));
		cur += sizeof(qid);
		glb = (struct lquota_glb_rec *)(buffer + cur);
		cur += sizeof(*glb);

		iter = NULL;
		list_for_each_entry(tmp, quota_list, qci_link) {
			if (tmp->qci_qc.qc_id == (__u32)qid) {
				iter = tmp;
				break;
			}
		}

		if (iter == NULL) {
			OBD_SLAB_ALLOC_PTR(iter, quota_iter_slab);
			if (iter == NULL) {
				rc = -ENOMEM;
				break;
			}

			INIT_LIST_HEAD(&iter->qci_link);

			inserted = false;
			list_for_each_entry(tmp, quota_list, qci_link) {
				if (tmp->qci_qc.qc_id < qid)
					continue;

				inserted = true;
				list_add_tail(&iter->qci_link,
					      &tmp->qci_link);
				break;
			}

			if (!inserted)
				list_add_tail(&iter->qci_link, quota_list);

			iter->qci_qc.qc_type = qtype;
			iter->qci_qc.qc_id = (__u32)qid;
			(*count)++;
		}

		if (is_md) {
			iter->qci_qc.qc_dqblk.dqb_valid |= QIF_ILIMITS;
			iter->qci_qc.qc_dqblk.dqb_ihardlimit =
							     glb->qbr_hardlimit;
			iter->qci_qc.qc_dqblk.dqb_isoftlimit =
							     glb->qbr_softlimit;
			iter->qci_qc.qc_dqblk.dqb_itime = glb->qbr_time;
		} else {
			iter->qci_qc.qc_dqblk.dqb_valid |= QIF_BLIMITS;
			iter->qci_qc.qc_dqblk.dqb_bhardlimit =
							     glb->qbr_hardlimit;
			iter->qci_qc.qc_dqblk.dqb_bsoftlimit =
							     glb->qbr_softlimit;
			iter->qci_qc.qc_dqblk.dqb_btime = glb->qbr_time;
		}
	}

	RETURN(rc);
}

/* iterate the quota setting from QMT and all QSDs to get the quota information
 * for all users or groups
 */
static int quotactl_iter(struct ll_sb_info *sbi, struct if_quotactl *qctl)
{
	struct list_head iter_quota_glb_list;
	struct list_head iter_obd_quota_md_list;
	struct list_head iter_obd_quota_dt_list;
	struct ll_quotactl_iter_list *ll_iter;
	struct lquota_iter *iter;
	struct obd_quotactl *oqctl;
	__u64 count;
	int rc = 0;

	ENTRY;

	OBD_ALLOC_PTR(ll_iter);
	if (ll_iter == NULL)
		RETURN(-ENOMEM);

	INIT_LIST_HEAD(&ll_iter->lqil_sbi_list);
	INIT_LIST_HEAD(&ll_iter->lqil_quotactl_iter_list);

	mutex_lock(&quotactl_iter_lock);

	if (!list_empty(&sbi->ll_all_quota_list))
		ll_quota_iter_check_and_cleanup(sbi, true);

	INIT_LIST_HEAD(&iter_quota_glb_list);
	INIT_LIST_HEAD(&iter_obd_quota_md_list);
	INIT_LIST_HEAD(&iter_obd_quota_dt_list);

	OBD_ALLOC_PTR(oqctl);
	if (oqctl == NULL)
		GOTO(out, rc = -ENOMEM);

	QCTL_COPY(oqctl, qctl);
	oqctl->qc_iter_list = (uintptr_t)&iter_quota_glb_list;
	rc = obd_quotactl(sbi->ll_md_exp, oqctl);
	if (rc)
		GOTO(cleanup, rc);

	QCTL_COPY(oqctl, qctl);
	oqctl->qc_cmd = LUSTRE_Q_ITEROQUOTA;
	oqctl->qc_iter_list = (uintptr_t)&iter_obd_quota_md_list;
	rc = obd_quotactl(sbi->ll_md_exp, oqctl);
	if (rc)
		GOTO(cleanup, rc);

	QCTL_COPY(oqctl, qctl);
	oqctl->qc_cmd = LUSTRE_Q_ITEROQUOTA;
	oqctl->qc_iter_list = (uintptr_t)&iter_obd_quota_dt_list;
	rc = obd_quotactl(sbi->ll_dt_exp, oqctl);
	if (rc)
		GOTO(cleanup, rc);

	count = 0;
	while ((iter = list_first_entry_or_null(&iter_quota_glb_list,
						struct lquota_iter, li_link))) {
		void *buffer;

		buffer = iter->li_buffer;
		rc = quotactl_iter_glb(&ll_iter->lqil_quotactl_iter_list,
				       buffer, iter->li_md_size, &count,
				       oqctl->qc_type, true);
		if (rc)
			GOTO(cleanup, rc);

		buffer = iter->li_buffer + LQUOTA_ITER_BUFLEN / 2;
		rc = quotactl_iter_glb(&ll_iter->lqil_quotactl_iter_list,
				       buffer, iter->li_dt_size, &count,
				       oqctl->qc_type,  false);

		if (rc)
			GOTO(cleanup, rc);

		list_del_init(&iter->li_link);
		OBD_FREE_LARGE(iter,
			       sizeof(struct lquota_iter) + LQUOTA_ITER_BUFLEN);
	}

	while ((iter = list_first_entry_or_null(&iter_obd_quota_md_list,
						struct lquota_iter, li_link))) {
		rc = quotactl_iter_acct(&ll_iter->lqil_quotactl_iter_list,
					iter->li_buffer, iter->li_md_size,
					&count, oqctl->qc_type, true);
		if (rc)
			GOTO(cleanup, rc);

		list_del_init(&iter->li_link);
		OBD_FREE_LARGE(iter,
			       sizeof(struct lquota_iter) + LQUOTA_ITER_BUFLEN);
	}

	while ((iter = list_first_entry_or_null(&iter_obd_quota_dt_list,
						struct lquota_iter, li_link))) {
		rc = quotactl_iter_acct(&ll_iter->lqil_quotactl_iter_list,
					iter->li_buffer, iter->li_dt_size,
					&count, oqctl->qc_type, false);
		if (rc)
			GOTO(cleanup, rc);

		list_del_init(&iter->li_link);
		OBD_FREE_LARGE(iter,
			       sizeof(struct lquota_iter) + LQUOTA_ITER_BUFLEN);
	}

	ll_iter->lqil_mark = ((__u64)current->pid << 32) |
			     ((__u32)qctl->qc_type << 8) |
			     (ktime_get_seconds() & 0xFFFFFF);
	ll_iter->lqil_flags = qctl->qc_type;
	ll_iter->lqil_pid = current->pid;
	ll_iter->lqil_iter_time = ktime_get_seconds();

	list_add(&ll_iter->lqil_sbi_list, &sbi->ll_all_quota_list);

	qctl->qc_allquota_count = count;
	qctl->qc_allquota_mark = ll_iter->lqil_mark;
	GOTO(out, rc);

cleanup:
	ll_quota_iter_check_and_cleanup(sbi, true);

	while ((iter = list_first_entry_or_null(&iter_quota_glb_list,
						struct lquota_iter, li_link))) {
		list_del_init(&iter->li_link);
		OBD_FREE_LARGE(iter,
			       sizeof(struct lquota_iter) + LQUOTA_ITER_BUFLEN);
	}

	while ((iter = list_first_entry_or_null(&iter_obd_quota_md_list,
						struct lquota_iter, li_link))) {
		list_del_init(&iter->li_link);
		OBD_FREE_LARGE(iter,
			       sizeof(struct lquota_iter) + LQUOTA_ITER_BUFLEN);
	}

	while ((iter = list_first_entry_or_null(&iter_obd_quota_dt_list,
						struct lquota_iter, li_link))) {
		list_del_init(&iter->li_link);
		OBD_FREE_LARGE(iter,
			       sizeof(struct lquota_iter) + LQUOTA_ITER_BUFLEN);
	}

	OBD_FREE_PTR(ll_iter);

out:
	OBD_FREE_PTR(oqctl);

	mutex_unlock(&quotactl_iter_lock);
	RETURN(rc);
}

static int quotactl_getallquota(struct ll_sb_info *sbi,
				struct if_quotactl *qctl)
{
	struct ll_quotactl_iter_list *ll_iter = NULL;
	struct if_quotactl_iter *iter = NULL;
	void __user *buffer = (void __user *)qctl->qc_allquota_buffer;
	__u64 cur = 0, count = qctl->qc_allquota_buflen;
	bool found = false;
	int rc = 0;

	ENTRY;

	mutex_lock(&quotactl_iter_lock);

	list_for_each_entry(ll_iter, &sbi->ll_all_quota_list, lqil_sbi_list) {
		if (qctl->qc_allquota_mark == ll_iter->lqil_mark) {
			found = true;
			break;
		}
	}

	if (!found) {
		mutex_unlock(&quotactl_iter_lock);
		RETURN(-EBUSY);
	}

	while ((iter = list_first_entry_or_null(
					&ll_iter->lqil_quotactl_iter_list,
					struct if_quotactl_iter, qci_link))) {
		if (count - cur < sizeof(struct if_quotactl)) {
			rc = -ERANGE;
			break;
		}

		if (copy_to_user(buffer + cur, &iter->qci_qc,
				 sizeof(struct if_quotactl))) {
			rc = -EFAULT;
			break;
		}

		cur += sizeof(struct if_quotactl);

		list_del_init(&iter->qci_link);
		OBD_SLAB_FREE_PTR(iter, quota_iter_slab);
	}

	/* cleanup in case of error */
	while ((iter = list_first_entry_or_null(
					&ll_iter->lqil_quotactl_iter_list,
					struct if_quotactl_iter, qci_link))) {
		list_del_init(&iter->qci_link);
		OBD_SLAB_FREE_PTR(iter, quota_iter_slab);
	}

	list_del_init(&ll_iter->lqil_sbi_list);
	OBD_FREE_PTR(ll_iter);

	mutex_unlock(&quotactl_iter_lock);

	RETURN(rc);
}

int quotactl_ioctl(struct super_block *sb, struct if_quotactl *qctl)
{
	struct ll_sb_info *sbi = ll_s2sbi(sb);
	int cmd = qctl->qc_cmd;
	int type = qctl->qc_type;
	int id = qctl->qc_id;
	int valid = qctl->qc_valid;
	int rc = 0;

	ENTRY;

	switch (cmd) {
	case Q_SETQUOTA:
	case Q_SETINFO:
	case LUSTRE_Q_SETDEFAULT:
	case LUSTRE_Q_SETQUOTAPOOL:
	case LUSTRE_Q_SETINFOPOOL:
	case LUSTRE_Q_SETDEFAULT_POOL:
	case LUSTRE_Q_DELETEQID:
	case LUSTRE_Q_RESETQID:
		if (!capable(CAP_SYS_ADMIN))
			RETURN(-EPERM);

		if (sb->s_flags & SB_RDONLY)
			RETURN(-EROFS);
		break;
	case Q_GETQUOTA:
	case LUSTRE_Q_GETDEFAULT:
	case LUSTRE_Q_GETQUOTAPOOL:
	case LUSTRE_Q_GETDEFAULT_POOL:
	case LUSTRE_Q_ITERQUOTA:
	case LUSTRE_Q_GETALLQUOTA:
		if (check_owner(type, id) &&
		    (!capable(CAP_SYS_ADMIN)))
			RETURN(-EPERM);
		break;
	case Q_GETINFO:
	case LUSTRE_Q_GETINFOPOOL:
		break;
	default:
		CERROR("%s: unsupported quotactl op: %#x: rc = %d\n",
		       sbi->ll_fsname, cmd, -EOPNOTSUPP);
		RETURN(-EOPNOTSUPP);
	}

	if (cmd == LUSTRE_Q_ITERQUOTA) {
		rc = quotactl_iter(sbi, qctl);
	} else if (cmd == LUSTRE_Q_GETALLQUOTA) {
		rc = quotactl_getallquota(sbi, qctl);
	} else if (valid != QC_GENERAL) {
		if (cmd == Q_GETINFO)
			qctl->qc_cmd = Q_GETOINFO;
		else if (cmd == Q_GETQUOTA ||
			 cmd == LUSTRE_Q_GETQUOTAPOOL)
			qctl->qc_cmd = Q_GETOQUOTA;
		else
			RETURN(-EINVAL);

		switch (valid) {
		case QC_MDTIDX:
			rc = obd_iocontrol(OBD_IOC_QUOTACTL, sbi->ll_md_exp,
					   sizeof(*qctl), qctl, NULL);
			break;
		case QC_OSTIDX:
			rc = obd_iocontrol(OBD_IOC_QUOTACTL, sbi->ll_dt_exp,
					   sizeof(*qctl), qctl, NULL);
			break;
		case QC_UUID:
			rc = obd_iocontrol(OBD_IOC_QUOTACTL, sbi->ll_md_exp,
					   sizeof(*qctl), qctl, NULL);
			if (rc == -EAGAIN)
				rc = obd_iocontrol(OBD_IOC_QUOTACTL,
						   sbi->ll_dt_exp,
						   sizeof(*qctl), qctl, NULL);
			break;
		default:
			rc = -EINVAL;
			break;
		}

		qctl->qc_cmd = cmd;
		if (rc)
			RETURN(rc);
	} else {
		struct obd_quotactl *oqctl;
		int oqctl_len = sizeof(*oqctl);

		if (LUSTRE_Q_CMD_IS_POOL(cmd))
			oqctl_len += LOV_MAXPOOLNAME + 1;

		OBD_ALLOC(oqctl, oqctl_len);
		if (oqctl == NULL)
			RETURN(-ENOMEM);

		QCTL_COPY(oqctl, qctl);
		rc = obd_quotactl(sbi->ll_md_exp, oqctl);
		if (rc) {
			OBD_FREE(oqctl, oqctl_len);
			RETURN(rc);
		}
		/* If QIF_SPACE is not set, client should collect the
		 * space usage from OSSs by itself
		 */
		if ((cmd == Q_GETQUOTA || cmd == LUSTRE_Q_GETQUOTAPOOL) &&
		    !(oqctl->qc_dqblk.dqb_valid & QIF_SPACE) &&
		    !oqctl->qc_dqblk.dqb_curspace) {
			struct obd_quotactl *oqctl_tmp;
			int qctl_len = sizeof(*oqctl_tmp) + LOV_MAXPOOLNAME + 1;

			OBD_ALLOC(oqctl_tmp, qctl_len);
			if (oqctl_tmp == NULL)
				GOTO(out, rc = -ENOMEM);

			if (cmd == LUSTRE_Q_GETQUOTAPOOL) {
				oqctl_tmp->qc_cmd = LUSTRE_Q_GETQUOTAPOOL;
				memcpy(oqctl_tmp->qc_poolname,
				       qctl->qc_poolname,
				       LOV_MAXPOOLNAME + 1);
			} else {
				oqctl_tmp->qc_cmd = Q_GETOQUOTA;
			}
			oqctl_tmp->qc_id = oqctl->qc_id;
			oqctl_tmp->qc_type = oqctl->qc_type;

			/* collect space usage from OSTs */
			oqctl_tmp->qc_dqblk.dqb_curspace = 0;
			rc = obd_quotactl(sbi->ll_dt_exp, oqctl_tmp);
			if (!rc || rc == -EREMOTEIO) {
				oqctl->qc_dqblk.dqb_curspace =
					oqctl_tmp->qc_dqblk.dqb_curspace;
				oqctl->qc_dqblk.dqb_valid |= QIF_SPACE;
			}

			/* collect space & inode usage from MDTs */
			oqctl_tmp->qc_cmd = Q_GETOQUOTA;
			oqctl_tmp->qc_dqblk.dqb_curspace = 0;
			oqctl_tmp->qc_dqblk.dqb_curinodes = 0;
			rc = obd_quotactl(sbi->ll_md_exp, oqctl_tmp);
			if (!rc || rc == -EREMOTEIO) {
				oqctl->qc_dqblk.dqb_curspace +=
					oqctl_tmp->qc_dqblk.dqb_curspace;
				oqctl->qc_dqblk.dqb_curinodes =
					oqctl_tmp->qc_dqblk.dqb_curinodes;
				oqctl->qc_dqblk.dqb_valid |= QIF_INODES;
			} else {
				oqctl->qc_dqblk.dqb_valid &= ~QIF_SPACE;
			}

			OBD_FREE(oqctl_tmp, qctl_len);
		}
out:
		QCTL_COPY(qctl, oqctl);
		OBD_FREE(oqctl, oqctl_len);
	}

	RETURN(rc);
}

static int ll_rmfid(struct file *file, void __user *arg)
{
	const struct fid_array __user *ufa = arg;
	struct inode *inode = file_inode(file);
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct fid_array *lfa = NULL, *lfa_new = NULL;
	int i, rc, *rcs = NULL;
	unsigned int nr;
	bool lfa_flag = false; /* lfa already free'ed */
	size_t size;

	ENTRY;
	if (!capable(CAP_DAC_READ_SEARCH) &&
	    !test_bit(LL_SBI_USER_FID2PATH, ll_i2sbi(inode)->ll_flags))
		RETURN(-EPERM);
	/* Only need to get the buflen */
	if (get_user(nr, &ufa->fa_nr))
		RETURN(-EFAULT);
	/* DoS protection */
	if (nr > OBD_MAX_FIDS_IN_ARRAY)
		RETURN(-E2BIG);

	size = offsetof(struct fid_array, fa_fids[nr]);
	OBD_ALLOC(lfa, size);
	if (!lfa)
		RETURN(-ENOMEM);
	OBD_ALLOC_PTR_ARRAY(rcs, nr);
	if (!rcs)
		GOTO(free_lfa, rc = -ENOMEM);

	if (copy_from_user(lfa, arg, size))
		GOTO(free_rcs, rc = -EFAULT);

	/* In case of subdirectory mount, we need to make sure all the files
	 * for which we want to remove FID are visible in the namespace.
	 */
	if (!fid_is_root(&sbi->ll_root_fid)) {
		int path_len = PATH_MAX, linkno;
		struct getinfo_fid2path *gf;
		int idx, last_idx = nr - 1;

		lfa_new = NULL;

		OBD_ALLOC(lfa_new, size);
		if (!lfa_new)
			GOTO(free_rcs, rc = -ENOMEM);
		lfa_new->fa_nr = 0;

		gf = kmalloc(sizeof(*gf) + path_len + 1, GFP_NOFS);
		if (!gf)
			GOTO(free_lfa_new, rc = -ENOMEM);

		for (idx = 0; idx < nr; idx++) {
			linkno = 0;
			while (1) {
				memset(gf, 0, sizeof(*gf) + path_len + 1);
				gf->gf_fid = lfa->fa_fids[idx];
				gf->gf_pathlen = path_len;
				gf->gf_linkno = linkno;
				rc = __ll_fid2path(inode, gf,
						   sizeof(*gf) + gf->gf_pathlen,
						   gf->gf_pathlen);
				if (rc == -ENAMETOOLONG) {
					struct getinfo_fid2path *tmpgf;

					path_len += PATH_MAX;
					tmpgf = krealloc(gf,
						     sizeof(*gf) + path_len + 1,
						     GFP_NOFS);
					if (!tmpgf) {
						kfree(gf);
						GOTO(free_lfa_new, rc = -ENOMEM);
					}
					gf = tmpgf;
					continue;
				}
				if (rc)
					break;
				if (gf->gf_linkno == linkno)
					break;
				linkno = gf->gf_linkno;
			}

			if (!rc) {
				/* All the links for this fid are visible in the
				 * mounted subdir. So add it to the list of fids
				 * to remove.
				 */
				lfa_new->fa_fids[lfa_new->fa_nr++] =
					lfa->fa_fids[idx];
			} else {
				/* At least one link for this fid is not visible
				 * in the mounted subdir. So add it at the end
				 * of the list that will be hidden to lower
				 * layers, and set -ENOENT as ret code.
				 */
				lfa_new->fa_fids[last_idx] = lfa->fa_fids[idx];
				rcs[last_idx--] = rc;
			}
		}
		kfree(gf);
		OBD_FREE(lfa, size);
		lfa_flag = true;
		lfa = lfa_new;
	}
	if (lfa->fa_nr == 0)
		GOTO(free_rcs, rc = rcs[nr - 1]);

	/* Call mdc_iocontrol */
	rc = md_rmfid(ll_i2mdexp(file_inode(file)), lfa, rcs, NULL);
	lfa->fa_nr = nr;
	if (!rc) {
		for (i = 0; i < nr; i++)
			if (rcs[i])
				lfa->fa_fids[i].f_ver = rcs[i];
		if (copy_to_user(arg, lfa, size))
			rc = -EFAULT;
	}

free_lfa_new:
	OBD_FREE(lfa_new, size);
free_rcs:
	OBD_FREE_PTR_ARRAY(rcs, nr);
free_lfa:
	if (!lfa_flag)
		OBD_FREE(lfa, size);

	RETURN(rc);
}

/* This function tries to get a single name component, to send to the server.
 * No actual path traversal involved, so we limit to NAME_MAX
 */
static char *ll_getname(const char __user *filename)
{
	int ret = 0, len;
	char *tmp;

	OBD_ALLOC(tmp, NAME_MAX + 1);

	if (!tmp)
		return ERR_PTR(-ENOMEM);

	len = strncpy_from_user(tmp, filename, NAME_MAX + 1);
	if (len < 0)
		ret = -ENOENT;
	else if (len > NAME_MAX)
		ret = -ENAMETOOLONG;

	if (ret) {
		OBD_FREE(tmp, NAME_MAX + 1);
		tmp =  ERR_PTR(ret);
	}
	return tmp;
}

static const char *const ladvise_names[] = LU_LADVISE_NAMES;

#define ll_putname(filename) OBD_FREE(filename, NAME_MAX + 1);

static long ll_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct dentry *dentry = file_dentry(file);
	struct inode *inode = file_inode(file);
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct obd_ioctl_data *data = NULL;
	void __user *uarg = (void __user *)arg;
	int rc = 0;

	ENTRY;
	CDEBUG(D_VFSTRACE|D_IOCTL, "VFS Op:inode="DFID"(%pK) cmd=%x arg=%lx\n",
	       PFID(ll_inode2fid(inode)), inode, cmd, arg);

	/* asm-ppc{,64} declares TCGETS, et. al. as type 't' not 'T' */
	if (_IOC_TYPE(cmd) == 'T' || _IOC_TYPE(cmd) == 't') /* tty ioctls */
		return -ENOTTY;

	ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_IOCTL, 1);
	switch (cmd) {
	case IOC_MDC_LOOKUP: {
		int namelen, len = 0;
		char *filename;

		rc = obd_ioctl_getdata(&data, &len, uarg);
		if (rc != 0)
			RETURN(rc);

		filename = data->ioc_inlbuf1;
		namelen = strlen(filename);
		if (namelen < 1) {
			CDEBUG(D_INFO, "IOC_MDC_LOOKUP missing filename\n");
			GOTO(out_free, rc = -EINVAL);
		}

		rc = ll_get_fid_by_name(inode, filename, namelen, NULL, NULL);
		if (rc < 0) {
			CERROR("%s: lookup "DNAME" failed: rc = %d\n",
			       sbi->ll_fsname,
			       encode_fn_dname(namelen, filename), rc);
			GOTO(out_free, rc);
		}
out_free:
		OBD_FREE_LARGE(data, len);
		return rc;
	}
	case LL_IOC_LMV_SETSTRIPE: {
		struct lmv_user_md  *lum;
		char *filename;
		int namelen = 0;
		int lumlen = 0;
		umode_t mode;
		bool createonly = false;
		int len;
		int rc;

		rc = obd_ioctl_getdata(&data, &len, uarg);
		if (rc)
			RETURN(rc);

		if (data->ioc_inlbuf1 == NULL || data->ioc_inlbuf2 == NULL ||
		    data->ioc_inllen1 == 0 || data->ioc_inllen2 == 0)
			GOTO(lmv_out_free, rc = -EINVAL);

		filename = data->ioc_inlbuf1;
		namelen = data->ioc_inllen1;

		if (namelen < 1) {
			CDEBUG(D_INFO, "IOC_MDC_LOOKUP missing filename\n");
			GOTO(lmv_out_free, rc = -EINVAL);
		}
		lum = (struct lmv_user_md *)data->ioc_inlbuf2;
		lumlen = data->ioc_inllen2;

		if (!lmv_user_magic_supported(lum->lum_magic)) {
			CERROR("%s: wrong lum magic %x : rc = %d\n",
			       encode_fn_len(filename, namelen),
			       lum->lum_magic, -EINVAL);
			GOTO(lmv_out_free, rc = -EINVAL);
		}

		if ((lum->lum_magic == LMV_USER_MAGIC ||
		     lum->lum_magic == LMV_USER_MAGIC_SPECIFIC) &&
		    lumlen < sizeof(*lum)) {
			CERROR("%s: wrong lum size %d for magic %x : rc = %d\n",
			       encode_fn_len(filename, namelen), lumlen,
			       lum->lum_magic, -EINVAL);
			GOTO(lmv_out_free, rc = -EINVAL);
		}

		if (lum->lum_magic == LMV_MAGIC_FOREIGN &&
		    lumlen < sizeof(struct lmv_foreign_md)) {
			CERROR("%s: wrong lum magic %x or size %d: rc = %d\n",
			       encode_fn_len(filename, namelen),
			       lum->lum_magic, lumlen, -EFAULT);
			GOTO(lmv_out_free, rc = -EINVAL);
		}

		mode = data->ioc_type;
		createonly = data->ioc_obdo1.o_flags & OBD_FL_OBDMDEXISTS;
		rc = ll_dir_setdirstripe(dentry, lum, lumlen, filename, mode,
					 createonly);
lmv_out_free:
		OBD_FREE_LARGE(data, len);
		RETURN(rc);

	}
	case LL_IOC_LMV_SET_DEFAULT_STRIPE: {
		struct lmv_user_md lum;
		struct lmv_user_md __user *ulump = uarg;
		int rc;

		if (copy_from_user(&lum, ulump, sizeof(lum)))
			RETURN(-EFAULT);

		if (lum.lum_magic != LMV_USER_MAGIC)
			RETURN(-EINVAL);

		rc = ll_dir_setstripe(inode, (struct lov_user_md *)&lum, 0);

		RETURN(rc);
	}
	case LL_IOC_LOV_SETSTRIPE_NEW:
	case LL_IOC_LOV_SETSTRIPE: {
		struct lov_user_md_v3 *lumv3 = NULL;
		struct lov_user_md_v1 lumv1;
		struct lov_user_md_v1 *lumv1_ptr = &lumv1;
		struct lov_user_md_v1 __user *lumv1p = uarg;
		struct lov_user_md_v3 __user *lumv3p = uarg;
		int lum_size = 0;
		int set_default = 0;

		BUILD_BUG_ON(sizeof(struct lov_user_md_v3) <=
			     sizeof(struct lov_comp_md_v1));
		BUILD_BUG_ON(sizeof(*lumv3) != sizeof(*lumv3p));
		/* first try with v1 which is smaller than v3 */
		if (copy_from_user(&lumv1, lumv1p, sizeof(lumv1)))
			RETURN(-EFAULT);

		if (is_root_inode(inode))
			set_default = 1;

		switch (lumv1.lmm_magic) {
		case LOV_USER_MAGIC_V3:
		case LOV_USER_MAGIC_SPECIFIC:
			lum_size = ll_lov_user_md_size(&lumv1);
			if (lum_size < 0)
				RETURN(lum_size);
			OBD_ALLOC(lumv3, lum_size);
			if (!lumv3)
				RETURN(-ENOMEM);
			if (copy_from_user(lumv3, lumv3p, lum_size))
				GOTO(out, rc = -EFAULT);
			lumv1_ptr = (struct lov_user_md_v1 *)lumv3;
			break;
		case LOV_USER_MAGIC_V1:
			break;
		default:
			GOTO(out, rc = -EOPNOTSUPP);
		}

		/* in v1 and v3 cases lumv1 points to data */
		rc = ll_dir_setstripe(inode, lumv1_ptr, set_default);
out:
		OBD_FREE(lumv3, lum_size);
		RETURN(rc);
	}
	case LL_IOC_LMV_GETSTRIPE: {
		struct lmv_user_md __user *ulmv = uarg;
		struct lmv_user_md lum;
		struct ptlrpc_request *request = NULL;
		union lmv_mds_md *lmm = NULL;
		int lmmsize;
		u64 valid = 0;
		struct lmv_user_md *tmp = NULL;
		int mdt_index;
		int lum_size;
		int stripe_count;
		int max_stripe_count;
		int i;
		int rc;

		if (copy_from_user(&lum, ulmv, sizeof(*ulmv)))
			RETURN(-EFAULT);

		/* get default LMV */
		if (lum.lum_magic == LMV_USER_MAGIC &&
		    lum.lum_type != LMV_TYPE_RAW) {
			rc = ll_dir_get_default_lmv(inode, &lum);
			if (rc)
				RETURN(rc);

			if (copy_to_user(ulmv, &lum, sizeof(lum)))
				RETURN(-EFAULT);

			RETURN(0);
		}

		max_stripe_count = lum.lum_stripe_count;
		/* lum_magic will indicate which stripe the ioctl will like
		 * to get, LMV_MAGIC_V1 is for normal LMV stripe, LMV_USER_MAGIC
		 * is for default LMV stripe
		 */
		if (lum.lum_magic == LMV_MAGIC_V1)
			valid |= OBD_MD_MEA;
		else if (lum.lum_magic == LMV_USER_MAGIC)
			valid |= OBD_MD_DEFAULT_MEA;
		else
			RETURN(-EINVAL);

		rc = ll_dir_getstripe_default(inode, (void **)&lmm, &lmmsize,
					      &request, NULL, valid);
		if (rc != 0)
			GOTO(finish_req, rc);

		/* get default LMV in raw mode */
		if (lum.lum_magic == LMV_USER_MAGIC) {
			if (copy_to_user(ulmv, lmm, lmmsize))
				GOTO(finish_req, rc = -EFAULT);
			GOTO(finish_req, rc);
		}

		/* if foreign LMV case, fake stripes number */
		if (lmm->lmv_magic == LMV_MAGIC_FOREIGN) {
			struct lmv_foreign_md *lfm;

			lfm = (struct lmv_foreign_md *)lmm;
			if (lfm->lfm_length < XATTR_SIZE_MAX -
			    offsetof(typeof(*lfm), lfm_value)) {
				__u32 size = lfm->lfm_length +
					     offsetof(typeof(*lfm), lfm_value);

				stripe_count = lmv_foreign_to_md_stripes(size);
			} else {
				CERROR("%s: invalid %d foreign size returned: rc = %d\n",
				       sbi->ll_fsname, lfm->lfm_length,
				       -EINVAL);
				return -EINVAL;
			}
		} else {
			stripe_count = lmv_mds_md_stripe_count_get(lmm);
		}
		if (max_stripe_count < stripe_count) {
			lum.lum_stripe_count = stripe_count;
			if (copy_to_user(ulmv, &lum, sizeof(lum)))
				GOTO(finish_req, rc = -EFAULT);
			GOTO(finish_req, rc = -E2BIG);
		}

		/* enough room on user side and foreign case */
		if (lmm->lmv_magic == LMV_MAGIC_FOREIGN) {
			struct lmv_foreign_md *lfm;
			__u32 size;

			lfm = (struct lmv_foreign_md *)lmm;
			size = lfm->lfm_length +
			       offsetof(struct lmv_foreign_md, lfm_value);
			if (copy_to_user(ulmv, lfm, size))
				GOTO(finish_req, rc = -EFAULT);
			GOTO(finish_req, rc);
		}

		lum_size = lmv_user_md_size(stripe_count,
					    LMV_USER_MAGIC_SPECIFIC);
		OBD_ALLOC(tmp, lum_size);
		if (tmp == NULL)
			GOTO(finish_req, rc = -ENOMEM);

		mdt_index = ll_get_mdt_idx(inode);
		if (mdt_index < 0)
			GOTO(out_tmp, rc = -ENOMEM);

		tmp->lum_magic = LMV_MAGIC_V1;
		tmp->lum_stripe_count = 0;
		tmp->lum_stripe_offset = mdt_index;
		tmp->lum_hash_type = lmv_mds_md_hash_type_get(lmm);
		for (i = 0; i < stripe_count; i++) {
			struct lu_fid	fid;

			fid_le_to_cpu(&fid, &lmm->lmv_md_v1.lmv_stripe_fids[i]);
			if (fid_is_sane(&fid)) {
				mdt_index = ll_get_mdt_idx_by_fid(sbi, &fid);
				if (mdt_index < 0)
					GOTO(out_tmp, rc = mdt_index);

				tmp->lum_objects[i].lum_mds = mdt_index;
				tmp->lum_objects[i].lum_fid = fid;
			}

			tmp->lum_stripe_count++;
		}

		if (copy_to_user(ulmv, tmp, lum_size))
			GOTO(out_tmp, rc = -EFAULT);
out_tmp:
		OBD_FREE(tmp, lum_size);
finish_req:
		ptlrpc_req_put(request);
		return rc;
	}
	case LL_IOC_REMOVE_ENTRY: {
		char *filename = NULL;
		int namelen = 0;
		int rc;

		/* Here is a little hack to avoid sending REINT_RMENTRY to
		 * unsupported server, which might crash the server(LU-2730),
		 * Because both LVB_TYPE and REINT_RMENTRY will be supported
		 * on 2.4, we use OBD_CONNECT_LVB_TYPE to detect whether the
		 * server will support REINT_RMENTRY XXX
		 */
		if (!(exp_connect_flags(sbi->ll_md_exp) & OBD_CONNECT_LVB_TYPE))
			RETURN(-EOPNOTSUPP);

		filename = ll_getname(uarg);
		if (IS_ERR(filename))
			RETURN(PTR_ERR(filename));

		namelen = strlen(filename);
		if (namelen < 1)
			GOTO(out_rmdir, rc = -EINVAL);

		rc = ll_rmdir_entry(inode, filename, namelen);
out_rmdir:
		if (filename)
			ll_putname(filename);
		RETURN(rc);
	}
	case LL_IOC_RMFID:
		RETURN(ll_rmfid(file, uarg));
	case LL_IOC_LOV_SWAP_LAYOUTS:
		RETURN(-EPERM);
	case LL_IOC_LOV_GETSTRIPE:
	case LL_IOC_LOV_GETSTRIPE_NEW:
	case LL_IOC_MDC_GETINFO_V1:
	case LL_IOC_MDC_GETINFO_V2:
	case IOC_MDC_GETFILEINFO_V1:
	case IOC_MDC_GETFILEINFO_V2:
	case IOC_MDC_GETFILESTRIPE: {
		struct ptlrpc_request *request = NULL;
		struct ptlrpc_request *root_request = NULL;
		struct lov_user_md __user *lump;
		struct lov_mds_md *lmm = NULL;
		struct mdt_body *body;
		char *filename = NULL;
		lstat_t __user *statp = NULL;
		lstatx_t __user *stxp = NULL;
		__u64 __user *flagsp = NULL;
		__u32 __user *lmmsizep = NULL;
		struct lu_fid __user *fidp = NULL;
		int lmmsize;
		bool api32;

		if (cmd == IOC_MDC_GETFILEINFO_V1 ||
		    cmd == IOC_MDC_GETFILEINFO_V2 ||
		    cmd == IOC_MDC_GETFILESTRIPE) {
			filename = ll_getname(uarg);
			if (IS_ERR(filename))
				RETURN(PTR_ERR(filename));

			rc = ll_lov_getstripe_ea_info(inode, filename, &lmm,
						      &lmmsize, &request);
		} else {
			rc = ll_dir_getstripe_default(inode, (void **)&lmm,
						      &lmmsize, &request,
						      &root_request, 0);
		}

		if (request) {
			body = req_capsule_server_get(&request->rq_pill,
						      &RMF_MDT_BODY);
			LASSERT(body != NULL);
		} else {
			GOTO(out_req, rc);
		}

		if (rc == -ENODATA && (cmd == IOC_MDC_GETFILEINFO_V1 ||
				       cmd == LL_IOC_MDC_GETINFO_V1 ||
				       cmd == IOC_MDC_GETFILEINFO_V2 ||
				       cmd == LL_IOC_MDC_GETINFO_V2)) {
			lmmsize = 0;
			rc = 0;
		}

		if (rc < 0)
			GOTO(out_req, rc);

		if (cmd == IOC_MDC_GETFILESTRIPE ||
		    cmd == LL_IOC_LOV_GETSTRIPE ||
		    cmd == LL_IOC_LOV_GETSTRIPE_NEW) {
			lump = uarg;
		} else if (cmd == IOC_MDC_GETFILEINFO_V1 ||
			   cmd == LL_IOC_MDC_GETINFO_V1){
			struct lov_user_mds_data_v1 __user *lmdp;

			lmdp = uarg;
			statp = &lmdp->lmd_st;
			lump = &lmdp->lmd_lmm;
		} else {
			struct lov_user_mds_data __user *lmdp;

			lmdp = uarg;
			fidp = &lmdp->lmd_fid;
			stxp = &lmdp->lmd_stx;
			flagsp = &lmdp->lmd_flags;
			lmmsizep = &lmdp->lmd_lmmsize;
			lump = &lmdp->lmd_lmm;
		}

		if (lmmsize == 0) {
			/* If the file has no striping then zero out *lump so
			 * that the caller isn't confused by garbage.
			 */
			if (clear_user(lump, sizeof(*lump)))
				GOTO(out_req, rc = -EFAULT);
		} else if (copy_to_user(lump, lmm, lmmsize)) {
			if (copy_to_user(lump, lmm, sizeof(*lump)))
				GOTO(out_req, rc = -EFAULT);
			rc = -EOVERFLOW;
		}
		api32 = test_bit(LL_SBI_32BIT_API, sbi->ll_flags);

		if (cmd == IOC_MDC_GETFILEINFO_V1 ||
		    cmd == LL_IOC_MDC_GETINFO_V1) {
			lstat_t st = { 0 };

			st.st_dev	= inode->i_sb->s_dev;
			st.st_mode	= body->mbo_mode;
			st.st_nlink	= body->mbo_nlink;
			st.st_uid	= body->mbo_uid;
			st.st_gid	= body->mbo_gid;
			st.st_rdev	= body->mbo_rdev;
			if (IS_ENCRYPTED(inode) &&
			    !ll_has_encryption_key(inode))
				st.st_size = round_up(st.st_size,
						   LUSTRE_ENCRYPTION_UNIT_SIZE);
			else
				st.st_size = body->mbo_size;
			st.st_blksize	= PAGE_SIZE;
			st.st_blocks	= body->mbo_blocks;
			st.st_atime	= body->mbo_atime;
			st.st_mtime	= body->mbo_mtime;
			st.st_ctime	= body->mbo_ctime;
			st.st_ino	= cl_fid_build_ino(&body->mbo_fid1,
							   api32);

			if (copy_to_user(statp, &st, sizeof(st)))
				GOTO(out_req, rc = -EFAULT);
		} else if (cmd == IOC_MDC_GETFILEINFO_V2 ||
			   cmd == LL_IOC_MDC_GETINFO_V2) {
			lstatx_t stx = { 0 };
			__u64 valid = body->mbo_valid;

			stx.stx_blksize = PAGE_SIZE;
			stx.stx_nlink = body->mbo_nlink;
			stx.stx_uid = body->mbo_uid;
			stx.stx_gid = body->mbo_gid;
			stx.stx_mode = body->mbo_mode;
			stx.stx_ino = cl_fid_build_ino(&body->mbo_fid1,
						       api32);
			if (IS_ENCRYPTED(inode) &&
			    !ll_has_encryption_key(inode))
				stx.stx_size = round_up(stx.stx_size,
						   LUSTRE_ENCRYPTION_UNIT_SIZE);
			else
				stx.stx_size = body->mbo_size;
			stx.stx_blocks = body->mbo_blocks;
			stx.stx_atime.tv_sec = body->mbo_atime;
			stx.stx_ctime.tv_sec = body->mbo_ctime;
			stx.stx_mtime.tv_sec = body->mbo_mtime;
			stx.stx_btime.tv_sec = body->mbo_btime;
			stx.stx_rdev_major = MAJOR(body->mbo_rdev);
			stx.stx_rdev_minor = MINOR(body->mbo_rdev);
			stx.stx_dev_major = MAJOR(inode->i_sb->s_dev);
			stx.stx_dev_minor = MINOR(inode->i_sb->s_dev);
			stx.stx_mask |= STATX_BASIC_STATS | STATX_BTIME;

			stx.stx_attributes_mask = STATX_ATTR_IMMUTABLE |
						  STATX_ATTR_APPEND;
#ifdef HAVE_LUSTRE_CRYPTO
			stx.stx_attributes_mask |= STATX_ATTR_ENCRYPTED;
#endif
			if (body->mbo_valid & OBD_MD_FLFLAGS) {
				stx.stx_attributes |= body->mbo_flags;
				/* if Lustre specific LUSTRE_ENCRYPT_FL flag is
				 * set, also set ext4 equivalent to please statx
				 */
				if (body->mbo_flags & LUSTRE_ENCRYPT_FL)
					stx.stx_attributes |=
						STATX_ATTR_ENCRYPTED;
			}

			/* For a striped directory, the size and blocks returned
			 * from MDT is not correct.
			 * The size and blocks are aggregated by client across
			 * all stripes.
			 * Thus for a striped directory, do not return the valid
			 * FLSIZE and FLBLOCKS flags to the caller.
			 * However, this whould be better decided by the MDS
			 * instead of the client.
			 */
			if (cmd == LL_IOC_MDC_GETINFO_V2 &&
			    ll_dir_striped(inode))
				valid &= ~(OBD_MD_FLSIZE | OBD_MD_FLBLOCKS);

			if (flagsp && copy_to_user(flagsp, &valid,
						   sizeof(*flagsp)))
				GOTO(out_req, rc = -EFAULT);

			if (fidp && copy_to_user(fidp, &body->mbo_fid1,
						 sizeof(*fidp)))
				GOTO(out_req, rc = -EFAULT);

			if (!(valid & OBD_MD_FLSIZE))
				stx.stx_mask &= ~STATX_SIZE;
			if (!(valid & OBD_MD_FLBLOCKS))
				stx.stx_mask &= ~STATX_BLOCKS;

			if (stxp && copy_to_user(stxp, &stx, sizeof(stx)))
				GOTO(out_req, rc = -EFAULT);

			if (lmmsizep && copy_to_user(lmmsizep, &lmmsize,
						     sizeof(*lmmsizep)))
				GOTO(out_req, rc = -EFAULT);
		}

		EXIT;
out_req:
		ptlrpc_req_put(request);
		ptlrpc_req_put(root_request);
		if (filename)
			ll_putname(filename);
		return rc;
	}
	case OBD_IOC_QUOTACTL: {
		struct if_quotactl *qctl;
		int qctl_len = sizeof(*qctl) + LOV_MAXPOOLNAME + 1;

		OBD_ALLOC(qctl, qctl_len);
		if (!qctl)
			RETURN(-ENOMEM);

		if (copy_from_user(qctl, uarg, sizeof(*qctl)))
			GOTO(out_quotactl, rc = -EFAULT);

		if (LUSTRE_Q_CMD_IS_POOL(qctl->qc_cmd)) {
			char __user *from = uarg +
					offsetof(typeof(*qctl), qc_poolname);
			if (copy_from_user(qctl->qc_poolname, from,
					   LOV_MAXPOOLNAME + 1))
				GOTO(out_quotactl, rc = -EFAULT);
		}

		rc = quotactl_ioctl(inode->i_sb, qctl);
		if ((rc == 0 || rc == -ENODATA) &&
		    copy_to_user(uarg, qctl, sizeof(*qctl)))
			rc = -EFAULT;
out_quotactl:
		OBD_FREE(qctl, qctl_len);
		RETURN(rc);
	}
	case LL_IOC_GETOBDCOUNT: {
		u32 count, vallen;
		struct obd_export *exp;

		if (copy_from_user(&count, uarg, sizeof(count)))
			RETURN(-EFAULT);

		/* get ost count when count is zero, get mdt count otherwise */
		exp = count ? sbi->ll_md_exp : sbi->ll_dt_exp;
		vallen = sizeof(count);
		rc = obd_get_info(NULL, exp, sizeof(KEY_TGT_COUNT),
				  KEY_TGT_COUNT, &vallen, &count);
		if (rc) {
			CERROR("%s: get target count failed: rc = %d\n",
			       sbi->ll_fsname, rc);
			RETURN(rc);
		}

		if (copy_to_user(uarg, &count, sizeof(count)))
			RETURN(-EFAULT);

		RETURN(0);
	}
	case LL_IOC_GET_CONNECT_FLAGS:
		RETURN(obd_iocontrol(cmd, sbi->ll_md_exp, 0, NULL, uarg));
	case LL_IOC_FID2MDTIDX: {
		struct obd_export *exp = ll_i2mdexp(inode);
		struct lu_fid fid;
		__u32 index;

		if (copy_from_user(&fid, uarg, sizeof(fid)))
			RETURN(-EFAULT);

		/* Call mdc_iocontrol */
		rc = obd_iocontrol(LL_IOC_FID2MDTIDX, exp, sizeof(fid), &fid,
				   (__u32 __user *)&index);
		if (rc != 0)
			RETURN(rc);

		RETURN(index);
	}
	case LL_IOC_HSM_REQUEST: {
		struct hsm_user_request *hur;
		ssize_t totalsize;

		OBD_ALLOC_PTR(hur);
		if (hur == NULL)
			RETURN(-ENOMEM);

		/* We don't know the true size yet; copy the fixed-size part */
		if (copy_from_user(hur, uarg, sizeof(*hur))) {
			OBD_FREE_PTR(hur);
			RETURN(-EFAULT);
		}

		/* Compute the whole struct size */
		totalsize = hur_len(hur);
		OBD_FREE_PTR(hur);
		if (totalsize < 0)
			RETURN(-E2BIG);

		/* Final size will be more than double totalsize */
		if (totalsize >= MDS_MAXREQSIZE / 3)
			RETURN(-E2BIG);

		OBD_ALLOC_LARGE(hur, totalsize);
		if (hur == NULL)
			RETURN(-ENOMEM);

		/* Copy the whole struct */
		if (copy_from_user(hur, uarg, totalsize))
			GOTO(out_hur, rc = -EFAULT);

		if (hur->hur_request.hr_action == HUA_RELEASE) {
			const struct lu_fid *fid;
			struct inode *f;
			int i;

			for (i = 0; i < hur->hur_request.hr_itemcount; i++) {
				fid = &hur->hur_user_item[i].hui_fid;
				f = search_inode_for_lustre(inode->i_sb, fid);
				if (IS_ERR(f)) {
					rc = PTR_ERR(f);
					break;
				}

				rc = ll_hsm_release(f);
				iput(f);
				if (rc != 0)
					break;
			}
		} else {
			rc = obd_iocontrol(cmd, ll_i2mdexp(inode), totalsize,
					   hur, NULL);
		}
out_hur:
		OBD_FREE_LARGE(hur, totalsize);

		RETURN(rc);
	}
	case LL_IOC_HSM_PROGRESS: {
		struct hsm_progress_kernel hpk;
		struct hsm_progress hp;

		if (copy_from_user(&hp, uarg, sizeof(hp)))
			RETURN(-EFAULT);

		hpk.hpk_fid = hp.hp_fid;
		hpk.hpk_cookie = hp.hp_cookie;
		hpk.hpk_extent = hp.hp_extent;
		hpk.hpk_flags = hp.hp_flags;
		hpk.hpk_errval = hp.hp_errval;
		hpk.hpk_data_version = 0;

		/* File may not exist in Lustre; all progress
		 * reported to Lustre root
		 */
		rc = obd_iocontrol(cmd, sbi->ll_md_exp, sizeof(hpk), &hpk,
				   NULL);
		RETURN(rc);
	}
	case LL_IOC_HSM_CT_START:
		if (!capable(CAP_SYS_ADMIN))
			RETURN(-EPERM);

		rc = copy_and_ct_start(cmd, sbi->ll_md_exp, uarg);
		RETURN(rc);

	case LL_IOC_HSM_COPY_START: {
		struct hsm_copy *copy;
		int rc;

		OBD_ALLOC_PTR(copy);
		if (copy == NULL)
			RETURN(-ENOMEM);
		if (copy_from_user(copy, uarg, sizeof(*copy))) {
			OBD_FREE_PTR(copy);
			RETURN(-EFAULT);
		}

		rc = ll_ioc_copy_start(inode->i_sb, copy);
		if (copy_to_user(uarg, copy, sizeof(*copy)))
			rc = -EFAULT;

		OBD_FREE_PTR(copy);
		RETURN(rc);
	}
	case LL_IOC_HSM_COPY_END: {
		struct hsm_copy *copy;
		int rc;

		OBD_ALLOC_PTR(copy);
		if (copy == NULL)
			RETURN(-ENOMEM);
		if (copy_from_user(copy, uarg, sizeof(*copy))) {
			OBD_FREE_PTR(copy);
			RETURN(-EFAULT);
		}

		rc = ll_ioc_copy_end(inode->i_sb, copy);
		if (copy_to_user(uarg, copy, sizeof(*copy)))
			rc = -EFAULT;

		OBD_FREE_PTR(copy);
		RETURN(rc);
	}
	case LL_IOC_MIGRATE: {
		struct lmv_user_md *lum;
		int len;
		char *filename;
		int namelen = 0;
		__u32 flags;
		int rc;

		rc = obd_ioctl_getdata(&data, &len, uarg);
		if (rc)
			RETURN(rc);

		if (data->ioc_inlbuf1 == NULL || data->ioc_inlbuf2 == NULL ||
		    data->ioc_inllen1 == 0 || data->ioc_inllen2 == 0)
			GOTO(migrate_free, rc = -EINVAL);

		filename = data->ioc_inlbuf1;
		namelen = data->ioc_inllen1;
		flags = data->ioc_type;

		if (namelen < 1 || namelen != strlen(filename) + 1) {
			CDEBUG(D_INFO, "IOC_MDC_LOOKUP missing filename\n");
			GOTO(migrate_free, rc = -EINVAL);
		}

		lum = (struct lmv_user_md *)data->ioc_inlbuf2;
		if (lum->lum_magic != LMV_USER_MAGIC &&
		    lum->lum_magic != LMV_USER_MAGIC_SPECIFIC) {
			rc = -EINVAL;
			CERROR("%s: wrong lum magic %x: rc = %d\n",
			       encode_fn_len(filename, namelen),
			       lum->lum_magic, rc);
			GOTO(migrate_free, rc);
		}

		rc = ll_migrate(inode, file, lum, filename, flags);
migrate_free:
		OBD_FREE_LARGE(data, len);

		RETURN(rc);
	}
	case LL_IOC_LADVISE2: {
		struct llapi_lu_ladvise2 *ladvise;

		OBD_ALLOC_PTR(ladvise);
		if (ladvise == NULL)
			RETURN(-ENOMEM);

		if (copy_from_user(ladvise, uarg, sizeof(*ladvise)))
			GOTO(out_ladvise, rc = -EFAULT);

		switch (ladvise->lla_advice) {
		case LU_LADVISE_AHEAD:
			if (ladvise->lla_start >= ladvise->lla_end) {
				CDEBUG(D_VFSTRACE,
				       "%s: Invalid range (%llu %llu) for %s\n",
				       sbi->ll_fsname, ladvise->lla_start,
				       ladvise->lla_end,
				       ladvise_names[ladvise->lla_advice]);
				GOTO(out_ladvise, rc = -EINVAL);
			}

			/*
			 * Currently we only support name indexing format
			 * ahead operations.
			 */
			if (ladvise->lla_ahead_mode != LU_AH_NAME_INDEX) {
				CDEBUG(D_VFSTRACE,
				       "%s: Invalid access mode (%d) for %s\n",
				       sbi->ll_fsname, ladvise->lla_ahead_mode,
				       ladvise_names[ladvise->lla_advice]);
				GOTO(out_ladvise, rc = -EINVAL);
			}

			/* Currently we only support stat-ahead operations. */
			if (!(ladvise->lla_access_flags & ACCESS_FL_STAT)) {
				CDEBUG(D_VFSTRACE,
				       "%s: Invalid access flags (%x) for %s\n",
				       sbi->ll_fsname,
				       ladvise->lla_access_flags,
				       ladvise_names[ladvise->lla_advice]);
				GOTO(out_ladvise, rc = -EINVAL);
			}

			rc = ll_ioctl_ahead(file, ladvise);
			break;
		default:
			rc = -EINVAL;
		}
out_ladvise:
		OBD_FREE_PTR(ladvise);
		RETURN(rc);
	}
	case LL_IOC_PCC_STATE: {
		struct lu_pcc_state __user *ustate =
			(struct lu_pcc_state __user *)arg;
		struct lu_pcc_state *state;
		struct dentry *parent = file_dentry(file);
		struct dentry *dchild = NULL;
		struct inode *child_inode = NULL;
		struct qstr qstr;
		int namelen = 0;
		char *name;

		OBD_ALLOC_PTR(state);
		if (state == NULL)
			RETURN(-ENOMEM);

		if (copy_from_user(state, ustate, sizeof(*state)))
			GOTO(out_state_free, rc = -EFAULT);

		name = state->pccs_path;
		namelen = strlen(name);
		if (namelen < 0 || state->pccs_namelen != namelen + 1) {
			CDEBUG(D_INFO, "IOC_PCC_STATE missing filename\n");
			GOTO(out_state_free, rc = -EINVAL);
		}

		/* Get Child from dcache first. */
		qstr.hash = ll_full_name_hash(parent, name, namelen);
		qstr.name = name;
		qstr.len = namelen;
		dchild = d_lookup(parent, &qstr);
		if (dchild) {
			if (dchild->d_inode)
				child_inode = igrab(dchild->d_inode);
			dput(dchild);
		}

		if (!child_inode) {
			unsigned long ino;
			struct lu_fid fid;

			rc = ll_get_fid_by_name(parent->d_inode, name, namelen,
						&fid, NULL);
			if (rc)
				GOTO(out_state_free, rc);

			ino = cl_fid_build_ino(&fid, ll_need_32bit_api(sbi));
			child_inode = ilookup5(inode->i_sb, ino,
					       ll_test_inode_by_fid, &fid);
		}

		if (!child_inode) {
			/*
			 * Target inode is not in inode cache, the
			 * corresponding PCC file may be already released,
			 * return immediately.
			 */
			state->pccs_type = LU_PCC_NONE;
			GOTO(out_copy_to, rc = 0);
		}

		if (!S_ISREG(child_inode->i_mode))
			GOTO(out_child_iput, rc = -EINVAL);

		rc = pcc_ioctl_state(NULL, child_inode, state);
		if (rc)
			GOTO(out_child_iput, rc);
out_copy_to:
		if (copy_to_user(ustate, state, sizeof(*state)))
			GOTO(out_child_iput, rc = -EFAULT);
out_child_iput:
		iput(child_inode);
out_state_free:
		OBD_FREE_PTR(state);
		RETURN(rc);
	}
	case LL_IOC_PCC_DETACH_BY_FID: {
		struct lu_pcc_detach_fid *detach;
		struct lu_fid *fid;
		struct inode *inode2;
		unsigned long ino;

		OBD_ALLOC_PTR(detach);
		if (detach == NULL)
			RETURN(-ENOMEM);

		if (copy_from_user(detach, uarg, sizeof(*detach)))
			GOTO(out_detach, rc = -EFAULT);

		fid = &detach->pccd_fid;
		ino = cl_fid_build_ino(fid, ll_need_32bit_api(sbi));
		inode2 = ilookup5(inode->i_sb, ino, ll_test_inode_by_fid, fid);
		if (inode2 == NULL)
			/* Target inode is not in inode cache, and PCC file
			 * has aleady released, return immdiately.
			 */
			GOTO(out_detach, rc = 0);

		if (!S_ISREG(inode2->i_mode))
			GOTO(out_iput, rc = -EINVAL);

		if (!pcc_inode_permission(inode2))
			GOTO(out_iput, rc = -EPERM);

		rc = pcc_ioctl_detach(inode2, &detach->pccd_flags);
		if (rc)
			GOTO(out_iput, rc);

		if (copy_to_user((char __user *)arg, detach, sizeof(*detach)))
			GOTO(out_iput, rc = -EFAULT);
out_iput:
		iput(inode2);
out_detach:
		OBD_FREE_PTR(detach);
		RETURN(rc);
	}
	default:
		rc = ll_iocontrol(inode, file, cmd, uarg);
		if (rc != -ENOTTY)
			RETURN(rc);
		RETURN(obd_iocontrol(cmd, sbi->ll_dt_exp, 0, NULL, uarg));
	}
}

static loff_t ll_dir_seek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_mapping->host;
	struct ll_file_data *lfd = file->private_data;
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	int api32 = ll_need_32bit_api(sbi);
	loff_t ret = -EINVAL;

	ENTRY;
	ll_inode_lock(inode);
	switch (origin) {
	case SEEK_SET:
		break;
	case SEEK_CUR:
		offset += file->f_pos;
		break;
	case SEEK_END:
		if (offset > 0)
			GOTO(out, ret);
		if (api32)
			offset += LL_DIR_END_OFF_32BIT;
		else
			offset += LL_DIR_END_OFF;
		break;
	default:
		GOTO(out, ret);
	}

	if (offset >= 0 &&
	    ((api32 && offset <= LL_DIR_END_OFF_32BIT) ||
	     (!api32 && offset <= LL_DIR_END_OFF))) {
		if (offset != file->f_pos) {
			bool hash64;

			hash64 = test_bit(LL_SBI_64BIT_HASH, sbi->ll_flags);
			if ((api32 && offset == LL_DIR_END_OFF_32BIT) ||
			    (!api32 && offset == LL_DIR_END_OFF))
				lfd->lfd_pos = MDS_DIR_END_OFF;
			else if (api32 && hash64)
				lfd->lfd_pos = offset << 32;
			else
				lfd->lfd_pos = offset;
			file->f_pos = offset;
#ifdef HAVE_STRUCT_FILE_F_VERSION
			file->f_version = 0;
#endif
		}
		ret = offset;
	}
	GOTO(out, ret);

out:
	ll_inode_unlock(inode);
	return ret;
}

static int ll_dir_open(struct inode *inode, struct file *file)
{
	ENTRY;
	RETURN(ll_file_open(inode, file));
}

static int ll_dir_release(struct inode *inode, struct file *file)
{
	ENTRY;
	RETURN(ll_file_release(inode, file));
}

/* notify error if partially read striped directory */
static int ll_dir_flush(struct file *file, fl_owner_t id)
{
	struct ll_file_data *lfd = file->private_data;
	int rc = lfd->fd_partial_readdir_rc;

	lfd->fd_partial_readdir_rc = 0;

	return rc;
}

const struct file_operations ll_dir_operations = {
	.llseek		= ll_dir_seek,
	.open		= ll_dir_open,
	.release	= ll_dir_release,
	.read		= generic_read_dir,
#ifdef HAVE_DIR_CONTEXT
	.iterate_shared	= ll_iterate,
#else
	.readdir	= ll_readdir,
#endif
	.unlocked_ioctl	= ll_dir_ioctl,
	.fsync		= ll_fsync,
	.flush		= ll_dir_flush,
};
