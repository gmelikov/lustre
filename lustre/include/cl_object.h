/* SPDX-License-Identifier: GPL-2.0 */

/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#ifndef _LUSTRE_CL_OBJECT_H
#define _LUSTRE_CL_OBJECT_H

/** \defgroup clio clio
 *
 * Client objects implement io operations and cache pages.
 *
 * Examples: lov and osc are implementations of cl interface.
 *
 * Big Theory Statement.
 *
 * Layered objects.
 *
 * Client implementation is based on the following data-types:
 *
 *   - cl_object
 *
 *   - cl_page
 *
 *   - cl_lock     represents an extent lock on an object.
 *
 *   - cl_io       represents high-level i/o activity such as whole read/write
 *                 system call, or write-out of pages from under the lock being
 *                 canceled. cl_io has sub-ios that can be stopped and resumed
 *                 independently, thus achieving high degree of transfer
 *                 parallelism. Single cl_io can be advanced forward by
 *                 the multiple threads (although in the most usual case of
 *                 read/write system call it is associated with the single user
 *                 thread, that issued the system call).
 *
 * Terminology
 *
 *     - to avoid confusion high-level I/O operation like read or write system
 *     call is referred to as "an io", whereas low-level I/O operation, like
 *     RPC, is referred to as "a transfer"
 *
 *     - "generic code" means generic (not file system specific) code in the
 *     hosting environment. "cl-code" means code (mostly in cl_*.c files) that
 *     is not layer specific.
 *
 * Locking.
 *
 *  - i_mutex
 *      - PG_locked
 *          - cl_object_header::coh_page_guard
 *          - lu_site::ls_guard
 *
 * See the top comment in cl_object.c for the description of overall locking and
 * reference-counting design.
 *
 * See comments below for the description of i/o, page, and dlm-locking
 * design.
 *
 */

/*
 * super-class definitions.
 */
#include <linux/aio.h>
#include <linux/fs.h>

#include <libcfs/libcfs.h>
#include <lu_object.h>
#include <linux/atomic.h>
#include <linux/mutex.h>
#include <linux/radix-tree.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/pagevec.h>
#include <libcfs/linux/linux-misc.h>
#include <lustre_dlm.h>
#include <lustre_compat.h>

struct obd_info;
struct inode;

struct cl_device;

struct cl_object;

struct cl_page;
struct cl_page_slice;
struct cl_lock;
struct cl_lock_slice;

struct cl_lock_operations;
struct cl_page_operations;

struct cl_io;
struct cl_io_slice;

struct cl_req_attr;

/**
 * Device in the client stack.
 *
 * \see vvp_device, lov_device, lovsub_device, osc_device
 */
struct cl_device {
	/** Super-class. */
	struct lu_device                   cd_lu_dev;
};

/* cl_object */
/**
 * "Data attributes" of cl_object. Data attributes can be updated
 * independently for a sub-object, and top-object's attributes are calculated
 * from sub-objects' ones.
 */
struct cl_attr {
	/** Object size, in bytes */
	loff_t cat_size;

	unsigned int cat_kms_valid:1;
	/**
	 * Known minimal size, in bytes.
	 *
	 * This is only valid when at least one DLM lock is held.
	 */
	loff_t cat_kms;
	/** Modification time. Measured in seconds since epoch. */
	time64_t cat_mtime;
	/** Access time. Measured in seconds since epoch. */
	time64_t cat_atime;
	/** Change time. Measured in seconds since epoch. */
	time64_t cat_ctime;
	/**
	 * Blocks allocated to this cl_object on the server file system.
	 *
	 * \todo XXX An interface for block size is needed.
	 */
	__u64  cat_blocks;
	/**
	 * User identifier for quota purposes.
	 */
	uid_t  cat_uid;
	/**
	 * Group identifier for quota purposes.
	 */
	gid_t  cat_gid;

	/* nlink of the directory */
	__u64  cat_nlink;

	/* Project identifier for quota purpose. */
	__u32  cat_projid;
};

/**
 * Fields in cl_attr that are being set.
 */
enum cl_attr_valid {
	CAT_SIZE	= BIT(0),
	CAT_KMS		= BIT(1),
	CAT_MTIME	= BIT(3),
	CAT_ATIME	= BIT(4),
	CAT_CTIME	= BIT(5),
	CAT_BLOCKS	= BIT(6),
	CAT_UID		= BIT(7),
	CAT_GID		= BIT(8),
	CAT_PROJID	= BIT(9),
	CAT_COMPRESSIBLE= BIT(10),
};

/**
 * Sub-class of lu_object with methods common for objects on the client
 * stacks.
 *
 * cl_object: represents a regular file system object, both a file and a
 *    stripe. cl_object is based on lu_object: it is identified by a fid,
 *    layered, cached, hashed, and lrued. Important distinction with the server
 *    side, where md_object and dt_object are used, is that cl_object "fans out"
 *    at the lov/sns level: depending on the file layout, single file is
 *    represented as a set of "sub-objects" (stripes). At the implementation
 *    level, struct lov_object contains an array of cl_objects. Each sub-object
 *    is a full-fledged cl_object, having its fid, living in the lru and hash
 *    table.
 *
 *    This leads to the next important difference with the server side: on the
 *    client, it's quite usual to have objects with the different sequence of
 *    layers. For example, typical top-object is composed of the following
 *    layers:
 *
 *        - vvp
 *        - lov
 *
 *    whereas its sub-objects are composed of
 *
 *        - lovsub
 *        - osc
 *
 *    layers. Here "lovsub" is a mostly dummy layer, whose purpose is to keep
 *    track of the object-subobject relationship.
 *
 *    Sub-objects are not cached independently: when top-object is about to
 *    be discarded from the memory, all its sub-objects are torn-down and
 *    destroyed too.
 *
 * \see vvp_object, lov_object, lovsub_object, osc_object
 */
struct cl_object {
	/** super class */
	struct lu_object                   co_lu;
	/** per-object-layer operations */
	const struct cl_object_operations *co_ops;
	/** offset of page slice in cl_page buffer */
	int				   co_slice_off;
};

/**
 * Description of the client object configuration. This is used for the
 * creation of a new client object that is identified by a more state than
 * fid.
 */
struct cl_object_conf {
	/** Super-class. */
	struct lu_object_conf     coc_lu;
	union {
		/**
		 * Object layout. This is consumed by lov.
		 */
		struct lu_buf	 coc_layout;
		/**
		 * Description of particular stripe location in the
		 * cluster. This is consumed by osc.
		 */
		struct lov_oinfo *coc_oinfo;
	} u;
	/**
	 * VFS inode. This is consumed by vvp.
	 */
	struct inode             *coc_inode;
	/**
	 * Layout lock handle.
	 */
	struct ldlm_lock	 *coc_lock;
	bool			 coc_try;
	/**
	 * Operation to handle layout, OBJECT_CONF_XYZ.
	 */
	int			  coc_opc;
};

enum {
	/** configure layout, new stripe, must must be holding layout lock. */
	OBJECT_CONF_SET = 0,
	/** invalidate the current stripe config when losing layout lock. */
	OBJECT_CONF_INVALIDATE = 1,
	/** wait for old layout to go away so that new layout can be set up. */
	OBJECT_CONF_WAIT = 2
};

enum {
	CL_LAYOUT_GEN_NONE	= (u32)-2,	/* layout lock was cancelled */
	CL_LAYOUT_GEN_EMPTY	= (u32)-1,	/* for empty layout */
};

struct cl_layout {
	/** the buffer to return the layout in lov_mds_md format. */
	struct lu_buf	cl_buf;
	/** size of layout in lov_mds_md format. */
	size_t		cl_size;
	/** Layout generation. */
	u32		cl_layout_gen;
	/** whether layout is a composite one */
	bool		cl_is_composite;
	/** Whether layout is a HSM released one */
	bool		cl_is_released;
	/** Whether layout is a readonly one */
	bool		cl_is_rdonly;
};

enum coo_inode_opc {
	COIO_INODE_LOCK,
	COIO_INODE_UNLOCK,
	COIO_SIZE_LOCK,
	COIO_SIZE_UNLOCK,
};

struct cl_dio_pages;

/**
 * Operations implemented for each cl object layer.
 *
 * \see vvp_ops, lov_ops, lovsub_ops, osc_ops
 */
struct cl_object_operations {
	/**
	 * Initialize page slice for this layer. Called top-to-bottom through
	 * every object layer when a new cl_page is instantiated. Layer
	 * keeping private per-page data, or requiring its own page operations
	 * vector should allocate these data here, and attach then to the page
	 * by calling cl_page_slice_add(). \a vmpage is locked (in the VM
	 * sense). Optional.
	 *
	 * \retval NULL success.
	 *
	 * \retval ERR_PTR(errno) failure code.
	 *
	 * \retval valid-pointer pointer to already existing referenced page
	 *         to be used instead of newly created.
	 */
	int  (*coo_page_init)(const struct lu_env *env, struct cl_object *obj,
			      struct cl_page *page, pgoff_t index);
	/**
	 * Initialize the dio pages structure with information from this layer
	 *
	 * Called top-to-bottom through every object layer to gather the
	 * per-layer information required for the dio, does the same job as
	 * coo_page_init but just once for each dio page array
	 */
	int  (*coo_dio_pages_init)(const struct lu_env *env,
				   struct cl_object *obj,
				   struct cl_dio_pages *cdp,
				   pgoff_t index);
	/**
	 * Initialize lock slice for this layer. Called top-to-bottom through
	 * every object layer when a new cl_lock is instantiated. Layer
	 * keeping private per-lock data, or requiring its own lock operations
	 * vector should allocate these data here, and attach then to the lock
	 * by calling cl_lock_slice_add(). Mandatory.
	 */
	int  (*coo_lock_init)(const struct lu_env *env,
			      struct cl_object *obj, struct cl_lock *lock,
			      const struct cl_io *io);
	/**
	 * Initialize io state for a given layer.
	 *
	 * called top-to-bottom once per io existence to initialize io
	 * state. If layer wants to keep some state for this type of io, it
	 * has to embed struct cl_io_slice in lu_env::le_ses, and register
	 * slice with cl_io_slice_add(). It is guaranteed that all threads
	 * participating in this io share the same session.
	 */
	int  (*coo_io_init)(const struct lu_env *env,
			    struct cl_object *obj, struct cl_io *io);
	/**
	 * Fill portion of \a attr that this layer controls. This method is
	 * called top-to-bottom through all object layers.
	 *
	 * \pre cl_object_header::coh_attr_guard of the top-object is locked.
	 *
	 * \return   0: to continue
	 * \return +ve: to stop iterating through layers (but 0 is returned
	 *              from enclosing cl_object_attr_get())
	 * \return -ve: to signal error
	 */
	int (*coo_attr_get)(const struct lu_env *env, struct cl_object *obj,
			    struct cl_attr *attr);
	/**
	 * Update attributes.
	 *
	 * \a valid is a bitmask composed from enum #cl_attr_valid, and
	 * indicating what attributes are to be set.
	 *
	 * \pre cl_object_header::coh_attr_guard of the top-object is locked.
	 *
	 * \return the same convention as for
	 * cl_object_operations::coo_attr_get() is used.
	 */
	int (*coo_attr_update)(const struct lu_env *env, struct cl_object *obj,
			       const struct cl_attr *attr,
			       enum cl_attr_valid valid);
	/**
	 * Mark the inode dirty. By this way, the inode will add into the
	 * writeback list of the corresponding @bdi_writeback, and then it will
	 * defer to write out the dirty pages to OSTs via the kernel writeback
	 * mechanism.
	 */
	void (*coo_dirty_for_sync)(const struct lu_env *env,
				   struct cl_object *obj);
	/**
	 * Update object configuration. Called top-to-bottom to modify object
	 * configuration.
	 *
	 * XXX error conditions and handling.
	 */
	int (*coo_conf_set)(const struct lu_env *env, struct cl_object *obj,
			    const struct cl_object_conf *conf);
	/**
	 * Glimpse ast. Executed when glimpse ast arrives for a lock on this
	 * object. Layers are supposed to fill parts of \a lvb that will be
	 * shipped to the glimpse originator as a glimpse result.
	 *
	 * \see vvp_object_glimpse(), lovsub_object_glimpse(),
	 * \see osc_object_glimpse()
	 */
	int (*coo_glimpse)(const struct lu_env *env,
			   const struct cl_object *obj, struct ost_lvb *lvb);
	/**
	 * Object prune method. Called when the layout is going to change on
	 * this object, therefore each layer has to clean up their cache,
	 * mainly pages and locks.
	 */
	int (*coo_prune)(const struct lu_env *env, struct cl_object *obj);
	/**
	 * Object getstripe method.
	 */
	int (*coo_getstripe)(const struct lu_env *env, struct cl_object *obj,
			     struct lov_user_md __user *lum, size_t size);
	/**
	 * Get FIEMAP mapping from the object.
	 */
	int (*coo_fiemap)(const struct lu_env *env, struct cl_object *obj,
			  struct ll_fiemap_info_key *fmkey,
			  struct fiemap *fiemap, size_t *buflen);
	/**
	 * Get layout and generation of the object.
	 */
	int (*coo_layout_get)(const struct lu_env *env, struct cl_object *obj,
			      struct cl_layout *layout);
	/**
	 * Get maximum size of the object.
	 */
	loff_t (*coo_maxbytes)(struct cl_object *obj);
	/**
	 * Set request attributes.
	 */
	void (*coo_req_attr_set)(const struct lu_env *env,
				 struct cl_object *obj,
				 struct cl_req_attr *attr);
	/**
	 * Flush \a obj data corresponding to \a lock. Used for DoM
	 * locks in llite's cancelling blocking ast callback.
	 */
	int (*coo_object_flush)(const struct lu_env *env,
				struct cl_object *obj,
				struct ldlm_lock *lock);
	/**
	 * operate upon inode. Used in LOV to lock/unlock inode from vvp layer.
	 */
	int (*coo_inode_ops)(const struct lu_env *env, struct cl_object *obj,
			     enum coo_inode_opc opc, void *data);
	/**
	 * Get ProjID for a request.
	 */
	void (*coo_req_projid_set)(const struct lu_env *env,
				   struct cl_object *obj, __u32 *projid);
};

/**
 * Extended header for client object.
 */
struct cl_object_header {
	/* Standard lu_object_header. cl_object::co_lu::lo_header points here.*/
	struct lu_object_header	coh_lu;

	/**
	 * Parent object. It is assumed that an object has a well-defined
	 * parent, but not a well-defined child (there may be multiple
	 * sub-objects, for the same top-object). cl_object_header::coh_parent
	 * field allows certain code to be written generically, without
	 * limiting possible cl_object layouts unduly.
	 */
	struct cl_object_header *coh_parent;
	/**
	 * Protects consistency between cl_attr of parent object and
	 * attributes of sub-objects, that the former is calculated ("merged")
	 * from.
	 *
	 * \todo XXX this can be read/write lock if needed.
	 */
	spinlock_t		 coh_attr_guard;
	/**
	 * Size of cl_page + page slices
	 */
	unsigned short		 coh_page_bufsize;
	/**
	 * Number of objects above this one: 0 for a top-object, 1 for its
	 * sub-object, etc.
	 */
	unsigned char		 coh_nesting;
};

/**
 * Helper macro: iterate over all layers of the object \a obj, assigning every
 * layer top-to-bottom to \a slice.
 */
#define cl_object_for_each(slice, obj)				\
	list_for_each_entry((slice),				\
			    &(obj)->co_lu.lo_header->loh_layers,\
			    co_lu.lo_linkage)

/**
 * Helper macro: iterate over all layers of the object \a obj, assigning every
 * layer bottom-to-top to \a slice.
 */
#define cl_object_for_each_reverse(slice, obj)				\
	list_for_each_entry_reverse((slice),				\
				    &(obj)->co_lu.lo_header->loh_layers,\
				    co_lu.lo_linkage)

#define CL_PAGE_EOF ((pgoff_t)~0ull)

/** \struct cl_page
 * Layered client page.
 *
 * cl_page: represents a portion of a file, cached in the memory. All pages
 *    of the given file are of the same size, and are kept in the radix tree
 *    hanging off the cl_object. cl_page doesn't fan out, but as sub-objects
 *    of the top-level file object are first class cl_objects, they have their
 *    own radix trees of pages and hence page is implemented as a sequence of
 *    struct cl_pages's, linked into double-linked list through
 *    cl_page::cp_parent and cl_page::cp_child pointers, each residing in the
 *    corresponding radix tree at the corresponding logical offset.
 *
 * cl_page is associated with VM page of the hosting environment (struct
 *    page in Linux kernel, for example), struct page. It is assumed, that this
 *    association is implemented by one of cl_page layers (top layer in the
 *    current design) that
 *
 *        - intercepts per-VM-page call-backs made by the environment (e.g.,
 *          memory pressure),
 *
 *        - translates state (page flag bits) and locking between lustre and
 *          environment.
 *
 *    The association between cl_page and struct page is immutable and
 *    established when cl_page is created.
 *
 * cl_page can be "owned" by a particular cl_io (see below), guaranteeing
 *    this io an exclusive access to this page w.r.t. other io attempts and
 *    various events changing page state (such as transfer completion, or
 *    eviction of the page from the memory). Note, that in general cl_io
 *    cannot be identified with a particular thread, and page ownership is not
 *    exactly equal to the current thread holding a lock on the page. Layer
 *    implementing association between cl_page and struct page has to implement
 *    ownership on top of available synchronization mechanisms.
 *
 *    While lustre client maintains the notion of an page ownership by io,
 *    hosting MM/VM usually has its own page concurrency control
 *    mechanisms. For example, in Linux, page access is synchronized by the
 *    per-page PG_locked bit-lock, and generic kernel code (generic_file_*())
 *    takes care to acquire and release such locks as necessary around the
 *    calls to the file system methods (->readpage(), ->prepare_write(),
 *    ->commit_write(), etc.). This leads to the situation when there are two
 *    different ways to own a page in the client:
 *
 *        - client code explicitly and voluntary owns the page (cl_page_own());
 *
 *        - VM locks a page and then calls the client, that has "to assume"
 *          the ownership from the VM (cl_page_assume()).
 *
 *    Dual methods to release ownership are cl_page_disown() and
 *    cl_page_unassume().
 *
 * cl_page is reference counted (cl_page::cp_ref). When reference counter
 *    drops to 0, the page is returned to the cache, unless it is in
 *    cl_page_state::CPS_FREEING state, in which case it is immediately
 *    destroyed.
 *
 *    The general logic guaranteeing the absence of "existential races" for
 *    pages is the following:
 *
 *        - there are fixed known ways for a thread to obtain a new reference
 *          to a page:
 *
 *            - by doing a lookup in the cl_object radix tree, protected by the
 *              spin-lock;
 *
 *            - by starting from VM-locked struct page and following some
 *              hosting environment method (e.g., following ->private pointer in
 *              the case of Linux kernel), see cl_vmpage_page();
 *
 *        - when the page enters cl_page_state::CPS_FREEING state, all these
 *          ways are severed with the proper synchronization
 *          (cl_page_delete());
 *
 *        - entry into cl_page_state::CPS_FREEING is serialized by the VM page
 *          lock;
 *
 *        - no new references to the page in cl_page_state::CPS_FREEING state
 *          are allowed (checked in cl_page_get()).
 *
 *    Together this guarantees that when last reference to a
 *    cl_page_state::CPS_FREEING page is released, it is safe to destroy the
 *    page, as neither references to it can be acquired at that point, nor
 *    ones exist.
 *
 * cl_page is a state machine. States are enumerated in enum
 *    cl_page_state. Possible state transitions are enumerated in
 *    cl_page_state_set(). State transition process (i.e., actual changing of
 *    cl_page::cp_state field) is protected by the lock on the underlying VM
 *    page.
 *
 * Linux Kernel implementation.
 *
 *    Binding between cl_page and struct page (which is a typedef for
 *    struct page) is implemented in the vvp layer. cl_page is attached to the
 *    ->private pointer of the struct page, together with the setting of
 *    PG_private bit in page->flags, and acquiring additional reference on the
 *    struct page (much like struct buffer_head, or any similar file system
 *    private data structures).
 *
 *    PG_locked lock is used to implement both ownership and transfer
 *    synchronization, that is, page is VM-locked in CPS_{OWNED,PAGE{IN,OUT}}
 *    states. No additional references are acquired for the duration of the
 *    transfer.
 *
 * \warning *THIS IS NOT* the behavior expected by the Linux kernel, where
 *          write-out is "protected" by the special PG_writeback bit.
 */

/**
 * States of cl_page. cl_page.c assumes particular order here.
 *
 * The page state machine is rather crude, as it doesn't recognize finer page
 * states like "dirty" or "up to date". This is because such states are not
 * always well defined for the whole stack (see, for example, the
 * implementation of the read-ahead, that hides page up-to-dateness to track
 * cache hits accurately). Such sub-states are maintained by the layers that
 * are interested in them.
 */
enum cl_page_state {
	/**
	 * Page is in the cache, un-owned. Page leaves cached state in the
	 * following cases:
	 *
	 *     - [cl_page_state::CPS_OWNED] io comes across the page and
	 *     owns it;
	 *
	 *     - [cl_page_state::CPS_PAGEOUT] page is dirty, the
	 *     req-formation engine decides that it wants to include this page
	 *     into an RPC being constructed, and yanks it from the cache;
	 *
	 *     - [cl_page_state::CPS_FREEING] VM callback is executed to
	 *     evict the page form the memory;
	 *
	 * \invariant cl_page::cp_owner == NULL && cl_page::cp_req == NULL
	 */
	CPS_CACHED = 1,
	/**
	 * Page is exclusively owned by some cl_io. Page may end up in this
	 * state as a result of
	 *
	 *     - io creating new page and immediately owning it;
	 *
	 *     - [cl_page_state::CPS_CACHED] io finding existing cached page
	 *     and owning it;
	 *
	 *     - [cl_page_state::CPS_OWNED] io finding existing owned page
	 *     and waiting for owner to release the page;
	 *
	 * Page leaves owned state in the following cases:
	 *
	 *     - [cl_page_state::CPS_CACHED] io decides to leave the page in
	 *     the cache, doing nothing;
	 *
	 *     - [cl_page_state::CPS_PAGEIN] io starts read transfer for
	 *     this page;
	 *
	 *     - [cl_page_state::CPS_PAGEOUT] io starts immediate write
	 *     transfer for this page;
	 *
	 *     - [cl_page_state::CPS_FREEING] io decides to destroy this
	 *     page (e.g., as part of truncate or extent lock cancellation).
	 *
	 * \invariant cl_page::cp_owner != NULL && cl_page::cp_req == NULL
	 */
	CPS_OWNED,
	/**
	 * Page is being written out, as a part of a transfer. This state is
	 * entered when req-formation logic decided that it wants this page to
	 * be sent through the wire _now_. Specifically, it means that once
	 * this state is achieved, transfer completion handler (with either
	 * success or failure indication) is guaranteed to be executed against
	 * this page independently of any locks and any scheduling decisions
	 * made by the hosting environment (that effectively means that the
	 * page is never put into cl_page_state::CPS_PAGEOUT state "in
	 * advance". This property is mentioned, because it is important when
	 * reasoning about possible dead-locks in the system). The page can
	 * enter this state as a result of
	 *
	 *     - [cl_page_state::CPS_OWNED] an io requesting an immediate
	 *     write-out of this page, or
	 *
	 *     - [cl_page_state::CPS_CACHED] req-forming engine deciding
	 *     that it has enough dirty pages cached to issue a "good"
	 *     transfer.
	 *
	 * The page leaves cl_page_state::CPS_PAGEOUT state when the transfer
	 * is completed---it is moved into cl_page_state::CPS_CACHED state.
	 *
	 * Underlying VM page is locked for the duration of transfer.
	 *
	 * \invariant: cl_page::cp_owner == NULL && cl_page::cp_req != NULL
	 */
	CPS_PAGEOUT,
	/**
	 * Page is being read in, as a part of a transfer. This is quite
	 * similar to the cl_page_state::CPS_PAGEOUT state, except that
	 * read-in is always "immediate"---there is no such thing a sudden
	 * construction of read request from cached, presumably not up to date,
	 * pages.
	 *
	 * Underlying VM page is locked for the duration of transfer.
	 *
	 * \invariant: cl_page::cp_owner == NULL && cl_page::cp_req != NULL
	 */
	CPS_PAGEIN,
	/**
	 * Page is being destroyed. This state is entered when client decides
	 * that page has to be deleted from its host object, as, e.g., a part
	 * of truncate.
	 *
	 * Once this state is reached, there is no way to escape it.
	 *
	 * \invariant: cl_page::cp_owner == NULL && cl_page::cp_req == NULL
	 */
	CPS_FREEING,
	CPS_NR
};

enum cl_page_type {
	/** Host page, the page is from the host inode which the cl_page
	 * belongs to.
	 */
	CPT_CACHEABLE = 1,

	/** Transient page, the transient cl_page is used to bind a cl_page
	 *  to vmpage which is not belonging to the same object of cl_page.
	 *  it is used in DirectIO and lockless IO.
	 */
	CPT_TRANSIENT,
	CPT_NR
};

#define	CP_STATE_BITS	4
#define	CP_TYPE_BITS	2
#define	CP_MAX_LAYER	2

/**
 * Fields are protected by the lock on struct page, except for atomics and
 * immutables.
 *
 * \invariant Data type invariants are in cl_page_invariant(). Basically:
 * cl_page::cp_parent and cl_page::cp_child are a well-formed double-linked
 * list, consistent with the parent/child pointers in the cl_page::cp_obj and
 * cl_page::cp_owner (when set).
 */
struct cl_page {
	/** Reference counter. */
	refcount_t		cp_ref;
	/** layout_entry + stripe index, composed using lov_comp_index() */
	unsigned int		cp_lov_index;
	/** page->index of the page within the whole file */
	pgoff_t			cp_page_index;
	/** An object this page is a part of. Immutable after creation. */
	struct cl_object	*cp_obj;
	/** vmpage */
	struct page		*cp_vmpage;
	/**
	 * Assigned if doing direct IO, because in this case cp_vmpage is not
	 * a valid page cache page, hence the inode cannot be inferred from
	 * cp_vmpage->mapping->host.
	 */
	struct inode		*cp_inode;
	/** Linkage of pages within group. Pages must be owned */
	struct list_head	cp_batch;
	/** array of slices offset. Immutable after creation. */
	unsigned char		cp_layer_offset[CP_MAX_LAYER];
	/** current slice index */
	unsigned char		cp_layer_count:2;
	/**
	 * Page state. This field is const to avoid accidental update, it is
	 * modified only internally within cl_page.c. Protected by a VM lock.
	 */
	enum cl_page_state	 cp_state:CP_STATE_BITS;
	/**
	 * Page type. Only CPT_TRANSIENT is used so far. Immutable after
	 * creation.
	 */
	enum cl_page_type	cp_type:CP_TYPE_BITS;
	unsigned		cp_defer_uptodate:1,
				cp_ra_updated:1,
				cp_ra_used:1,
				cp_in_kmem_array:1;
	union {
		/* which slab kmem index this memory allocated from */
		short int	cp_kmem_index;
		/* or the page size if it's not in the slab kmem array */
		short int	cp_kmem_size;
	};

	/**
	 * Owning IO in cl_page_state::CPS_OWNED state. Sub-page can be owned
	 * by sub-io. Protected by a VM lock.
	 */
	struct cl_io            *cp_owner;
	/** Assigned if doing a sync_io */
	struct cl_sync_io	*cp_sync_io;
};

/**
 * Per-layer part of cl_page.
 *
 * \see vvp_page, lov_page, osc_page
 */
struct cl_page_slice {
	struct cl_page                  *cpl_page;
	const struct cl_page_operations *cpl_ops;
};

/**
 * Lock mode. For the client extent locks.
 *
 * \ingroup cl_lock
 */
enum cl_lock_mode {
	CLM_READ,
	CLM_WRITE,
	CLM_GROUP,
	CLM_MAX,
};

/**
 * Requested transfer type.
 */
enum cl_req_type {
	CRT_READ,
	CRT_WRITE,
	CRT_NR
};

/**
 * Per-layer page operations.
 *
 * Methods taking an \a io argument are for the activity happening in the
 * context of given \a io. Page is assumed to be owned by that io, except for
 * the obvious cases.
 *
 * \see vvp_page_ops, lov_page_ops, osc_page_ops
 */
struct cl_page_operations {
	/**
	 * cl_page<->struct page methods. Only one layer in the stack has to
	 * implement these. Current code assumes that this functionality is
	 * provided by the topmost layer, see __cl_page_disown() as an example.
	 */

	/**
	 * Update file attributes when all we have is this page.  Used for tiny
	 * writes to update attributes when we don't have a full cl_io.
	 */
	void (*cpo_page_touch)(const struct lu_env *env,
			       const struct cl_page_slice *slice, size_t to);
	/**
	 * Page destruction.
	 */

	/**
	 * Called when page is truncated from the object. Optional.
	 *
	 * \see cl_page_discard()
	 * \see vvp_page_discard(), osc_page_discard()
	 */
	void (*cpo_discard)(const struct lu_env *env,
			    const struct cl_page_slice *slice,
			    struct cl_io *io);
	/**
	 * Called when page is removed from the cache, and is about to being
	 * destroyed. Optional.
	 *
	 * \see cl_page_delete()
	 * \see vvp_page_delete(), osc_page_delete()
	 */
	void (*cpo_delete)(const struct lu_env *env,
			   const struct cl_page_slice *slice);
	/**
	 * Optional debugging helper. Prints given page slice.
	 *
	 * \see cl_page_print()
	 */
	int (*cpo_print)(const struct lu_env *env,
			 const struct cl_page_slice *slice,
			 void *cookie, lu_printer_t p);
	/**
	 * \name transfer
	 *
	 * Transfer methods.
	 *
	 */
	/**
	 * Request type dependent vector of operations.
	 *
	 * Transfer operations depend on transfer mode (cl_req_type). To avoid
	 * passing transfer mode to each and every of these methods, and to
	 * avoid branching on request type inside of the methods, separate
	 * methods for cl_req_type:CRT_READ and cl_req_type:CRT_WRITE are
	 * provided. That is, method invocation usually looks like
	 *
	 *         slice->cp_ops.io[req->crq_type].cpo_method(env, slice, ...);
	 */
	struct {
		/**
		 * Completion handler. This is guaranteed to be eventually
		 * fired after cl_page_prep() or cl_page_make_ready() call.
		 *
		 * This method can be called in a non-blocking context. It is
		 * guaranteed however, that the page involved and its object
		 * are pinned in memory (and, hence, calling cl_page_put() is
		 * safe).
		 *
		 * \see cl_page_complete()
		 */
		void (*cpo_complete)(const struct lu_env *env,
				     const struct cl_page_slice *slice,
				       int ioret);
	} io[CRT_NR];
	/**
	 * Tell transfer engine that only [to, from] part of a page should be
	 * transmitted.
	 *
	 * This is used for immediate transfers.
	 *
	 * \todo XXX this is not very good interface. It would be much better
	 * if all transfer parameters were supplied as arguments to
	 * cl_io_operations::cio_submit() call, but it is not clear how to do
	 * this for page queues.
	 *
	 * \see cl_page_clip()
	 */
	void (*cpo_clip)(const struct lu_env *env,
			 const struct cl_page_slice *slice, int from, int to);
	/**
	 * Write out a page by kernel. This is only called by ll_writepage
	 * right now.
	 *
	 * \see cl_page_flush()
	 */
	int (*cpo_flush)(const struct lu_env *env,
			 const struct cl_page_slice *slice,
			 struct cl_io *io);
};

/**
 * Helper macro, dumping detailed information about \a page into a log.
 */
#define CL_PAGE_DEBUG(mask, env, page, format, ...)                     \
do {                                                                    \
	if (cfs_cdebug_show(mask, DEBUG_SUBSYSTEM)) {                   \
		LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, mask, NULL);        \
		cl_page_print(env, &msgdata, lu_cdebug_printer, page);  \
		CDEBUG(mask, format, ## __VA_ARGS__);                   \
	}                                                               \
} while (0)

/**
 * Helper macro, dumping shorter information about \a page into a log.
 */
#define CL_PAGE_HEADER(mask, env, page, format, ...)                          \
do {                                                                          \
	if (cfs_cdebug_show(mask, DEBUG_SUBSYSTEM)) {                         \
		LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, mask, NULL);              \
		cl_page_header_print(env, &msgdata, lu_cdebug_printer, page); \
		CDEBUG(mask, format, ## __VA_ARGS__);                         \
	}                                                                     \
} while (0)

static inline struct page *cl_page_vmpage(const struct cl_page *page)
{
	LASSERT(page->cp_vmpage != NULL);
	return page->cp_vmpage;
}

static inline pgoff_t cl_page_index(const struct cl_page *cp)
{
	return cl_page_vmpage(cp)->index;
}

/**
 * Check if a cl_page is in use.
 *
 * Client cache holds a refcount, this refcount will be dropped when
 * the page is taken out of cache, see vvp_page_delete().
 */
static inline bool __page_in_use(const struct cl_page *page, int refc)
{
	return (refcount_read(&page->cp_ref) > refc + 1);
}

/**
 * Caller itself holds a refcount of cl_page.
 */
#define cl_page_in_use(pg)       __page_in_use(pg, 1)
/**
 * Caller doesn't hold a refcount.
 */
#define cl_page_in_use_noref(pg) __page_in_use(pg, 0)

/** \struct cl_lock
 *
 * Extent locking on the client.
 *
 * LAYERING
 *
 * The locking model of the new client code is built around
 *
 *        struct cl_lock
 *
 * data-type representing an extent lock on a regular file. cl_lock is a
 * layered object (much like cl_object and cl_page), it consists of a header
 * (struct cl_lock) and a list of layers (struct cl_lock_slice), linked to
 * cl_lock::cll_layers list through cl_lock_slice::cls_linkage.
 *
 * Typical cl_lock consists of one layer:
 *
 *     - lov_lock (lov specific data).
 *
 * lov_lock contains an array of sub-locks. Each of these sub-locks is a
 * normal cl_lock: it has a header (struct cl_lock) and a list of layers:
 *
 *     - osc_lock
 *
 * Each sub-lock is associated with a cl_object (representing stripe
 * sub-object or the file to which top-level cl_lock is associated to), and is
 * linked into that cl_object::coh_locks. In this respect cl_lock is similar to
 * cl_object (that at lov layer also fans out into multiple sub-objects), and
 * is different from cl_page, that doesn't fan out (there is usually exactly
 * one osc_page for every vvp_page). We shall call vvp-lov portion of the lock
 * a "top-lock" and its lovsub-osc portion a "sub-lock".
 *
 * LIFE CYCLE
 *
 * cl_lock is a cacheless data container for the requirements of locks to
 * complete the IO. cl_lock is created before I/O starts and destroyed when the
 * I/O is complete.
 *
 * cl_lock depends on LDLM lock to fulfill lock semantics. LDLM lock is attached
 * to cl_lock at OSC layer. LDLM lock is still cacheable.
 *
 * INTERFACE AND USAGE
 *
 * Two major methods are supported for cl_lock: clo_enqueue and clo_cancel.  A
 * cl_lock is enqueued by cl_lock_request(), which will call clo_enqueue()
 * methods for each layer to enqueue the lock. At the LOV layer, if a cl_lock
 * consists of multiple sub cl_locks, each sub locks will be enqueued
 * correspondingly. At OSC layer, the lock enqueue request will tend to reuse
 * cached LDLM lock; otherwise a new LDLM lock will have to be requested from
 * OST side.
 *
 * cl_lock_cancel() must be called to release a cl_lock after use. clo_cancel()
 * method will be called for each layer to release the resource held by this
 * lock. At OSC layer, the reference count of LDLM lock, which is held at
 * clo_enqueue time, is released.
 *
 * LDLM lock can only be canceled if there is no cl_lock using it.
 *
 * Overall process of the locking during IO operation is as following:
 *
 *     - once parameters for IO are setup in cl_io, cl_io_operations::cio_lock()
 *       is called on each layer. Responsibility of this method is to add locks,
 *       needed by a given layer into cl_io.ci_lockset.
 *
 *     - once locks for all layers were collected, they are sorted to avoid
 *       dead-locks (cl_io_locks_sort()), and enqueued.
 *
 *     - when all locks are acquired, IO is performed;
 *
 *     - locks are released after IO is complete.
 *
 * Striping introduces major additional complexity into locking. The
 * fundamental problem is that it is generally unsafe to actively use (hold)
 * two locks on the different OST servers at the same time, as this introduces
 * inter-server dependency and can lead to cascading evictions.
 *
 * Basic solution is to sub-divide large read/write IOs into smaller pieces so
 * that no multi-stripe locks are taken (note that this design abandons POSIX
 * read/write semantics). Such pieces ideally can be executed concurrently. At
 * the same time, certain types of IO cannot be sub-divived, without
 * sacrificing correctness. This includes:
 *
 *  - O_APPEND write, where [0, EOF] lock has to be taken, to guarantee
 *  atomicity;
 *
 *  - ftruncate(fd, offset), where [offset, EOF] lock has to be taken.
 *
 * Also, in the case of read(fd, buf, count) or write(fd, buf, count), where
 * buf is a part of memory mapped Lustre file, a lock or locks protecting buf
 * has to be held together with the usual lock on [offset, offset + count].
 *
 * Interaction with DLM
 *
 * In the expected setup, cl_lock is ultimately backed up by a collection of
 * DLM locks (struct ldlm_lock). Association between cl_lock and DLM lock is
 * implemented in osc layer, that also matches DLM events (ASTs, cancellation,
 * etc.) into cl_lock_operation calls. See struct osc_lock for a more detailed
 * description of interaction with DLM.
 */

/**
 * Lock description.
 */
struct cl_lock_descr {
	/** Object this lock is granted for. */
	struct cl_object *cld_obj;
	/** Index of the first page protected by this lock. */
	pgoff_t           cld_start;
	/** Index of the last page (inclusive) protected by this lock. */
	pgoff_t           cld_end;
	/** Group ID, for group lock */
	__u64             cld_gid;
	/** Lock mode. */
	enum cl_lock_mode cld_mode;
	/**
	 * flags to enqueue lock. A combination of bit-flags from
	 * enum cl_enq_flags.
	 */
	__u32             cld_enq_flags;
};

#define DDESCR "%s(%d):[%lu, %lu]:%x"
#define PDESCR(descr)							\
	cl_lock_mode_name((descr)->cld_mode), (descr)->cld_mode,	\
	(descr)->cld_start, (descr)->cld_end, (descr)->cld_enq_flags

const char *cl_lock_mode_name(const enum cl_lock_mode mode);

/**
 * Layered client lock.
 */
struct cl_lock {
	/** List of slices. Immutable after creation. */
	struct list_head      cll_layers;
	/** lock attribute, extent, cl_object, etc. */
	struct cl_lock_descr  cll_descr;
};

/**
 * Per-layer part of cl_lock
 *
 * \see lov_lock, osc_lock
 */
struct cl_lock_slice {
	struct cl_lock                  *cls_lock;
	/** Object slice corresponding to this lock slice. Immutable after
	 * creation.
	 */
	struct cl_object                *cls_obj;
	const struct cl_lock_operations *cls_ops;
	/** Linkage into cl_lock::cll_layers. Immutable after creation. */
	struct list_head		 cls_linkage;
};

/**
 *
 * \see lov_lock_ops, osc_lock_ops
 */
struct cl_lock_operations {
	/**
	 * Attempts to enqueue the lock. Called top-to-bottom.
	 *
	 * \retval 0	this layer has enqueued the lock successfully
	 * \retval >0	this layer has enqueued the lock, but need to wait on
	 *		@anchor for resources
	 * \retval -ve	failure
	 *
	 * \see lov_lock_enqueue(), osc_lock_enqueue()
	 */
	int  (*clo_enqueue)(const struct lu_env *env,
			    const struct cl_lock_slice *slice,
			    struct cl_io *io, struct cl_sync_io *anchor);
	/**
	 * Cancel a lock, release its DLM lock ref, while does not cancel the
	 * DLM lock
	 */
	void (*clo_cancel)(const struct lu_env *env,
			   const struct cl_lock_slice *slice);
	/**
	 * Destructor. Frees resources and the slice.
	 *
	 * \see lov_lock_fini(), osc_lock_fini()
	 */
	void (*clo_fini)(const struct lu_env *env, struct cl_lock_slice *slice);
	/**
	 * Optional debugging helper. Prints given lock slice.
	 */
	int (*clo_print)(const struct lu_env *env, void *cookie,
			 lu_printer_t p, const struct cl_lock_slice *slice);
};

#define CL_LOCK_DEBUG(mask, env, lock, format, ...)                     \
do {                                                                    \
	if (cfs_cdebug_show(mask, DEBUG_SUBSYSTEM)) {                   \
		LIBCFS_DEBUG_MSG_DATA_DECL(msgdata, mask, NULL);        \
		cl_lock_print(env, &msgdata, lu_cdebug_printer, lock);  \
		CDEBUG(mask, format, ## __VA_ARGS__);                   \
	}                                                               \
} while (0)

#define CL_LOCK_ASSERT(expr, env, lock) do {                            \
	if (likely(expr))                                               \
		break;                                                  \
									\
	CL_LOCK_DEBUG(D_ERROR, env, lock, "failed at %s.\n", #expr);    \
	LBUG();                                                         \
} while (0)


/** \addtogroup cl_page_list cl_page_list
 * Page list used to perform collective operations on a group of pages.
 *
 * Pages are added to the list one by one. cl_page_list acquires a reference
 * for every page in it. Page list is used to perform collective operations on
 * pages:
 *
 *     - submit pages for an immediate transfer,
 *
 *     - own pages on behalf of certain io (waiting for each page in turn),
 *
 *     - discard pages.
 *
 * When list is finalized, it releases references on all pages it still has.
 *
 * \todo XXX concurrency control.
 *
 */
struct cl_page_list {
	unsigned int		 pl_nr;
	struct list_head	 pl_pages;
};

/**
 * A 2-queue of pages. A convenience data-type for common use case, 2-queue
 * contains an incoming page list and an outgoing page list.
 */
struct cl_2queue {
	struct cl_page_list c2_qin;
	struct cl_page_list c2_qout;
};

/** \struct cl_io
 * I/O
 *
 * cl_io represents a high level I/O activity like
 * read(2)/write(2)/truncate(2) system call, or cancellation of an extent
 * lock.
 *
 * cl_io is a layered object, much like cl_{object,page,lock} but with one
 * important distinction. We want to minimize number of calls to the allocator
 * in the fast path, e.g., in the case of read(2) when everything is cached:
 * client already owns the lock over region being read, and data are cached
 * due to read-ahead. To avoid allocation of cl_io layers in such situations,
 * per-layer io state is stored in the session, associated with the io, see
 * struct {vvp,lov,osc}_io for example. Sessions allocation is amortized
 * by using free-lists, see cl_env_get().
 *
 * There is a small predefined number of possible io types, enumerated in enum
 * cl_io_type.
 *
 * cl_io is a state machine, that can be advanced concurrently by the multiple
 * threads. It is up to these threads to control the concurrency and,
 * specifically, to detect when io is done, and its state can be safely
 * released.
 *
 * For read/write io overall execution plan is as following:
 *
 *     (0) initialize io state through all layers;
 *
 *     (1) loop: prepare chunk of work to do
 *
 *     (2) call all layers to collect locks they need to process current chunk
 *
 *     (3) sort all locks to avoid dead-locks, and acquire them
 *
 *     (4) process the chunk: call per-page methods
 *         cl_io_operations::cio_prepare_write(),
 *         cl_io_operations::cio_commit_write() for write)
 *
 *     (5) release locks
 *
 *     (6) repeat loop.
 *
 * To implement the "parallel IO mode", lov layer creates sub-io's (lazily to
 * address allocation efficiency issues mentioned above), and returns with the
 * special error condition from per-page method when current sub-io has to
 * block. This causes io loop to be repeated, and lov switches to the next
 * sub-io in its cl_io_operations::cio_iter_init() implementation.
 */

/** IO types */
enum cl_io_type {
	/** read system call */
	CIT_READ = 1,
	/** write system call */
	CIT_WRITE,
	/** truncate, utime system calls */
	CIT_SETATTR,
	/** get data version */
	CIT_DATA_VERSION,
	/**
	 * page fault handling
	 */
	CIT_FAULT,
	/**
	 * fsync system call handling
	 * To write out a range of file
	 */
	CIT_FSYNC,
	/**
	 * glimpse. An io context to acquire glimpse lock.
	 */
	CIT_GLIMPSE,
	/**
	 * Miscellaneous io. This is used for occasional io activity that
	 * doesn't fit into other types. Currently this is used for:
	 *
	 *     - cancellation of an extent lock. This io exists as a context
	 *     to write dirty pages from under the lock being canceled back
	 *     to the server;
	 *
	 *     - VM induced page write-out. An io context for writing page out
	 *     for memory cleansing;
	 *
	 *     - grouplock. An io context to acquire group lock.
	 *
	 * CIT_MISC io is used simply as a context in which locks and pages
	 * are manipulated. Such io has no internal "process", that is,
	 * cl_io_loop() is never called for it.
	 */
	CIT_MISC,
	/**
	 * ladvise handling
	 * To give advice about access of a file
	 */
	CIT_LADVISE,
	/**
	 * SEEK_HOLE/SEEK_DATA handling to search holes or data
	 * across all file objects
	 */
	CIT_LSEEK,
	CIT_OP_NR
};

/**
 * States of cl_io state machine
 */
enum cl_io_state {
	/** Not initialized. */
	CIS_ZERO,
	/** Initialized. */
	CIS_INIT,
	/** IO iteration started. */
	CIS_IT_STARTED,
	/** Locks taken. */
	CIS_LOCKED,
	/** Actual IO is in progress. */
	CIS_IO_GOING,
	/** IO for the current iteration finished. */
	CIS_IO_FINISHED,
	/** Locks released. */
	CIS_UNLOCKED,
	/** Iteration completed. */
	CIS_IT_ENDED,
	/** cl_io finalized. */
	CIS_FINI
};

/**
 * IO state private for a layer.
 *
 * This is usually embedded into layer session data, rather than allocated
 * dynamically.
 *
 * \see vvp_io, lov_io, osc_io
 */
struct cl_io_slice {
	struct cl_io			*cis_io;
	/** corresponding object slice. Immutable after creation. */
	struct cl_object		*cis_obj;
	/** io operations. Immutable after creation. */
	const struct cl_io_operations	*cis_iop;
	/**
	 * linkage into a list of all slices for a given cl_io, hanging off
	 * cl_io::ci_layers. Immutable after creation.
	 */
	struct list_head		cis_linkage;
};

typedef void (*cl_commit_cbt)(const struct lu_env *, struct cl_io *,
			      struct folio_batch *);

struct cl_read_ahead {
	/* Maximum page index the readahead window will end.
	 * This is determined DLM lock coverage, RPC and stripe boundary.
	 * cra_end is included.
	 */
	pgoff_t		cra_end_idx;
	/* optimal RPC size for this read, by pages */
	unsigned long	cra_rpc_pages;
	/* Release callback. If readahead holds resources underneath, this
	 * function should be called to release it.
	 */
	void		(*cra_release)(const struct lu_env *env,
				       struct cl_read_ahead *ra);

	/* Callback data for cra_release routine */
	void		*cra_dlmlock;
	void		*cra_oio;

	/*
	 * Linkage to track all cl_read_aheads for a read-ahead operations,
	 * used for releasing DLM locks acquired during read-ahead.
	 */
	struct list_head cra_linkage;

	/* whether lock is in contention */
	bool		 cra_contention;
};

static inline void cl_read_ahead_release(const struct lu_env *env,
					 struct cl_read_ahead *ra)
{
	if (ra->cra_release != NULL)
		ra->cra_release(env, ra);
}

enum cl_io_priority {
	/* Normal I/O, usually just queue the pages in the client side cache. */
	IO_PRIO_NORMAL	= 0,
	/* I/O is urgent and should flush queued pages to OSTs ASAP. */
	IO_PRIO_URGENT,
	/* The memcg is under high memory pressure and the user write process
	 * is dirty exceeded and under rate limiting in balance_dirty_pages().
	 * It needs to flush dirty pages for the corresponding @wb ASAP.
	 */
	IO_PRIO_DIRTY_EXCEEDED,
	/*
	 * I/O is urgent and flushing pages are marked with OBD_BRW_SOFT_SYNC
	 * flag and may trigger a soft sync on OSTs. Thus it can free unstable
	 * pages much quickly.
	 */
	IO_PRIO_SOFT_SYNC,
	/*
	 * The system or a certain memcg is under high memory pressure. Need to
	 * flush dirty pages to OSTs immediately and I/O RPC must wait the write
	 * transcation commit on OSTs synchronously to release unstable pages.
	 */
	IO_PRIO_HARD_SYNC,
	IO_PRIO_MAX,
};

static inline bool cl_io_high_prio(enum cl_io_priority prio)
{
	return prio >= IO_PRIO_URGENT;
}

/**
 * Per-layer io operations.
 * \see vvp_io_ops, lov_io_ops, lovsub_io_ops, osc_io_ops
 */
struct cl_io_operations {
	/**
	 * Vector of io state transition methods for every io type.
	 *
	 * \see cl_page_operations::io
	 */
	struct {
		/**
		 * Prepare io iteration at a given layer.
		 *
		 * Called top-to-bottom at the beginning of each iteration of
		 * "io loop" (if it makes sense for this type of io). Here
		 * layer selects what work it will do during this iteration.
		 *
		 * \see cl_io_operations::cio_iter_fini()
		 */
		int (*cio_iter_init)(const struct lu_env *env,
				     const struct cl_io_slice *slice);
		/**
		 * Finalize io iteration.
		 *
		 * Called bottom-to-top at the end of each iteration of "io
		 * loop". Here layers can decide whether IO has to be
		 * continued.
		 *
		 * \see cl_io_operations::cio_iter_init()
		 */
		void (*cio_iter_fini)(const struct lu_env *env,
				      const struct cl_io_slice *slice);
		/**
		 * Collect locks for the current iteration of io.
		 *
		 * Called top-to-bottom to collect all locks necessary for
		 * this iteration. This methods shouldn't actually enqueue
		 * anything, instead it should post a lock through
		 * cl_io_lock_add(). Once all locks are collected, they are
		 * sorted and enqueued in the proper order.
		 */
		int  (*cio_lock)(const struct lu_env *env,
				 const struct cl_io_slice *slice);
		/**
		 * Finalize unlocking.
		 *
		 * Called bottom-to-top to finish layer specific unlocking
		 * functionality, after generic code released all locks
		 * acquired by cl_io_operations::cio_lock().
		 */
		void  (*cio_unlock)(const struct lu_env *env,
				    const struct cl_io_slice *slice);
		/**
		 * Start io iteration.
		 *
		 * Once all locks are acquired, called top-to-bottom to
		 * commence actual IO. In the current implementation,
		 * top-level vvp_io_{read,write}_start() does all the work
		 * synchronously by calling generic_file_*(), so other layers
		 * are called when everything is done.
		 */
		int  (*cio_start)(const struct lu_env *env,
				  const struct cl_io_slice *slice);
		/**
		 * Called top-to-bottom at the end of io loop. Here layer
		 * might wait for an unfinished asynchronous io.
		 */
		void (*cio_end)(const struct lu_env *env,
				const struct cl_io_slice *slice);
		/**
		 * Called bottom-to-top to notify layers that read/write IO
		 * iteration finished, with \a nob bytes transferred.
		 */
		void (*cio_advance)(const struct lu_env *env,
				    const struct cl_io_slice *slice,
				    size_t nob);
		/**
		 * Called once per io, bottom-to-top to release io resources.
		 */
		void (*cio_fini)(const struct lu_env *env,
				  const struct cl_io_slice *slice);
	} op[CIT_OP_NR];

	/**
	 * Submit pages from \a queue->c2_qin for IO, and move
	 * successfully submitted pages into \a queue->c2_qout. Return
	 * non-zero if failed to submit even the single page. If
	 * submission failed after some pages were moved into \a
	 * queue->c2_qout, completion callback with non-zero ioret is
	 * executed on them.
	 */
	int  (*cio_submit)(const struct lu_env *env,
			   struct cl_io *io,
			   const struct cl_io_slice *slice,
			   enum cl_req_type crt, struct cl_2queue *queue);
	/* the dio version of cio_submit, this either submits all pages
	 * successfully or fails.  Uses an array, rather than a queue.
	 */
	int  (*cio_dio_submit)(const struct lu_env *env,
			       struct cl_io *io,
			       const struct cl_io_slice *slice,
			       enum cl_req_type crt,
			       struct cl_dio_pages *cdp);
	/**
	 * Queue async page for write.
	 * The difference between cio_submit and cio_queue is that
	 * cio_submit is for urgent request.
	 */
	int  (*cio_commit_async)(const struct lu_env *env,
				 const struct cl_io_slice *slice,
				 struct cl_page_list *queue, int from, int to,
				 cl_commit_cbt cb, enum cl_io_priority prio);
	/**
	 * Release active extent.
	 */
	void  (*cio_extent_release)(const struct lu_env *env,
				    const struct cl_io_slice *slice,
				    enum cl_io_priority prio);
	/**
	 * Decide maximum read ahead extent
	 *
	 * \pre io->ci_type == CIT_READ
	 */
	int (*cio_read_ahead)(const struct lu_env *env,
			      const struct cl_io_slice *slice,
			      pgoff_t start, struct cl_read_ahead *ra);
	/**
	 *
	 * Reserve LRU slots before IO.
	 */
	int (*cio_lru_reserve)(const struct lu_env *env,
			       const struct cl_io_slice *slice,
			       loff_t pos, size_t bytes);
	/**
	 * Optional debugging helper. Print given io slice.
	 */
	int (*cio_print)(const struct lu_env *env, void *cookie,
			 lu_printer_t p, const struct cl_io_slice *slice);
};

/**
 * Flags to lock enqueue procedure.
 * \ingroup cl_lock
 */
enum cl_enq_flags {
	/**
	 * instruct server to not block, if conflicting lock is found. Instead
	 * -EAGAIN is returned immediately.
	 */
	CEF_NONBLOCK     = 0x00000001,
	/**
	 * Tell lower layers this is a glimpse request, translated to
	 * LDLM_FL_HAS_INTENT at LDLM layer.
	 *
	 * Also, because glimpse locks never block other locks, we count this
	 * as automatically compatible with other osc locks.
	 * (see osc_lock_compatible)
	 */
	CEF_GLIMPSE        = 0x00000002,
	/**
	 * tell the server to instruct (though a flag in the blocking ast) an
	 * owner of the conflicting lock, that it can drop dirty pages
	 * protected by this lock, without sending them to the server.
	 */
	CEF_DISCARD_DATA = 0x00000004,
	/**
	 * tell the sub layers that it must be a `real' lock. This is used for
	 * mmapped-buffer locks, glimpse locks, manually requested locks
	 * (LU_LADVISE_LOCKAHEAD) that must never be converted into lockless
	 * mode.
	 *
	 * \see vvp_mmap_locks(), cl_glimpse_lock, cl_request_lock().
	 */
	CEF_MUST         = 0x00000008,
	/**
	 * tell the sub layers that never request a `real' lock. This flag is
	 * not used currently.
	 *
	 * cl_io::ci_lockreq and CEF_{MUST,NEVER} flags specify lockless
	 * conversion policy: ci_lockreq describes generic information of lock
	 * requirement for this IO, especially for locks which belong to the
	 * object doing IO; however, lock itself may have precise requirements
	 * that are described by the enqueue flags.
	 */
	CEF_NEVER        = 0x00000010,
	/**
	 * tell the dlm layer this is a speculative lock request
	 * speculative lock requests are locks which are not requested as part
	 * of an I/O operation.  Instead, they are requested because we expect
	 * to use them in the future.  They are requested asynchronously at the
	 * ptlrpc layer.
	 *
	 * Currently used for asynchronous glimpse locks and manually requested
	 * locks (LU_LADVISE_LOCKAHEAD).
	 */
	CEF_SPECULATIVE          = 0x00000020,
	/**
	 * enqueue a lock to test DLM lock existence.
	 */
	CEF_PEEK	= 0x00000040,
	/**
	 * Lock match only. Used by group lock in I/O as group lock
	 * is known to exist.
	 */
	CEF_LOCK_MATCH  = 0x00000080,
	/**
	 * tell the DLM layer to lock only the requested range
	 */
	CEF_LOCK_NO_EXPAND    = 0x00000100,
	/**
	 * mask of enq_flags.
	 */
	CEF_MASK         = 0x000001ff,
};

/**
 * Link between lock and io. Intermediate structure is needed, because the
 * same lock can be part of multiple io's simultaneously.
 */
struct cl_io_lock_link {
	/** linkage into one of cl_lockset lists. */
	struct list_head        cill_linkage;
	struct cl_lock          cill_lock;
	/** optional destructor */
	void                    (*cill_fini)(const struct lu_env *env,
					     struct cl_io_lock_link *link);
};
#define cill_descr	cill_lock.cll_descr

/**
 * Lock-set represents a collection of locks, that io needs at a
 * time. Generally speaking, client tries to avoid holding multiple locks when
 * possible, because
 *
 *      - holding extent locks over multiple ost's introduces the danger of
 *        "cascading timeouts";
 *
 *      - holding multiple locks over the same ost is still dead-lock prone,
 *        see comment in osc_lock_enqueue(),
 *
 * but there are certain situations where this is unavoidable:
 *
 *      - O_APPEND writes have to take [0, EOF] lock for correctness;
 *
 *      - truncate has to take [new-size, EOF] lock for correctness;
 *
 *      - SNS has to take locks across full stripe for correctness;
 *
 *      - in the case when user level buffer, supplied to {read,write}(file0),
 *        is a part of a memory mapped lustre file, client has to take a dlm
 *        locks on file0, and all files that back up the buffer (or a part of
 *        the buffer, that is being processed in the current chunk, in any
 *        case, there are situations where at least 2 locks are necessary).
 *
 * In such cases we at least try to take locks in the same consistent
 * order. To this end, all locks are first collected, then sorted, and then
 * enqueued.
 */
struct cl_lockset {
	/** locks to be acquired. */
	struct list_head  cls_todo;
	/** locks acquired. */
	struct list_head  cls_done;
};

/**
 * Lock requirements(demand) for IO. It should be cl_io_lock_req,
 * but 'req' is always to be thought as 'request' :-)
 */
enum cl_io_lock_dmd {
	/** Always lock data (e.g., O_APPEND). */
	CILR_MANDATORY = 0,
	/** Layers are free to decide between local and global locking. */
	CILR_MAYBE,
	/** Never lock: there is no cache (e.g., liblustre). */
	CILR_NEVER
};

enum cl_fsync_mode {
	/** start writeback, do not wait for them to finish */
	CL_FSYNC_NONE		= 0,
	/** start writeback and wait for them to finish */
	CL_FSYNC_LOCAL		= 1,
	/** discard all of dirty pages in a specific file range */
	CL_FSYNC_DISCARD	= 2,
	/** start writeback and make sure they have reached storage before
	 * return. OST_SYNC RPC must be issued and finished
	 */
	CL_FSYNC_ALL		= 3,
	/** start writeback, thus the kernel can reclaim some memory */
	CL_FSYNC_RECLAIM	= 4,
};

struct cl_io_rw_common {
	loff_t	crw_pos;
	size_t	crw_bytes;
	int	crw_nonblock;
};
enum cl_setattr_subtype {
	/** regular setattr **/
	CL_SETATTR_REG = 1,
	/** truncate(2) **/
	CL_SETATTR_TRUNC,
	/** fallocate(2) - mode preallocate **/
	CL_SETATTR_FALLOCATE
};

struct cl_io_range {
	loff_t cir_pos;
	size_t cir_count;
};

struct cl_io_pt {
	struct cl_io_pt *cip_next;
	struct kiocb cip_iocb;
	struct iov_iter cip_iter;
	struct file *cip_file;
	enum cl_io_type cip_iot;
	unsigned int cip_need_restart:1;
	loff_t cip_pos;
	size_t cip_count;
	ssize_t cip_result;
};

/**
 * State for io.
 *
 * cl_io is shared by all threads participating in this IO (in current
 * implementation only one thread advances IO, but parallel IO design and
 * concurrent copy_*_user() require multiple threads acting on the same IO. It
 * is up to these threads to serialize their activities, including updates to
 * mutable cl_io fields.
 */
struct cl_io {
	/** type of this IO. Immutable after creation. */
	enum cl_io_type                ci_type;
	/** current state of cl_io state machine. */
	enum cl_io_state               ci_state;
	/** main object this io is against. Immutable after creation. */
	struct cl_object              *ci_obj;
	/** top level dio_aio */
	struct cl_dio_aio	      *ci_dio_aio;
	/**
	 * Upper layer io, of which this io is a part of. Immutable after
	 * creation.
	 */
	struct cl_io                  *ci_parent;
	/** List of slices. Immutable after creation. */
	struct list_head		ci_layers;
	/** list of locks (to be) acquired by this io. */
	struct cl_lockset              ci_lockset;
	/** lock requirements, this is just a help info for sublayers. */
	enum cl_io_lock_dmd            ci_lockreq;
	/** layout version when this IO occurs */
	__u32				ci_layout_version;
	union {
		struct cl_rd_io {
			struct cl_io_rw_common rd;
		} ci_rd;
		struct cl_wr_io {
			struct cl_io_rw_common wr;
			int                    wr_append;
			int                    wr_sync;
		} ci_wr;
		struct cl_io_rw_common ci_rw;
		struct cl_setattr_io {
			struct ost_lvb		 sa_attr;
			unsigned int		 sa_attr_flags;
			unsigned int		 sa_avalid; /* ATTR_* */
			unsigned int		 sa_xvalid; /* OP_XVALID */
			int			 sa_stripe_index;
			struct ost_layout	 sa_layout;
			const struct lu_fid	*sa_parent_fid;
			/* SETATTR interface is used for regular setattr, */
			/* truncate(2) and fallocate(2) subtypes */
			enum cl_setattr_subtype	 sa_subtype;
			/* The following are used for fallocate(2) */
			int			 sa_falloc_mode;
			loff_t			 sa_falloc_offset;
			loff_t			 sa_falloc_end;
			/* id fields used for truncate/fallocate */
			uid_t			 sa_attr_uid;
			gid_t			 sa_attr_gid;
			__u32			 sa_attr_projid;
		} ci_setattr;
		struct cl_data_version_io {
			u64 dv_data_version;
			u32 dv_layout_version;
			int dv_flags;
		} ci_data_version;
		struct cl_fault_io {
			/** page index within file. */
			pgoff_t		ft_index;
			/** bytes valid byte on a faulted page. */
			size_t		ft_bytes;
			/** writable page? for nopage() only */
			int		ft_writable;
			/** page of an executable? */
			int		ft_executable;
			/** page_mkwrite() */
			int		ft_mkwrite;
			/** resulting page */
			struct cl_page *ft_page;
		} ci_fault;
		struct cl_fsync_io {
			loff_t			 fi_start;
			loff_t			 fi_end;
			/** file system level fid */
			struct lu_fid	 	*fi_fid;
			enum cl_fsync_mode	 fi_mode;
			/* how many pages were written/discarded */
			unsigned int		 fi_nr_written;
			enum cl_io_priority	 fi_prio;
		} ci_fsync;
		struct cl_ladvise_io {
			__u64			 lio_start;
			__u64			 lio_end;
			/** file system level fid */
			struct lu_fid		*lio_fid;
			enum lu_ladvise_type	 lio_advice;
			__u64			 lio_flags;
		} ci_ladvise;
		struct cl_lseek_io {
			loff_t			 ls_start;
			loff_t			 ls_result;
			int			 ls_whence;
		} ci_lseek;
		struct cl_misc_io {
			time64_t		 lm_next_rpc_time;
		} ci_misc;
	} u;
	struct cl_2queue	ci_queue;
	size_t			ci_bytes;
	int			ci_result;
	unsigned int		ci_continue:1,
	/**
	 * This io has held grouplock, to inform sublayers that
	 * don't do lockless i/o.
	 */
			     ci_no_srvlock:1,
	/**
	 * The whole IO need to be restarted because layout has been changed
	 */
			     ci_need_restart:1,
	/**
	 * to not refresh layout - the IO issuer knows that the layout won't
	 * change(page operations, layout change causes all page to be
	 * discarded), or it doesn't matter if it changes(sync).
	 */
			     ci_ignore_layout:1,
	/**
	 * Need MDS intervention to complete a write.
	 * Write intent is required for the following cases:
	 * 1. component being written is not initialized, or
	 * 2. the mirrored files are NOT in WRITE_PENDING state.
	 */
			     ci_need_write_intent:1,
	/**
	 * File is in PCC-RO state, need MDS intervention to complete
	 * a data modifying operation.
	 */
			     ci_need_pccro_clear:1,
	/**
	 * Check if layout changed after the IO finishes. Mainly for HSM
	 * requirement. If IO occurs to openning files, it doesn't need to
	 * verify layout because HSM won't release openning files.
	 * Right now, only two opertaions need to verify layout: glimpse
	 * and setattr.
	 */
			     ci_verify_layout:1,
	/**
	 * file is released, restore has to to be triggered by vvp layer
	 */
			     ci_restore_needed:1,
	/**
	 * O_NOATIME
	 */
			     ci_noatime:1,
	/* Tell sublayers not to expand LDLM locks requested for this IO */
			     ci_lock_no_expand:1,
	/**
	 * Set if non-delay RPC should be used for this IO.
	 *
	 * If this file has multiple mirrors, and if the OSTs of the current
	 * mirror is inaccessible, non-delay RPC would error out quickly so
	 * that the upper layer can try to access the next mirror.
	 */
			     ci_ndelay:1,
	/**
	 * Set if IO is triggered by async workqueue readahead.
	 */
			     ci_async_readahead:1,
	/**
	 * Ignore lockless and do normal locking for this io.
	 */
			     ci_dio_lock:1,
	/**
	 * Set if we've tried all mirrors for this read IO, if it's not set,
	 * the read IO will check to-be-read OSCs' status, and make fast-switch
	 * another mirror if some of the OSTs are not healthy.
	 */
			     ci_tried_all_mirrors:1,
	/**
	 * Random read hints, readahead will be disabled.
	 */
			     ci_rand_read:1,
	/**
	 * Sequential read hints.
	 */
			     ci_seq_read:1,
	/**
	 * Do parallel (async) submission of DIO RPCs.  Note DIO is still sync
	 * to userspace, only the RPCs are submitted async, then waited for at
	 * the llite layer before returning.
	 */
			     ci_parallel_dio:1,
	/**
	 * this DIO is at least partly unaligned, and so the unaligned DIO
	 * path is being used for this entire IO
	 */
			     ci_unaligned_dio:1,
	/**
	 * there is an interop issue with unpatched clients/servers that
	 * exceed 4k read/write offsets with I/O exceeding LNET_MTU.
	 * This flag cleared if a target is not patched.
	 */
			     ci_allow_unaligned_dio:1,
	/**
	 * Bypass quota check
	 */
			     ci_noquota:1,
	/**
	 * io_uring direct IO with flags IOCB_NOWAIT.
	 */
			     ci_iocb_nowait:1,
	/**
	 * The filesystem must exclusively acquire invalidate_lock before
	 * invalidating page cache in truncate / hole punch / DLM extent
	 * lock blocking AST path (and thus calling into ->invalidatepage)
	 * to block races between page cache invalidation and page cache
	 * filling functions (fault, read, ...)
	 */
			     ci_invalidate_page_cache:1,
	/* was this IO switched from BIO to DIO for hybrid IO? */
			     ci_hybrid_switched:1;

	/**
	 * How many times the read has retried before this one.
	 * Set by the top level and consumed by the LOV.
	 */
	unsigned int	     ci_ndelay_tried;
	/**
	 * Designated mirror index for this I/O.
	 */
	unsigned int	     ci_designated_mirror;
	/**
	 * Number of pages owned by this IO. For invariant checking.
	 */
	unsigned int	     ci_owned_nr;
	/**
	 * Range of write intent. Valid if ci_need_write_intent is set.
	 */
	struct lu_extent     ci_write_intent;
};

/**
 * Per-transfer attributes.
 */
struct cl_req_attr {
	enum cl_req_type cra_type;
	u64		 cra_flags;
	struct cl_page  *cra_page;
	/** Generic attributes for the server consumption. */
	struct obdo	*cra_oa;
	/** process jobid/uid/gid performing the io */
	struct job_info	cra_jobinfo;
};

enum cache_stats_item {
	/** how many cache lookups were performed */
	CS_lookup = 0,
	/** how many times cache lookup resulted in a hit */
	CS_hit,
	/** how many entities are in the cache right now */
	CS_total,
	/** how many entities in the cache are actively used (and cannot be
	 * evicted) right now
	 */
	CS_busy,
	/** how many entities were created at all */
	CS_create,
	CS_NR
};

#define CS_NAMES { "lookup", "hit", "total", "busy", "create" }

/**
 * Stats for a generic cache (similar to inode, lu_object, etc. caches).
 */
struct cache_stats {
	const char	*cs_name;
	atomic_t	cs_stats[CS_NR];
};

/** These are not exported so far */
void cache_stats_init(struct cache_stats *cs, const char *name);

/**
 * Client-side site. This represents particular client stack. "Global"
 * variables should (directly or indirectly) be added here to allow multiple
 * clients to co-exist in the single address space.
 */
struct cl_site {
	struct lu_site		cs_lu;
	/**
	 * Statistical counters. Atomics do not scale, something better like
	 * per-cpu counters is needed.
	 *
	 * These are exported as /proc/fs/lustre/llite/.../site
	 *
	 * When interpreting keep in mind that both sub-locks (and sub-pages)
	 * and top-locks (and top-pages) are accounted here.
	 */
	struct cache_stats	cs_pages;
	atomic_t		cs_pages_state[CPS_NR];
};

int  cl_site_init(struct cl_site *s, struct cl_device *top);
void cl_site_fini(struct cl_site *s);

/**
 * Output client site statistical counters into a buffer. Suitable for
 * ll_rd_*()-style functions.
 */
int cl_site_stats_print(const struct cl_site *site, struct seq_file *m);

/**
 * \name helpers
 *
 * Type conversion and accessory functions.
 */

static inline struct cl_site *lu2cl_site(const struct lu_site *site)
{
	return container_of(site, struct cl_site, cs_lu);
}

static inline struct cl_device *lu2cl_dev(const struct lu_device *d)
{
	LASSERT(d == NULL || IS_ERR(d) || lu_device_is_cl(d));
	return container_of_safe(d, struct cl_device, cd_lu_dev);
}

static inline struct lu_device *cl2lu_dev(struct cl_device *d)
{
	return &d->cd_lu_dev;
}

static inline struct cl_object *lu2cl(const struct lu_object *o)
{
	LASSERT(o == NULL || IS_ERR(o) || lu_device_is_cl(o->lo_dev));
	return container_of_safe(o, struct cl_object, co_lu);
}

static inline const struct cl_object_conf *
lu2cl_conf(const struct lu_object_conf *conf)
{
	return container_of_safe(conf, struct cl_object_conf, coc_lu);
}

static inline struct cl_object *cl_object_next(const struct cl_object *obj)
{
	return obj ? lu2cl(lu_object_next(&obj->co_lu)) : NULL;
}

static inline struct cl_object_header *luh2coh(const struct lu_object_header *h)
{
	return container_of_safe(h, struct cl_object_header, coh_lu);
}

static inline struct cl_site *cl_object_site(const struct cl_object *obj)
{
	return lu2cl_site(obj->co_lu.lo_dev->ld_site);
}

static inline
struct cl_object_header *cl_object_header(const struct cl_object *obj)
{
	return luh2coh(obj->co_lu.lo_header);
}

static inline int cl_device_init(struct cl_device *d, struct lu_device_type *t)
{
	return lu_device_init(&d->cd_lu_dev, t);
}

static inline void cl_device_fini(struct cl_device *d)
{
	lu_device_fini(&d->cd_lu_dev);
}

void cl_page_slice_add(struct cl_page *page, struct cl_page_slice *slice,
		       struct cl_object *obj,
		       const struct cl_page_operations *ops);
void cl_lock_slice_add(struct cl_lock *lock, struct cl_lock_slice *slice,
		       struct cl_object *obj,
		       const struct cl_lock_operations *ops);
void cl_io_slice_add(struct cl_io *io, struct cl_io_slice *slice,
		     struct cl_object *obj, const struct cl_io_operations *ops);

struct cl_object *cl_object_top(struct cl_object *o);
struct cl_object *cl_object_find(const struct lu_env *env, struct cl_device *cd,
				 const struct lu_fid *fid,
				 const struct cl_object_conf *c);

int  cl_object_header_init(struct cl_object_header *h);
void cl_object_header_fini(struct cl_object_header *h);
void cl_object_put(const struct lu_env *env, struct cl_object *o);
void cl_object_get(struct cl_object *o);
void cl_object_attr_lock(struct cl_object *o);
void cl_object_attr_unlock(struct cl_object *o);
int  cl_object_attr_get(const struct lu_env *env, struct cl_object *obj,
			struct cl_attr *attr);
int  cl_object_attr_update(const struct lu_env *env, struct cl_object *obj,
			   const struct cl_attr *attr,
			   enum cl_attr_valid valid);
void cl_object_dirty_for_sync(const struct lu_env *env, struct cl_object *obj);
int  cl_object_glimpse(const struct lu_env *env, struct cl_object *obj,
		       struct ost_lvb *lvb);
int  cl_conf_set(const struct lu_env *env, struct cl_object *obj,
		 const struct cl_object_conf *conf);
int  cl_object_prune(const struct lu_env *env, struct cl_object *obj);
void cl_object_kill(const struct lu_env *env, struct cl_object *obj);
int cl_object_getstripe(const struct lu_env *env, struct cl_object *obj,
			struct lov_user_md __user *lum, size_t size);
int cl_object_fiemap(const struct lu_env *env, struct cl_object *obj,
		     struct ll_fiemap_info_key *fmkey, struct fiemap *fiemap,
		     size_t *buflen);
int cl_object_layout_get(const struct lu_env *env, struct cl_object *obj,
			 struct cl_layout *cl);
loff_t cl_object_maxbytes(struct cl_object *obj);
int cl_object_flush(const struct lu_env *env, struct cl_object *obj,
		    struct ldlm_lock *lock);
int cl_object_inode_ops(const struct lu_env *env, struct cl_object *obj,
			enum coo_inode_opc opc, void *data);
void cl_req_projid_set(const struct lu_env *env, struct cl_object *obj,
		       __u32 *projid);

/**
 * Returns true, iff \a o0 and \a o1 are slices of the same object.
 */
static inline int cl_object_same(struct cl_object *o0, struct cl_object *o1)
{
	return cl_object_header(o0) == cl_object_header(o1);
}

static inline void cl_object_page_init(struct cl_object *clob, int size)
{
	clob->co_slice_off = cl_object_header(clob)->coh_page_bufsize;
	cl_object_header(clob)->coh_page_bufsize += round_up(size, 8);
	WARN_ON(cl_object_header(clob)->coh_page_bufsize > 512);
}

static inline void *cl_object_page_slice(struct cl_object *clob,
					 struct cl_page *page)
{
	return (void *)((char *)page + clob->co_slice_off);
}

/**
 * Return refcount of cl_object.
 */
static inline int cl_object_refc(struct cl_object *clob)
{
	struct lu_object_header *header = clob->co_lu.lo_header;

	return atomic_read(&header->loh_ref);
}


ssize_t cl_dio_pages_init(const struct lu_env *env, struct cl_object *obj,
			  struct cl_dio_pages *cdp, struct iov_iter *iter,
			  int rw, size_t maxsize, loff_t offset,
			  bool unaligned);

/* cl_page */
struct cl_page *cl_page_find(const struct lu_env *env,
			     struct cl_object *obj,
			     pgoff_t idx, struct page *vmpage,
			     enum cl_page_type type);
struct cl_page *cl_page_alloc(const struct lu_env *env,
			      struct cl_object *o, pgoff_t ind,
			      struct page *vmpage,
			      enum cl_page_type type);
void cl_page_get(struct cl_page *page);
void cl_page_put(const struct lu_env *env,
			    struct cl_page *page);
void cl_batch_put(const struct lu_env *env, struct cl_page *page,
		  struct folio_batch *fbatch);
void cl_page_print(const struct lu_env *env, void *cookie,
		   lu_printer_t printer, const struct cl_page *pg);
void cl_page_header_print(const struct lu_env *env, void *cookie,
			  lu_printer_t printer, const struct cl_page *pg);
struct cl_page *cl_vmpage_page(struct page *vmpage, struct cl_object *obj);

/**
 * \name ownership
 *
 * Functions dealing with the ownership of page by io.
 */

int cl_page_own(const struct lu_env *env, struct cl_io *io,
		struct cl_page *page);
int cl_page_own_try(const struct lu_env *env,
		    struct cl_io *io, struct cl_page *page);
void cl_page_assume(const struct lu_env *env,
		    struct cl_io *io, struct cl_page *page);
void cl_page_unassume(const struct lu_env *env,
		      struct cl_io *io, struct cl_page *pg);
void cl_page_disown(const struct lu_env *env, struct cl_io *io,
		    struct cl_page *page);
int cl_page_is_owned(const struct cl_page *pg, const struct cl_io *io);

/**
 * \name transfer
 *
 * Functions dealing with the preparation of a page for a transfer, and
 * tracking transfer state.
 */
int cl_page_prep(const struct lu_env *env, struct cl_io *io,
		 struct cl_page *pg, enum cl_req_type crt);
void cl_dio_pages_complete(const struct lu_env *env, struct cl_dio_pages *pg,
			   int count, int ioret);
void cl_page_complete(const struct lu_env *env, struct cl_page *pg,
		      enum cl_req_type crt, int ioret);
int cl_page_make_ready(const struct lu_env *env, struct cl_page *pg,
		       enum cl_req_type crt);
void cl_page_clip(const struct lu_env *env, struct cl_page *pg, int from,
		  int to);
int cl_page_flush(const struct lu_env *env, struct cl_io *io,
		  struct cl_page *pg);

/**
 * \name helper routines
 * Functions to discard, delete and export a cl_page.
 */
void cl_page_discard(const struct lu_env *env, struct cl_io *io,
		     struct cl_page *pg);
void cl_page_delete(const struct lu_env *env, struct cl_page *pg);
void cl_page_touch(const struct lu_env *env, const struct cl_page *pg,
		   size_t to);
void cl_lock_print(const struct lu_env *env, void *cookie,
		   lu_printer_t printer, const struct cl_lock *lock);
void cl_lock_descr_print(const struct lu_env *env, void *cookie,
			 lu_printer_t printer,
			 const struct cl_lock_descr *descr);

/**
 * Data structure managing a client's cached pages. A count of
 * "unstable" pages is maintained, and an LRU of clean pages is
 * maintained. "unstable" pages are pages pinned by the ptlrpc
 * layer for recovery purposes.
 */
struct cl_client_cache {
	/**
	 * # of client cache refcount
	 * # of users (OSCs) + 2 (held by llite and lov)
	 */
	refcount_t		ccc_users;
	/**
	 * # of threads are doing shrinking
	 */
	unsigned int		ccc_lru_shrinkers;
	/**
	 * # of LRU entries available
	 */
	atomic_long_t		ccc_lru_left;
	/**
	 * # of unevictable LRU entries
	 */
	atomic_long_t		ccc_unevict_lru_used;
	/**
	 * List of entities(OSCs) for this LRU cache
	 */
	struct list_head	ccc_lru;
	/**
	 * Max # of LRU entries
	 */
	unsigned long		ccc_lru_max;
	/**
	 * Lock to protect ccc_lru list
	 */
	spinlock_t		ccc_lru_lock;
	/**
	 * Set if unstable check is enabled
	 */
	unsigned int		ccc_unstable_check:1,
	/**
	 * Whether unevictable (mlock pages) checking is enabled
	 */
				ccc_mlock_pages_enable:1;
	/**
	 * # of unstable pages for this mount point
	 */
	atomic_long_t		ccc_unstable_nr;
	/**
	 * Serialize max_cache_mb write operation
	 */
	struct mutex		ccc_max_cache_mb_lock;
};
/**
 * cl_cache functions
 */
struct cl_client_cache *cl_cache_init(unsigned long lru_page_max);
void cl_cache_incref(struct cl_client_cache *cache);
void cl_cache_decref(struct cl_client_cache *cache);

/* cl_lock */
int cl_lock_request(const struct lu_env *env, struct cl_io *io,
		    struct cl_lock *lock);
int cl_lock_init(const struct lu_env *env, struct cl_lock *lock,
		 const struct cl_io *io);
void cl_lock_fini(const struct lu_env *env, struct cl_lock *lock);
const struct cl_lock_slice *cl_lock_at(const struct cl_lock *lock,
				       const struct lu_device_type *dtype);
void cl_lock_release(const struct lu_env *env, struct cl_lock *lock);

int cl_lock_enqueue(const struct lu_env *env, struct cl_io *io,
		    struct cl_lock *lock, struct cl_sync_io *anchor);
void cl_lock_cancel(const struct lu_env *env, struct cl_lock *lock);

struct cl_dio_pages;

/* cl_io */
int   cl_io_init(const struct lu_env *env, struct cl_io *io,
		 enum cl_io_type iot, struct cl_object *obj);
int   cl_io_sub_init(const struct lu_env *env, struct cl_io *io,
		     enum cl_io_type iot, struct cl_object *obj);
int   cl_io_rw_init(const struct lu_env *env, struct cl_io *io,
		    enum cl_io_type iot, loff_t pos, size_t bytes);
int   cl_io_loop(const struct lu_env *env, struct cl_io *io);

void  cl_io_fini(const struct lu_env *env, struct cl_io *io);
int   cl_io_iter_init(const struct lu_env *env, struct cl_io *io);
void  cl_io_iter_fini(const struct lu_env *env, struct cl_io *io);
int   cl_io_lock(const struct lu_env *env, struct cl_io *io);
void  cl_io_unlock(const struct lu_env *env, struct cl_io *io);
int   cl_io_start(const struct lu_env *env, struct cl_io *io);
void  cl_io_end(const struct lu_env *env, struct cl_io *io);
int   cl_io_lock_add(const struct lu_env *env, struct cl_io *io,
		     struct cl_io_lock_link *link);
int   cl_io_lock_alloc_add(const struct lu_env *env, struct cl_io *io,
			   struct cl_lock_descr *descr);
int   cl_dio_submit_rw    (const struct lu_env *env, struct cl_io *io,
			   enum cl_req_type iot, struct cl_dio_pages *cdp);
int   cl_io_submit_rw(const struct lu_env *env, struct cl_io *io,
		      enum cl_req_type iot, struct cl_2queue *queue);
int   cl_io_submit_sync(const struct lu_env *env, struct cl_io *io,
			enum cl_req_type iot, struct cl_2queue *queue,
			long timeout);
int   cl_io_commit_async(const struct lu_env *env, struct cl_io *io,
			  struct cl_page_list *queue, int from, int to,
			  cl_commit_cbt cb, enum cl_io_priority prio);
void  cl_io_extent_release(const struct lu_env *env, struct cl_io *io,
			   enum cl_io_priority prio);
int cl_io_lru_reserve(const struct lu_env *env, struct cl_io *io,
		      loff_t pos, size_t bytes);
int   cl_io_read_ahead(const struct lu_env *env, struct cl_io *io,
		       pgoff_t start, struct cl_read_ahead *ra);
void  cl_io_rw_advance(const struct lu_env *env, struct cl_io *io,
		       size_t bytes);

/**
 * True, iff \a io is an O_APPEND write(2).
 */
static inline int cl_io_is_append(const struct cl_io *io)
{
	return io->ci_type == CIT_WRITE && io->u.ci_wr.wr_append;
}

static inline int cl_io_is_sync_write(const struct cl_io *io)
{
	return io->ci_type == CIT_WRITE && io->u.ci_wr.wr_sync;
}

static inline int cl_io_is_mkwrite(const struct cl_io *io)
{
	return io->ci_type == CIT_FAULT && io->u.ci_fault.ft_mkwrite;
}

/**
 * True, iff \a io is a truncate(2).
 */
static inline int cl_io_is_trunc(const struct cl_io *io)
{
	return io->ci_type == CIT_SETATTR &&
		(io->u.ci_setattr.sa_avalid & ATTR_SIZE) &&
		(io->u.ci_setattr.sa_subtype != CL_SETATTR_FALLOCATE);
}

static inline int cl_io_is_fallocate(const struct cl_io *io)
{
	return (io->ci_type == CIT_SETATTR) &&
	       (io->u.ci_setattr.sa_subtype == CL_SETATTR_FALLOCATE);
}

struct cl_io *cl_io_top(struct cl_io *io);

#define CL_IO_SLICE_CLEAN(obj, base) memset_startat(obj, 0, base)

/* cl_page_list */
/**
 * Last page in the page list.
 */
static inline struct cl_page *cl_page_list_last(struct cl_page_list *plist)
{
	LASSERT(plist->pl_nr > 0);
	return list_entry(plist->pl_pages.prev, struct cl_page, cp_batch);
}

static inline struct cl_page *cl_page_list_first(struct cl_page_list *plist)
{
	LASSERT(plist->pl_nr > 0);
	return list_first_entry(&plist->pl_pages, struct cl_page, cp_batch);
}

/**
 * Iterate over pages in a page list.
 */
#define cl_page_list_for_each(page, list)                               \
	list_for_each_entry((page), &(list)->pl_pages, cp_batch)

/**
 * Iterate over pages in a page list, taking possible removals into account.
 */
#define cl_page_list_for_each_safe(page, temp, list)                    \
	list_for_each_entry_safe((page), (temp), &(list)->pl_pages, cp_batch)

void cl_page_list_init(struct cl_page_list *plist);
void cl_page_list_add(struct cl_page_list *plist, struct cl_page *page,
		      bool getref);
void cl_page_list_move(struct cl_page_list *dst, struct cl_page_list *src,
		       struct cl_page *page);
void cl_page_list_move_head(struct cl_page_list *dst, struct cl_page_list *src,
			    struct cl_page *page);
void cl_page_list_splice(struct cl_page_list *list,
			 struct cl_page_list *head);
void cl_page_list_del(const struct lu_env *env,
		      struct cl_page_list *plist, struct cl_page *page,
		      bool putref);
void cl_page_list_disown(const struct lu_env *env,
			 struct cl_page_list *plist);
void cl_page_list_assume(const struct lu_env *env,
			 struct cl_io *io, struct cl_page_list *plist);
void cl_page_list_discard(const struct lu_env *env,
			  struct cl_io *io, struct cl_page_list *plist);
void cl_page_list_fini(const struct lu_env *env, struct cl_page_list *plist);

void cl_2queue_init(struct cl_2queue *queue);
void cl_2queue_disown(const struct lu_env *env, struct cl_2queue *queue);
void cl_2queue_assume(const struct lu_env *env, struct cl_io *io,
		      struct cl_2queue *queue);
void cl_2queue_discard(const struct lu_env *env, struct cl_io *io,
		       struct cl_2queue *queue);
void cl_2queue_fini(const struct lu_env *env, struct cl_2queue *queue);
void cl_2queue_init_page(struct cl_2queue *queue, struct cl_page *page);

void cl_req_attr_set(const struct lu_env *env, struct cl_object *obj,
		     struct cl_req_attr *attr);

/* cl_sync_io */
struct cl_sync_io;
struct cl_dio_aio;
struct cl_sub_dio;

typedef void (cl_sync_io_end_t)(const struct lu_env *, struct cl_sync_io *);

void cl_sync_io_init_notify(struct cl_sync_io *anchor, int nr, void *dio_aio,
			    cl_sync_io_end_t *end);

int cl_sync_io_wait(const struct lu_env *env, struct cl_sync_io *anchor,
		    long timeout);
void __cl_sync_io_note(const struct lu_env *env, struct cl_sync_io *anchor,
		       int count, int ioret);
void cl_sync_io_note(const struct lu_env *env, struct cl_sync_io *anchor,
		     int ioret);
int cl_sync_io_wait_recycle(const struct lu_env *env, struct cl_sync_io *anchor,
			    long timeout, int ioret);
void cl_dio_pages_2queue(struct cl_dio_pages *ldp);
struct cl_dio_aio *cl_dio_aio_alloc(struct kiocb *iocb, struct cl_object *obj,
				    bool is_aio);
struct cl_sub_dio *cl_sub_dio_alloc(struct cl_dio_aio *ll_aio,
				    struct iov_iter *iter, bool write,
				    bool unaligned, bool sync);
void cl_dio_aio_free(const struct lu_env *env, struct cl_dio_aio *aio);
void cl_sub_dio_free(struct cl_sub_dio *sdio);
static inline void cl_sync_io_init(struct cl_sync_io *anchor, int nr)
{
	cl_sync_io_init_notify(anchor, nr, NULL, NULL);
}

/**
 * Anchor for synchronous transfer. This is allocated on a stack by thread
 * doing synchronous transfer, and a pointer to this structure is set up in
 * every page submitted for transfer. Transfer completion routine updates
 * anchor and wakes up waiting thread when transfer is complete.
 */
struct cl_sync_io {
	/** number of pages yet to be transferred. */
	atomic_t		csi_sync_nr;
	/** has this i/o completed? */
	atomic_t		csi_complete;
	/** error code. */
	int			csi_sync_rc;
	/** completion to be signaled when transfer is complete. */
	wait_queue_head_t	csi_waitq;
	/** callback to invoke when this IO is finished */
	cl_sync_io_end_t       *csi_end_io;
	/* private pointer for an associated DIO/AIO */
	void		       *csi_dio_aio;
};

/** direct IO pages */
struct cl_dio_pages {
	/*
	 * page array for RDMA - for aligned i/o, this is the user provided
	 * pages, but for unaligned i/o, this is the internal buffer
	 */
	struct page		**cdp_pages;

	struct cl_page		**cdp_cl_pages;
	struct cl_sync_io	*cdp_sync_io;
	struct cl_2queue	cdp_queue;
	/* the file offset of the first page. */
	loff_t                  cdp_file_offset;
	unsigned int		cdp_lov_index;
	/** # of pages in the array. */
	unsigned int		cdp_page_count;
	/* the first and last page can be incomplete, this records the
	 * offsets
	 */
	int			cdp_from;
	int			cdp_to;
};

/* Top level struct used for AIO and DIO */
struct cl_dio_aio {
	struct cl_sync_io	cda_sync;
	struct cl_object	*cda_obj;
	struct kiocb		*cda_iocb;
	ssize_t			cda_bytes;
	struct mm_struct	*cda_mm;
	unsigned		cda_no_aio_complete:1,
				cda_creator_free:1,
				cda_is_aio:1;
};

struct cl_iter_dup {
	void		*id_vec;             /* dup'd vec (iov/bvec/kvec) */
	size_t		id_vec_size;         /* bytes allocated for id_vec */
};

/* Sub-dio used for splitting DIO (and AIO, because AIO is DIO) according to
 * the layout/striping, so we can do parallel submit of DIO RPCs
 */
struct cl_sub_dio {
	struct cl_sync_io	csd_sync;
	ssize_t			csd_bytes;
	struct cl_dio_aio	*csd_ll_aio;
	struct cl_dio_pages	csd_dio_pages;
	struct iov_iter		csd_iter;
	struct cl_iter_dup	csd_dup;
	spinlock_t		csd_lock;
	unsigned		csd_creator_free:1,
				csd_write:1,
				csd_unaligned:1,
				csd_write_copied:1;
};

static inline u64 cl_io_nob_aligned(u64 off, u32 nob, u32 pgsz)
{
	return (((nob / pgsz) - 1) * pgsz) + (pgsz - (off & (pgsz - 1)));
}

void ll_release_user_pages(struct page **pages, int npages);
int ll_allocate_dio_buffer(struct cl_dio_pages *cdp, size_t io_size);
void ll_free_dio_buffer(struct cl_dio_pages *cdp);
ssize_t ll_dio_user_copy(struct cl_sub_dio *sdio);

#ifndef HAVE_KTHREAD_USE_MM
#define kthread_use_mm(mm) use_mm(mm)
#define kthread_unuse_mm(mm) unuse_mm(mm)
#endif

/** \defgroup cl_env cl_env
 *
 * lu_env handling for a client.
 *
 * lu_env is an environment within which lustre code executes. Its major part
 * is lu_context---a fast memory allocation mechanism that is used to conserve
 * precious kernel stack space. Originally lu_env was designed for a server,
 * where
 *
 *     - there is a (mostly) fixed number of threads, and
 *
 *     - call chains have no non-lustre portions inserted between lustre code.
 *
 * On a client both these assumtpion fails, because every user thread can
 * potentially execute lustre code as part of a system call, and lustre calls
 * into VFS or MM that call back into lustre.
 *
 * To deal with that, cl_env wrapper functions implement the following
 * optimizations:
 *
 *     - allocation and destruction of environment is amortized by caching no
 *     longer used environments instead of destroying them;
 *
 * \see lu_env, lu_context, lu_context_key
 */

/* cl_env */
struct lu_env *cl_env_get(__u16 *refcheck);
struct lu_env *cl_env_alloc(__u16 *refcheck, __u32 tags);
void cl_env_put(struct lu_env *env, __u16 *refcheck);
unsigned int cl_env_cache_purge(unsigned int nr);
struct lu_env *cl_env_percpu_get(void);
void cl_env_percpu_put(struct lu_env *env);


/*
 * Misc
 */
void cl_attr2lvb(struct ost_lvb *lvb, const struct cl_attr *attr);
void cl_lvb2attr(struct cl_attr *attr, const struct ost_lvb *lvb);

struct cl_device *cl_type_setup(const struct lu_env *env, struct lu_site *site,
				struct lu_device_type *ldt,
				struct lu_device *next);

int cl_global_init(void);
void cl_global_fini(void);

int lov_read_and_clear_async_rc(struct cl_object *clob);

#endif /* _LINUX_CL_OBJECT_H */
