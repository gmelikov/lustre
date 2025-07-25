// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 *
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Client IO.
 *
 * Author: Nikita Danilov <nikita.danilov@sun.com>
 * Author: Jinshan Xiong <jinshan.xiong@intel.com>
 *
 */

#define DEBUG_SUBSYSTEM S_CLASS

#include <linux/sched.h>
#include <linux/list.h>
#include <linux/list_sort.h>
#include <linux/mmu_context.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre_fid.h>
#include <cl_object.h>
#include "cl_internal.h"

/*
 * cl_io interface.
 */

static inline int cl_io_type_is_valid(enum cl_io_type type)
{
	return CIT_READ <= type && type < CIT_OP_NR;
}

static inline int cl_io_is_loopable(const struct cl_io *io)
{
	return cl_io_type_is_valid(io->ci_type) && io->ci_type != CIT_MISC;
}

/*
 * cl_io invariant that holds at all times when exported cl_io_*() functions
 * are entered and left.
 */
static inline int cl_io_invariant(const struct cl_io *io)
{
	/*
	 * io can own pages only when it is ongoing. Sub-io might
	 * still be in CIS_LOCKED state when top-io is in
	 * CIS_IO_GOING.
	 */
	return ergo(io->ci_owned_nr > 0, io->ci_state == CIS_IO_GOING ||
		    (io->ci_state == CIS_LOCKED && io->ci_parent != NULL));
}

/*
 * Finalize @io, by calling cl_io_operations::cio_fini() bottom-to-top.
 */
void cl_io_fini(const struct lu_env *env, struct cl_io *io)
{
	struct cl_io_slice    *slice;

	LINVRNT(cl_io_type_is_valid(io->ci_type));
	LINVRNT(cl_io_invariant(io));
	ENTRY;

	while (!list_empty(&io->ci_layers)) {
		slice = container_of(io->ci_layers.prev, struct cl_io_slice,
				     cis_linkage);
		list_del_init(&slice->cis_linkage);
		if (slice->cis_iop->op[io->ci_type].cio_fini != NULL)
			slice->cis_iop->op[io->ci_type].cio_fini(env, slice);
		/*
		 * Invalidate slice to catch use after free. This assumes that
		 * slices are allocated within session and can be touched
		 * after ->cio_fini() returns.
		 */
		slice->cis_io = NULL;
	}
	io->ci_state = CIS_FINI;

	/* sanity check for layout change */
	switch(io->ci_type) {
	case CIT_READ:
	case CIT_WRITE:
	case CIT_DATA_VERSION:
	case CIT_FAULT:
		break;
	case CIT_FSYNC:
		LASSERT(!io->ci_need_restart);
		break;
	case CIT_SETATTR:
	case CIT_MISC:
		/* Check ignore layout change conf */
		LASSERT(ergo(io->ci_ignore_layout || !io->ci_verify_layout,
				!io->ci_need_restart));
		break;
	case CIT_GLIMPSE:
		break;
	case CIT_LADVISE:
	case CIT_LSEEK:
		break;
	default:
		LBUG();
	}
	EXIT;
}
EXPORT_SYMBOL(cl_io_fini);

static int __cl_io_init(const struct lu_env *env, struct cl_io *io,
			enum cl_io_type iot, struct cl_object *obj)
{
	struct cl_object *scan;
	int result;

	LINVRNT(io->ci_state == CIS_ZERO || io->ci_state == CIS_FINI);
	LINVRNT(cl_io_type_is_valid(iot));
	LINVRNT(cl_io_invariant(io));
	ENTRY;

	io->ci_type = iot;
	INIT_LIST_HEAD(&io->ci_lockset.cls_todo);
	INIT_LIST_HEAD(&io->ci_lockset.cls_done);
	INIT_LIST_HEAD(&io->ci_layers);

	result = 0;
	cl_object_for_each(scan, obj) {
		if (scan->co_ops->coo_io_init != NULL) {
			result = scan->co_ops->coo_io_init(env, scan, io);
			if (result != 0)
				break;
		}
	}
	if (result == 0)
		io->ci_state = CIS_INIT;
	RETURN(result);
}

/**
 * cl_io_sub_init() - Initialize sub-IO (cl_io_operations::cio_init()
 * top-to-bottom)
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @iot: Requested transfer type (READ / WRITE)
 * @obj: Sub object linked to IO operation (obj != cl_object_top(obj)).
 *
 * Return:
 * * %0: Success
 * * %-ERRNO: Failure
 */
int cl_io_sub_init(const struct lu_env *env, struct cl_io *io,
		   enum cl_io_type iot, struct cl_object *obj)
{
	LASSERT(obj != cl_object_top(obj));

	return __cl_io_init(env, io, iot, obj);
}
EXPORT_SYMBOL(cl_io_sub_init);

/**
 * cl_io_init() - Initialize @io (cl_io_operations::cio_init() top-to-bottom)
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @iot: Requested transfer type (READ / WRITE)
 * @obj: Object linked to IO operation (obj == cl_object_top(obj))
 *
 * Caller has to call cl_io_fini() after a call to cl_io_init(), no matter
 * what the latter returned.
 *
 * Return:
 * * %0: Success
 * * %-ERRNO: Failure
 */
int cl_io_init(const struct lu_env *env, struct cl_io *io,
	       enum cl_io_type iot, struct cl_object *obj)
{
	LASSERT(obj == cl_object_top(obj));

	/* clear I/O restart from previous instance */
	io->ci_need_restart = 0;

	return __cl_io_init(env, io, iot, obj);
}
EXPORT_SYMBOL(cl_io_init);

/**
 * cl_io_rw_init() - Initialize read or write IO
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @iot: Requested transfer type (READ / WRITE)
 * @pos: IO start position
 * @bytes: Number of bytes to Read/Write depending on @iot
 *
 * * Return:
 * * %0: Success
 * * %-ERRNO: Failure
 */
int cl_io_rw_init(const struct lu_env *env, struct cl_io *io,
		  enum cl_io_type iot, loff_t pos, size_t bytes)
{
	LINVRNT(iot == CIT_READ || iot == CIT_WRITE);
	LINVRNT(io->ci_obj != NULL);
	ENTRY;

	LU_OBJECT_HEADER(D_VFSTRACE, env, &io->ci_obj->co_lu,
			 "io range: %u [%llu, %llu) %u %u\n",
			 iot, (__u64)pos, (__u64)pos + bytes,
			 io->u.ci_rw.crw_nonblock, io->u.ci_wr.wr_append);
	io->u.ci_rw.crw_pos    = pos;
	io->u.ci_rw.crw_bytes  = bytes;
	RETURN(cl_io_init(env, io, iot, io->ci_obj));
}
EXPORT_SYMBOL(cl_io_rw_init);

#ifdef HAVE_LIST_CMP_FUNC_T
static int cl_lock_descr_cmp(void *priv,
			     const struct list_head *a,
			     const struct list_head *b)
#else /* !HAVE_LIST_CMP_FUNC_T */
static int cl_lock_descr_cmp(void *priv,
			     struct list_head *a, struct list_head *b)
#endif /* HAVE_LIST_CMP_FUNC_T */
{
	const struct cl_io_lock_link *l0 = list_entry(a, struct cl_io_lock_link,
						      cill_linkage);
	const struct cl_io_lock_link *l1 = list_entry(b, struct cl_io_lock_link,
						      cill_linkage);
	const struct cl_lock_descr *d0 = &l0->cill_descr;
	const struct cl_lock_descr *d1 = &l1->cill_descr;

	return lu_fid_cmp(lu_object_fid(&d0->cld_obj->co_lu),
			  lu_object_fid(&d1->cld_obj->co_lu));
}

static void cl_lock_descr_merge(struct cl_lock_descr *d0,
				const struct cl_lock_descr *d1)
{
	d0->cld_start = min(d0->cld_start, d1->cld_start);
	d0->cld_end = max(d0->cld_end, d1->cld_end);

	if (d1->cld_mode == CLM_WRITE && d0->cld_mode != CLM_WRITE)
		d0->cld_mode = CLM_WRITE;

	if (d1->cld_mode == CLM_GROUP && d0->cld_mode != CLM_GROUP)
		d0->cld_mode = CLM_GROUP;
}

static int cl_lockset_merge(const struct cl_lockset *set,
			    const struct cl_lock_descr *need)
{
	struct cl_io_lock_link *scan;

	ENTRY;
	list_for_each_entry(scan, &set->cls_todo, cill_linkage) {
		if (!cl_object_same(scan->cill_descr.cld_obj, need->cld_obj))
			continue;

		/* Merge locks for the same object because ldlm lock server
		 * may expand the lock extent, otherwise there is a deadlock
		 * case if two conflicted locks are queueud for the same object
		 * and lock server expands one lock to overlap the another.
		 * The side effect is that it can generate a multi-stripe lock
		 * that may cause casacading problem */
		cl_lock_descr_merge(&scan->cill_descr, need);
		CDEBUG(D_VFSTRACE, "lock: %d: [%lu, %lu]\n",
		       scan->cill_descr.cld_mode, scan->cill_descr.cld_start,
		       scan->cill_descr.cld_end);
		RETURN(+1);
	}
	RETURN(0);
}

static int cl_lockset_lock(const struct lu_env *env, struct cl_io *io,
			   struct cl_lockset *set)
{
	struct cl_io_lock_link *link;
	struct cl_io_lock_link *temp;
	int result;

	ENTRY;
	result = 0;
	list_for_each_entry_safe(link, temp, &set->cls_todo, cill_linkage) {
		result = cl_lock_request(env, io, &link->cill_lock);
		if (result < 0)
			break;

		list_move(&link->cill_linkage, &set->cls_done);
	}
	RETURN(result);
}

/*
 * Takes locks necessary for the current iteration of IO.
 *
 * Calls cl_io_operations::cio_lock() top-to-bottom to collect locks required
 * by layers for the current iteration. Then sort locks (to avoid dead-locks),
 * and acquire them.
 */
int cl_io_lock(const struct lu_env *env, struct cl_io *io)
{
	const struct cl_io_slice *scan;
	int result = 0;

	LINVRNT(cl_io_is_loopable(io));
	LINVRNT(io->ci_state == CIS_IT_STARTED);
	LINVRNT(cl_io_invariant(io));

	ENTRY;
	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_lock == NULL)
			continue;
		result = scan->cis_iop->op[io->ci_type].cio_lock(env, scan);
		if (result != 0)
			break;
	}
	if (result == 0) {
		/*
		 * Sort locks in lexicographical order of their (fid,
		 * start-offset) pairs to avoid deadlocks.
		 */
		list_sort(NULL, &io->ci_lockset.cls_todo, cl_lock_descr_cmp);
		result = cl_lockset_lock(env, io, &io->ci_lockset);
	}
	if (result != 0)
		cl_io_unlock(env, io);
	else
		io->ci_state = CIS_LOCKED;
	RETURN(result);
}
EXPORT_SYMBOL(cl_io_lock);

/*
 * Release locks takes by IO
 */
void cl_io_unlock(const struct lu_env *env, struct cl_io *io)
{
	struct cl_lockset *set;
	struct cl_io_lock_link *link;
	struct cl_io_lock_link *temp;
	const struct cl_io_slice *scan;

	LASSERT(cl_io_is_loopable(io));
	LASSERT(CIS_IT_STARTED <= io->ci_state && io->ci_state < CIS_UNLOCKED);
	LINVRNT(cl_io_invariant(io));

	ENTRY;
	set = &io->ci_lockset;

	list_for_each_entry_safe(link, temp, &set->cls_todo, cill_linkage) {
		list_del_init(&link->cill_linkage);
		if (link->cill_fini != NULL)
			link->cill_fini(env, link);
	}

	list_for_each_entry_safe(link, temp, &set->cls_done, cill_linkage) {
		list_del_init(&link->cill_linkage);
		cl_lock_release(env, &link->cill_lock);
		if (link->cill_fini != NULL)
			link->cill_fini(env, link);
	}

	list_for_each_entry_reverse(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_unlock != NULL)
			scan->cis_iop->op[io->ci_type].cio_unlock(env, scan);
	}
	io->ci_state = CIS_UNLOCKED;
	EXIT;
}
EXPORT_SYMBOL(cl_io_unlock);

/**
 * cl_io_iter_init() - Prepares next iteration of IO.
 * @env: execution environment
 * @io: pointer to a cl_io struct
 *
 * Calls cl_io_operations::cio_iter_init() top-to-bottom. This exists to give
 * layers a chance to modify @io parameters, e.g., so that lov can restrict @io
 * to a single stripe.
 *
 * * Return:
 * * %0: successfully initialized
 * * %-ERRNO: failure
 */
int cl_io_iter_init(const struct lu_env *env, struct cl_io *io)
{
	const struct cl_io_slice *scan;
	int result;

	LINVRNT(cl_io_is_loopable(io));
	LINVRNT(io->ci_state == CIS_INIT || io->ci_state == CIS_IT_ENDED);
	LINVRNT(cl_io_invariant(io));

	ENTRY;
	result = 0;
	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_iter_init == NULL)
			continue;
		result = scan->cis_iop->op[io->ci_type].cio_iter_init(env,
								      scan);
		if (result != 0)
			break;
	}
	if (result == 0)
		io->ci_state = CIS_IT_STARTED;
	RETURN(result);
}
EXPORT_SYMBOL(cl_io_iter_init);

/*
 * Finalizes IO iteration.
 *
 * Calls cl_io_operations::cio_iter_fini() bottom-to-top.
 */
void cl_io_iter_fini(const struct lu_env *env, struct cl_io *io)
{
	const struct cl_io_slice *scan;

	LINVRNT(cl_io_is_loopable(io));
	LINVRNT(io->ci_state <= CIS_IT_STARTED ||
		io->ci_state > CIS_IO_FINISHED);
	LINVRNT(cl_io_invariant(io));

	ENTRY;
	list_for_each_entry_reverse(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_iter_fini != NULL)
			scan->cis_iop->op[io->ci_type].cio_iter_fini(env, scan);
	}
	io->ci_state = CIS_IT_ENDED;
	EXIT;
}
EXPORT_SYMBOL(cl_io_iter_fini);

/**
 * cl_io_rw_advance() - Records read/write @io progressed @bytes forward
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @bytes: Number of bytes IO should advance
 */
void cl_io_rw_advance(const struct lu_env *env, struct cl_io *io, size_t bytes)
{
	const struct cl_io_slice *scan;

	ENTRY;

	LINVRNT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE ||
		bytes == 0);
	LINVRNT(cl_io_is_loopable(io));
	LINVRNT(cl_io_invariant(io));

	io->u.ci_rw.crw_pos   += bytes;
	io->u.ci_rw.crw_bytes -= bytes;

	/* layers have to be notified. */
	list_for_each_entry_reverse(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_advance != NULL)
			scan->cis_iop->op[io->ci_type].cio_advance(env, scan,
								   bytes);
	}
	EXIT;
}

/*
 * Adds a lock to a lockset.
 */
int cl_io_lock_add(const struct lu_env *env, struct cl_io *io,
		   struct cl_io_lock_link *link)
{
	int result;

	ENTRY;
	if (cl_lockset_merge(&io->ci_lockset, &link->cill_descr))
		result = +1;
	else {
		list_add(&link->cill_linkage, &io->ci_lockset.cls_todo);
		result = 0;
	}
	RETURN(result);
}
EXPORT_SYMBOL(cl_io_lock_add);

static void cl_free_io_lock_link(const struct lu_env *env,
				 struct cl_io_lock_link *link)
{
	OBD_FREE_PTR(link);
}

/*
 * Allocates new lock link, and uses it to add a lock to a lockset.
 */
int cl_io_lock_alloc_add(const struct lu_env *env, struct cl_io *io,
			 struct cl_lock_descr *descr)
{
	struct cl_io_lock_link *link;
	int result;

	ENTRY;
	OBD_ALLOC_PTR(link);
	if (link != NULL) {
		link->cill_descr = *descr;
		link->cill_fini	 = cl_free_io_lock_link;
		result = cl_io_lock_add(env, io, link);
		if (result) /* lock match */
			link->cill_fini(env, link);
	} else
		result = -ENOMEM;

	RETURN(result);
}
EXPORT_SYMBOL(cl_io_lock_alloc_add);

/*
 * Starts IO by calling cl_io_operations::cio_start() top-to-bottom.
 */
int cl_io_start(const struct lu_env *env, struct cl_io *io)
{
	const struct cl_io_slice *scan;
	int result = 0;

	LINVRNT(cl_io_is_loopable(io));
	LINVRNT(io->ci_state == CIS_LOCKED);
	LINVRNT(cl_io_invariant(io));
	ENTRY;

	io->ci_state = CIS_IO_GOING;
	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_start == NULL)
			continue;
		result = scan->cis_iop->op[io->ci_type].cio_start(env, scan);
		if (result != 0)
			break;
	}
	if (result >= 0)
		result = 0;
	RETURN(result);
}
EXPORT_SYMBOL(cl_io_start);

/*
 * Wait until current IO iteration is finished by calling
 * cl_io_operations::cio_end() bottom-to-top.
 */
void cl_io_end(const struct lu_env *env, struct cl_io *io)
{
	const struct cl_io_slice *scan;

	LINVRNT(cl_io_is_loopable(io));
	LINVRNT(io->ci_state == CIS_IO_GOING);
	LINVRNT(cl_io_invariant(io));
	ENTRY;

	list_for_each_entry_reverse(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->op[io->ci_type].cio_end != NULL)
			scan->cis_iop->op[io->ci_type].cio_end(env, scan);
		/* TODO: error handling. */
	}
	io->ci_state = CIS_IO_FINISHED;
	EXIT;
}
EXPORT_SYMBOL(cl_io_end);

/*
 * Called by read IO, to decide the readahead extent
 *
 * see cl_io_operations::cio_read_ahead()
 */
int cl_io_read_ahead(const struct lu_env *env, struct cl_io *io,
		     pgoff_t start, struct cl_read_ahead *ra)
{
	const struct cl_io_slice *scan;
	int result = 0;

	LINVRNT(io->ci_type == CIT_READ ||
		io->ci_type == CIT_FAULT ||
		io->ci_type == CIT_WRITE);
	LINVRNT(io->ci_state == CIS_IO_GOING || io->ci_state == CIS_LOCKED);
	LINVRNT(cl_io_invariant(io));
	ENTRY;

	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->cio_read_ahead == NULL)
			continue;

		result = scan->cis_iop->cio_read_ahead(env, scan, start, ra);
		if (result != 0)
			break;
	}
	RETURN(result > 0 ? 0 : result);
}
EXPORT_SYMBOL(cl_io_read_ahead);

/*
 * Called before IO start, to reserve enough LRU slots to avoid
 * deadlock.
 *
 * \see cl_io_operations::cio_lru_reserve()
 */
int cl_io_lru_reserve(const struct lu_env *env, struct cl_io *io,
		      loff_t pos, size_t bytes)
{
	const struct cl_io_slice *scan;
	int result = 0;

	LINVRNT(io->ci_type == CIT_READ || io->ci_type == CIT_WRITE);
	LINVRNT(cl_io_invariant(io));
	ENTRY;

	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->cio_lru_reserve) {
			result = scan->cis_iop->cio_lru_reserve(env, scan,
								pos, bytes);
			if (result)
				break;
		}
	}

	RETURN(result);
}
EXPORT_SYMBOL(cl_io_lru_reserve);

/**
 * cl_io_commit_async() - Commit a list of contiguous pages into writeback cache
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @queue: pointer to cl_page_list struct (pages that will be committed)
 * @from: Starting position
 * @to: Ending position
 * @cb: callback function
 * @prio: I/O priority
 *
 * Returns 0 if all pages committed, or errcode if error occurred.
 * see cl_io_operations::cio_commit_async()
 */
int cl_io_commit_async(const struct lu_env *env, struct cl_io *io,
		       struct cl_page_list *queue, int from, int to,
		       cl_commit_cbt cb, enum cl_io_priority prio)
{
	const struct cl_io_slice *scan;
	int result = 0;
	ENTRY;

	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->cio_commit_async == NULL)
			continue;
		result = scan->cis_iop->cio_commit_async(env, scan, queue,
							 from, to, cb, prio);
		if (result != 0)
			break;
	}
	RETURN(result);
}
EXPORT_SYMBOL(cl_io_commit_async);

void cl_io_extent_release(const struct lu_env *env, struct cl_io *io,
			  enum cl_io_priority prio)
{
	const struct cl_io_slice *scan;
	ENTRY;

	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->cio_extent_release == NULL)
			continue;
		scan->cis_iop->cio_extent_release(env, scan, prio);
	}
	EXIT;
}
EXPORT_SYMBOL(cl_io_extent_release);

int cl_dio_submit_rw(const struct lu_env *env, struct cl_io *io,
		     enum cl_req_type crt, struct cl_dio_pages *cdp)
{
	const struct cl_io_slice *scan;
	int result = 0;

	ENTRY;

	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->cio_dio_submit == NULL)
			continue;
		result = scan->cis_iop->cio_dio_submit(env, io, scan, crt,
						       cdp);
		if (result != 0)
			break;
	}
	RETURN(result);
}
EXPORT_SYMBOL(cl_dio_submit_rw);

/**
 * cl_io_submit_rw() - Submits a list of pages for immediate IO.
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @crt: Requested transfer type (READ / WRITE)
 * @queue: pointer to a cl_2queue struct (pages in IO operation)
 *
 * After the function gets returned, The submitted pages are moved to
 * queue->c2_qout queue, and queue->c2_qin contain both the pages don't need
 * to be submitted, and the pages are errant to submit.
 *
 * Returns 0 if at least one page was submitted, error code otherwise.
 * (see cl_io_operations::cio_submit())
 */
int cl_io_submit_rw(const struct lu_env *env, struct cl_io *io,
		    enum cl_req_type crt, struct cl_2queue *queue)
{
	const struct cl_io_slice *scan;
	int result = 0;
	ENTRY;

	list_for_each_entry(scan, &io->ci_layers, cis_linkage) {
		if (scan->cis_iop->cio_submit == NULL)
			continue;
		result = scan->cis_iop->cio_submit(env, io, scan, crt, queue);
		if (result != 0)
			break;
	}
	/*
	 * If ->cio_submit() failed, no pages were sent.
	 */
	LASSERT(ergo(result != 0, list_empty(&queue->c2_qout.pl_pages)));
	RETURN(result);
}
EXPORT_SYMBOL(cl_io_submit_rw);

/**
 * cl_io_submit_sync() - submit a sync_io and wait for the IO to be finished,
 * or error happens. If @timeout is zero, it means to wait for the IO
 * unconditionally.
 * @env: execution environment
 * @io: pointer to a cl_io struct
 * @iot: Requested transfer type (READ / WRITE)
 * @queue: pointer to a cl_2queue struct (pages in IO operation)
 * @timeout: Time in seconds to wait for IO completion.
 *
 * This is used for synchronous submission of an async IO, so the waiting is
 * done here in this function and the IO is done when this function returns.
 *
 * Return:
 * * %0: Success
 * * %-ERRNO: Failure
 */
int cl_io_submit_sync(const struct lu_env *env, struct cl_io *io,
		      enum cl_req_type iot, struct cl_2queue *queue,
		      long timeout)
{
	struct cl_sync_io *anchor = &cl_env_info(env)->clt_anchor;
	struct cl_page *pg;
	int rc;
	ENTRY;

	cl_page_list_for_each(pg, &queue->c2_qin) {
		LASSERT(pg->cp_sync_io == NULL);
		/* this is for sync submission of async IO, IO that was always
		 * sync (like DIO) is handled differently
		 */
		LASSERT(pg->cp_type != CPT_TRANSIENT);
		pg->cp_sync_io = anchor;
	}

	cl_sync_io_init(anchor, queue->c2_qin.pl_nr);
	rc = cl_io_submit_rw(env, io, iot, queue);
	if (rc == 0) {
		/*
		 * If some pages weren't sent for any reason (e.g.,
		 * read found up-to-date pages in the cache, or write found
		 * clean pages), count them as completed to avoid infinite
		 * wait.
		 */
		cl_page_list_for_each(pg, &queue->c2_qin) {
			pg->cp_sync_io = NULL;
			cl_sync_io_note(env, anchor, 1);
		}

		/* wait for the IO to be finished. */
		rc = cl_sync_io_wait(env, anchor, timeout);
		cl_page_list_assume(env, io, &queue->c2_qout);
	} else {
		LASSERT(list_empty(&queue->c2_qout.pl_pages));
		cl_page_list_for_each(pg, &queue->c2_qin)
			pg->cp_sync_io = NULL;
	}
	RETURN(rc);
}
EXPORT_SYMBOL(cl_io_submit_sync);

/**
 * cl_io_loop() - Main IO loop.
 * @env: thread environment in which the I/O operation is executed
 * @io: pointer to a cl_io struct
 *
 * Pumps IO through iterations calling
 *
 *    - cl_io_iter_init()
 *
 *    - cl_io_lock()
 *
 *    - cl_io_start()
 *
 *    - cl_io_end()
 *
 *    - cl_io_unlock()
 *
 *    - cl_io_iter_fini()
 *
 * repeatedly until there is no more @io to do.
 *
 * Return:
 * * %0: IO was success
 * * %-ERRNO: Failure
 */
int cl_io_loop(const struct lu_env *env, struct cl_io *io)
{
	int result = 0;
	int rc = 0;

	LINVRNT(cl_io_is_loopable(io));
	ENTRY;

	do {
		size_t bytes;

		io->ci_continue = 0;
		result = cl_io_iter_init(env, io);
		if (result == 0) {
			bytes = io->ci_bytes;
			result = cl_io_lock(env, io);
			if (result == 0) {
				/*
				 * Notify layers that locks has been taken,
				 * and do actual i/o.
				 *
				 *   - llite: kms, short read;
				 *   - llite: generic_file_read();
				 */
				result = cl_io_start(env, io);
				/*
				 * Send any remaining pending
				 * io, etc.
				 *
				 **   - llite: ll_rw_stats_tally.
				 */
				cl_io_end(env, io);
				cl_io_unlock(env, io);
				cl_io_rw_advance(env, io, io->ci_bytes - bytes);
			}
		}
		cl_io_iter_fini(env, io);
		if (result)
			rc = result;
	} while ((result == 0 || result == -EIOCBQUEUED) &&
		 io->ci_continue);

	if (rc && !result)
		result = rc;

	if (result == -EAGAIN && io->ci_ndelay && !io->ci_iocb_nowait) {
		if (!io->ci_tried_all_mirrors) {
			io->ci_need_restart = 1;
			result = 0;
		} else {
			result = -EIO;
		}
	}

	if (result == 0)
		result = io->ci_result;
	RETURN(result < 0 ? result : 0);
}
EXPORT_SYMBOL(cl_io_loop);

/**
 * cl_io_slice_add() - Adds IO slice to the cl_io.
 * @io: pointer to a cl_io struct
 * @slice: pointer to the cl_io_slice struct (Part of IO operation)
 * @obj: Object linked to IO operation
 * @ops: operations/methods applicable on @slice
 *
 * This is called by cl_object_operations::coo_io_init() methods to add a
 * per-layer state to the @io. New state is added at the end of
 * cl_io::ci_layers list, that is, it is at the bottom of the stack.
 *
 * see cl_lock_slice_add(), cl_req_slice_add(), cl_page_slice_add()
 */
void cl_io_slice_add(struct cl_io *io, struct cl_io_slice *slice,
		     struct cl_object *obj,
		     const struct cl_io_operations *ops)
{
	struct list_head *linkage = &slice->cis_linkage;

	LASSERT((linkage->prev == NULL && linkage->next == NULL) ||
		list_empty(linkage));
	ENTRY;

	list_add_tail(linkage, &io->ci_layers);
	slice->cis_io  = io;
	slice->cis_obj = obj;
	slice->cis_iop = ops;
	EXIT;
}
EXPORT_SYMBOL(cl_io_slice_add);


/*
 * Initializes page list.
 */
void cl_page_list_init(struct cl_page_list *plist)
{
	ENTRY;
	plist->pl_nr = 0;
	INIT_LIST_HEAD(&plist->pl_pages);
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_init);

/*
 * Adds a page to a page list.
 */
void cl_page_list_add(struct cl_page_list *plist, struct cl_page *page,
		      bool getref)
{
	ENTRY;
	/* it would be better to check that page is owned by "current" io, but
	 * it is not passed here. */
	if (page->cp_type != CPT_TRANSIENT)
		LASSERT(page->cp_owner != NULL);

	LASSERT(list_empty(&page->cp_batch));
	list_add_tail(&page->cp_batch, &plist->pl_pages);
	++plist->pl_nr;
	if (getref)
		cl_page_get(page);
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_add);

/*
 * Removes a page from a page list.
 */
void cl_page_list_del(const struct lu_env *env,
		      struct cl_page_list *plist, struct cl_page *page,
		      bool putref)
{
	LASSERT(plist->pl_nr > 0);

	ENTRY;
	list_del_init(&page->cp_batch);
	--plist->pl_nr;
	if (putref)
		cl_page_put(env, page);
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_del);

/*
 * Moves a page from one page list to another.
 */
void cl_page_list_move(struct cl_page_list *dst, struct cl_page_list *src,
		       struct cl_page *page)
{
	LASSERT(src->pl_nr > 0);

	ENTRY;
	list_move_tail(&page->cp_batch, &dst->pl_pages);
	--src->pl_nr;
	++dst->pl_nr;
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_move);

/*
 * Moves a page from one page list to the head of another list.
 */
void cl_page_list_move_head(struct cl_page_list *dst, struct cl_page_list *src,
			    struct cl_page *page)
{
	LASSERT(src->pl_nr > 0);

	ENTRY;
	list_move(&page->cp_batch, &dst->pl_pages);
	--src->pl_nr;
	++dst->pl_nr;
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_move_head);

/*
 * splice the cl_page_list, just as list head does
 */
void cl_page_list_splice(struct cl_page_list *src, struct cl_page_list *dst)
{
	ENTRY;
	dst->pl_nr += src->pl_nr;
	src->pl_nr = 0;
	list_splice_tail_init(&src->pl_pages, &dst->pl_pages);

	EXIT;
}
EXPORT_SYMBOL(cl_page_list_splice);

/*
 * Disowns pages in a queue.
 */
void cl_page_list_disown(const struct lu_env *env, struct cl_page_list *plist)
{
	struct cl_page *page;
	struct cl_page *temp;

	ENTRY;
	cl_page_list_for_each_safe(page, temp, plist) {
		LASSERT(plist->pl_nr > 0);

		list_del_init(&page->cp_batch);
		--plist->pl_nr;
		/*
		 * __cl_page_disown rather than usual cl_page_disown() is used,
		 * because pages are possibly in CPS_FREEING state already due
		 * to the call to cl_page_list_discard().
		 */
		/*
		 * XXX __cl_page_disown() will fail if page is not locked.
		 */
		__cl_page_disown(env, page);
		cl_page_put(env, page);
	}
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_disown);

/*
 * Releases pages from queue.
 */
void cl_page_list_fini(const struct lu_env *env, struct cl_page_list *plist)
{
	struct cl_page *page;
	struct cl_page *temp;

	ENTRY;
	cl_page_list_for_each_safe(page, temp, plist)
		cl_page_list_del(env, plist, page, true);
	LASSERT(plist->pl_nr == 0);
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_fini);

/*
 * Assumes all pages in a queue.
 */
void cl_page_list_assume(const struct lu_env *env,
			 struct cl_io *io, struct cl_page_list *plist)
{
	struct cl_page *page;

	cl_page_list_for_each(page, plist)
		cl_page_assume(env, io, page);
}

/*
 * Discards all pages in a queue.
 */
void cl_page_list_discard(const struct lu_env *env, struct cl_io *io,
			  struct cl_page_list *plist)
{
	struct cl_page *page;

	ENTRY;
	cl_page_list_for_each(page, plist)
		cl_page_discard(env, io, page);
	EXIT;
}
EXPORT_SYMBOL(cl_page_list_discard);

/*
 * Initialize dual page queue.
 */
void cl_2queue_init(struct cl_2queue *queue)
{
	ENTRY;
	cl_page_list_init(&queue->c2_qin);
	cl_page_list_init(&queue->c2_qout);
	EXIT;
}
EXPORT_SYMBOL(cl_2queue_init);

/*
 * Disown pages in both lists of a 2-queue.
 */
void cl_2queue_disown(const struct lu_env *env, struct cl_2queue *queue)
{
	ENTRY;
	cl_page_list_disown(env, &queue->c2_qin);
	cl_page_list_disown(env, &queue->c2_qout);
	EXIT;
}
EXPORT_SYMBOL(cl_2queue_disown);

/*
 * Discard (truncate) pages in both lists of a 2-queue.
 */
void cl_2queue_discard(const struct lu_env *env,
		       struct cl_io *io, struct cl_2queue *queue)
{
	ENTRY;
	cl_page_list_discard(env, io, &queue->c2_qin);
	cl_page_list_discard(env, io, &queue->c2_qout);
	EXIT;
}
EXPORT_SYMBOL(cl_2queue_discard);

/*
 * Assume to own the pages in cl_2queue
 */
void cl_2queue_assume(const struct lu_env *env,
		      struct cl_io *io, struct cl_2queue *queue)
{
	cl_page_list_assume(env, io, &queue->c2_qin);
	cl_page_list_assume(env, io, &queue->c2_qout);
}

/*
 * Finalize both page lists of a 2-queue.
 */
void cl_2queue_fini(const struct lu_env *env, struct cl_2queue *queue)
{
	ENTRY;
	cl_page_list_fini(env, &queue->c2_qout);
	cl_page_list_fini(env, &queue->c2_qin);
	EXIT;
}
EXPORT_SYMBOL(cl_2queue_fini);

/*
 * Initialize a 2-queue to contain @page in its incoming page list.
 */
void cl_2queue_init_page(struct cl_2queue *queue, struct cl_page *page)
{
	ENTRY;
	cl_2queue_init(queue);
	/*
	 * Add a page to the incoming page list of 2-queue.
	 */
	cl_page_list_add(&queue->c2_qin, page, true);
	EXIT;
}
EXPORT_SYMBOL(cl_2queue_init_page);

/* Returns top-level io. (See cl_object.c:cl_object_top()) */
struct cl_io *cl_io_top(struct cl_io *io)
{
	ENTRY;
	while (io->ci_parent != NULL)
		io = io->ci_parent;
	RETURN(io);
}
EXPORT_SYMBOL(cl_io_top);

/**
 * cl_req_attr_set() - Set attr for IO request
 * @env: execution environment
 * @obj: Object linked to IO operation (obj == cl_object_top(obj))
 * @attr: Per-transfer attributes
 *
 * Fills in attributes that are passed to server together with transfer. Only
 * attributes from @obj->cra_flags may be touched. This can be called multiple
 * times for the same request
 */
void cl_req_attr_set(const struct lu_env *env, struct cl_object *obj,
		     struct cl_req_attr *attr)
{
	struct cl_object *scan;
	ENTRY;

	cl_object_for_each(scan, obj) {
		if (scan->co_ops->coo_req_attr_set != NULL)
			scan->co_ops->coo_req_attr_set(env, scan, attr);
	}
	EXIT;
}
EXPORT_SYMBOL(cl_req_attr_set);

/**
 * cl_sync_io_init_notify() - Initialize synchronous IO wait @anchor for @nr
 * pages with optional @end handler.
 * @anchor: owned by caller, initialized here.
 * @nr: number of pages initially pending in sync.
 * @dio_aio: Is it a DIO or AIO
 * @end: optional callback sync_io completion, can be used to trigger erasure
 * coding, integrity, dedupe, or similar operation. @end is called with a
 * spinlock on anchor->csi_waitq.lock
 */
void cl_sync_io_init_notify(struct cl_sync_io *anchor, int nr,
			    void *dio_aio, cl_sync_io_end_t *end)
{
	ENTRY;
	memset(anchor, 0, sizeof(*anchor));
	init_waitqueue_head(&anchor->csi_waitq);
	atomic_set(&anchor->csi_sync_nr, nr);
	atomic_set(&anchor->csi_complete, 0);
	anchor->csi_sync_rc = 0;
	anchor->csi_end_io = end;
	anchor->csi_dio_aio = dio_aio;
	EXIT;
}
EXPORT_SYMBOL(cl_sync_io_init_notify);

/*
 * Wait until all IO completes. Transfer completion routine has to call
 * cl_sync_io_note() for every entity.
 */
int cl_sync_io_wait(const struct lu_env *env, struct cl_sync_io *anchor,
		    long timeout)
{
	int rc = 0;
	ENTRY;

	LASSERT(timeout >= 0);

	if (timeout > 0 &&
	    wait_event_idle_timeout(anchor->csi_waitq,
				    atomic_read(&anchor->csi_complete) == 1,
				    cfs_time_seconds(timeout)) == 0) {
		rc = -ETIMEDOUT;
		CERROR("IO failed: %d, still wait for %d remaining entries\n",
		       rc, atomic_read(&anchor->csi_complete));
	}

	wait_event_idle(anchor->csi_waitq,
			atomic_read(&anchor->csi_complete) == 1);
	if (!rc)
		rc = anchor->csi_sync_rc;

	/* We take the lock to ensure that cl_sync_io_note() has finished */
	spin_lock(&anchor->csi_waitq.lock);
	LASSERT(atomic_read(&anchor->csi_sync_nr) == 0);
	LASSERT(atomic_read(&anchor->csi_complete) == 1);
	spin_unlock(&anchor->csi_waitq.lock);

	RETURN(rc);
}
EXPORT_SYMBOL(cl_sync_io_wait);

static inline void dio_aio_complete(struct kiocb *iocb, ssize_t res)
{
#ifdef HAVE_AIO_COMPLETE
	aio_complete(iocb, res, 0);
#else
	if (iocb->ki_complete)
# ifdef HAVE_KIOCB_COMPLETE_2ARGS
		iocb->ki_complete(iocb, res);
# else
		iocb->ki_complete(iocb, res, 0);
# endif
#endif
}

void cl_dio_pages_2queue(struct cl_dio_pages *cdp)
{
	int i = 0;

	cl_2queue_init(&cdp->cdp_queue);

	for (i = 0; i < cdp->cdp_page_count; i++) {
		struct cl_page *page = cdp->cdp_cl_pages[i];

		cl_page_list_add(&cdp->cdp_queue.c2_qin, page, false);
	}
}
EXPORT_SYMBOL(cl_dio_pages_2queue);

static void cl_dio_aio_end(const struct lu_env *env, struct cl_sync_io *anchor)
{
	struct cl_dio_aio *aio = container_of(anchor, typeof(*aio), cda_sync);
	ssize_t ret = anchor->csi_sync_rc;

	ENTRY;

	if (!aio->cda_no_aio_complete)
		dio_aio_complete(aio->cda_iocb, ret ?: aio->cda_bytes);

	EXIT;
}

static inline void csd_dup_free(struct cl_iter_dup *dup)
{
	void *tmp = dup->id_vec;

	dup->id_vec = NULL;
	OBD_FREE(tmp, dup->id_vec_size);
}

static void cl_sub_dio_end(const struct lu_env *env, struct cl_sync_io *anchor)
{
	struct cl_sub_dio *sdio = container_of(anchor, typeof(*sdio), csd_sync);
	struct cl_dio_pages *cdp = &sdio->csd_dio_pages;
	ssize_t ret = anchor->csi_sync_rc;
	bool array_incomplete = false;
	int i;

	ENTRY;

	if (cdp->cdp_cl_pages) {
		for (i = 0; i < cdp->cdp_page_count; i++) {
			struct cl_page *page = cdp->cdp_cl_pages[i];
			/* if we failed allocating pages, the page array may be
			 * incomplete, so check the pointers
			 *
			 * FIXME: This extra tracking of array completeness is
			 * just a debug check and will be removed later in the
			 * series.
			 */
			if (page)
				cl_page_put(env, page);
			else if (array_incomplete)
				LASSERT(!page);
			else
				array_incomplete = true;
		}
		OBD_FREE_PTR_ARRAY_LARGE(cdp->cdp_cl_pages,
					 cdp->cdp_page_count);
	}

	if (sdio->csd_unaligned) {
		CDEBUG(D_VFSTRACE,
		       "finishing unaligned dio %s aio->cda_bytes %ld\n",
		       sdio->csd_write ? "write" : "read", sdio->csd_bytes);
		/* read copies *from* the kernel buffer *to* userspace
		 * here at the end, write copies *to* the kernel
		 * buffer from userspace at the start
		 */
		if (!sdio->csd_write && sdio->csd_bytes > 0)
			ret = ll_dio_user_copy(sdio);
		ll_free_dio_buffer(cdp);
		/* handle the freeing here rather than in cl_sub_dio_free
		 * because we have the unmodified iovec pointer
		 */
		csd_dup_free(&sdio->csd_dup);
	} else {
		/* unaligned DIO does not get user pages, so it doesn't have to
		 * release them, but aligned I/O must
		 */
		ll_release_user_pages(cdp->cdp_pages, cdp->cdp_page_count);
	}
	cl_sync_io_note(env, &sdio->csd_ll_aio->cda_sync, ret);

	EXIT;
}

struct cl_dio_aio *cl_dio_aio_alloc(struct kiocb *iocb, struct cl_object *obj,
				    bool is_aio)
{
	struct cl_dio_aio *aio;

	OBD_SLAB_ALLOC_PTR_GFP(aio, cl_dio_aio_kmem, GFP_NOFS);
	if (aio != NULL) {
		/*
		 * Hold one ref so that it won't be released until
		 * every pages is added.
		 */
		cl_sync_io_init_notify(&aio->cda_sync, 1, aio, cl_dio_aio_end);
		aio->cda_iocb = iocb;
		aio->cda_is_aio = is_aio;
		aio->cda_no_aio_complete = !is_aio;
		/* if this is true AIO, the memory is freed by the last call
		 * to cl_sync_io_note (when all the I/O is complete), because
		 * no one is waiting (in the kernel) for this to complete
		 *
		 * in other cases, the last user is cl_sync_io_wait, and in
		 * that case, the creator frees the struct after that call
		 */
		aio->cda_creator_free = !is_aio;

		cl_object_get(obj);
		aio->cda_obj = obj;
		aio->cda_mm = get_task_mm(current);
	}
	return aio;
}
EXPORT_SYMBOL(cl_dio_aio_alloc);

struct cl_sub_dio *cl_sub_dio_alloc(struct cl_dio_aio *ll_aio,
				    struct iov_iter *iter, bool write,
				    bool unaligned, bool sync)
{
	struct cl_sub_dio *sdio;

	OBD_SLAB_ALLOC_PTR_GFP(sdio, cl_sub_dio_kmem, GFP_NOFS);
	if (sdio != NULL) {
		/*
		 * Hold one ref so that it won't be released until
		 * every pages is added.
		 */
		cl_sync_io_init_notify(&sdio->csd_sync, 1, sdio,
				       cl_sub_dio_end);

		sdio->csd_ll_aio = ll_aio;
		sdio->csd_creator_free = sync;
		sdio->csd_write = write;
		sdio->csd_unaligned = unaligned;
		spin_lock_init(&sdio->csd_lock);

		atomic_add(1,  &ll_aio->cda_sync.csi_sync_nr);

		if (sdio->csd_unaligned) {
			size_t v_sz = 0;

			/* we need to make a copy of the user iovec at this
			 * point in time, in order to:
			 *
			 * A) have the correct state of the iovec for this
			 * chunk of I/O, ie, the main iovec is altered as we do
			 * I/O and this chunk needs the current state
			 * B) have a chunk-local copy; doing the IO later
			 * modifies the iovec, so to process each chunk from a
			 * separate thread requires a local copy of the iovec
			 */
			sdio->csd_iter = *iter;
			if (iov_iter_is_bvec(iter))
				v_sz = iter->nr_segs * sizeof(struct bio_vec);
			else if (iov_iter_is_kvec(iter) || iter_is_iovec(iter))
				v_sz = iter->nr_segs * sizeof(struct iovec);

			/* xarray and discard do not need vec to be dup'd */
			if (!v_sz)
				goto out;

			OBD_ALLOC(sdio->csd_dup.id_vec, v_sz);
			if (!sdio->csd_dup.id_vec) {
				cl_sub_dio_free(sdio);
				sdio = NULL;
				goto out;
			}
			memcpy(sdio->csd_dup.id_vec, iter->__iov, v_sz);
			sdio->csd_dup.id_vec_size = v_sz;
			sdio->csd_iter.__iov = sdio->csd_dup.id_vec;
		}
	}
out:
	return sdio;
}
EXPORT_SYMBOL(cl_sub_dio_alloc);

void cl_dio_aio_free(const struct lu_env *env, struct cl_dio_aio *aio)
{
	if (aio) {
		if (aio->cda_mm)
			mmput(aio->cda_mm);
		cl_object_put(env, aio->cda_obj);
		OBD_SLAB_FREE_PTR(aio, cl_dio_aio_kmem);
	}
}
EXPORT_SYMBOL(cl_dio_aio_free);

void cl_sub_dio_free(struct cl_sub_dio *sdio)
{
	if (sdio) {
		if (sdio->csd_dup.id_vec) {
			LASSERT(sdio->csd_unaligned);
			csd_dup_free(&sdio->csd_dup);
			sdio->csd_iter.__iov = NULL;
		}
		OBD_SLAB_FREE_PTR(sdio, cl_sub_dio_kmem);
	}
}
EXPORT_SYMBOL(cl_sub_dio_free);

/*
 * For unaligned DIO.
 *
 * Allocate the internal buffer from/to which we will perform DIO.  This takes
 * the user I/O parameters and allocates an internal buffer large enough to
 * hold it.  The pages in this buffer are aligned with pages in the file (ie,
 * they have a 1-to-1 mapping with file pages).
 */
int ll_allocate_dio_buffer(struct cl_dio_pages *cdp, size_t io_size)
{
	size_t pg_offset;
	int result = 0;

	ENTRY;

	/* page level offset in the file where the I/O starts */
	pg_offset = cdp->cdp_file_offset & ~PAGE_MASK;
	/* this adds 1 for the first page and removes the bytes in it from the
	 * io_size, making the rest of the calculation aligned
	 */
	if (pg_offset) {
		cdp->cdp_page_count++;
		io_size -= min_t(size_t, PAGE_SIZE - pg_offset, io_size);
	}

	/* calculate pages for the rest of the buffer */
	cdp->cdp_page_count += (io_size + PAGE_SIZE - 1) >> PAGE_SHIFT;

#ifdef HAVE_DIO_ITER
	cdp->cdp_pages = kvzalloc(cdp->cdp_page_count * sizeof(struct page *),
				  GFP_NOFS);
#else
	OBD_ALLOC_PTR_ARRAY_LARGE(cdp->cdp_pages, cdp->cdp_page_count);
#endif
	if (cdp->cdp_pages == NULL)
		GOTO(out, result = -ENOMEM);

	result = obd_pool_get_pages_array(cdp->cdp_pages, cdp->cdp_page_count);
	if (result)
		GOTO(out, result);

out:
	if (result) {
		if (cdp->cdp_pages)
			ll_free_dio_buffer(cdp);
	}

	if (result == 0)
		result = cdp->cdp_page_count;

	RETURN(result);
}
EXPORT_SYMBOL(ll_allocate_dio_buffer);

void ll_free_dio_buffer(struct cl_dio_pages *cdp)
{
	obd_pool_put_pages_array(cdp->cdp_pages, cdp->cdp_page_count);

#ifdef HAVE_DIO_ITER
	kvfree(cdp->cdp_pages);
#else
	OBD_FREE_PTR_ARRAY_LARGE(cdp->cdp_pages, cdp->cdp_page_count);
#endif
}
EXPORT_SYMBOL(ll_free_dio_buffer);

/*
 * ll_release_user_pages - tear down page struct array
 * @pages: array of page struct pointers underlying target buffer
 */
void ll_release_user_pages(struct page **pages, int npages)
{
	int i;

	if (npages == 0) {
		LASSERT(!pages);
		return;
	}

	for (i = 0; i < npages; i++) {
		if (!pages[i])
			break;
		put_page(pages[i]);
	}

#if defined(HAVE_DIO_ITER)
	kvfree(pages);
#else
	OBD_FREE_PTR_ARRAY_LARGE(pages, npages);
#endif
}
EXPORT_SYMBOL(ll_release_user_pages);

#ifdef HAVE_FAULT_IN_IOV_ITER_READABLE
#define ll_iov_iter_fault_in_readable(iov, bytes) \
	fault_in_iov_iter_readable(iov, bytes)
#else
#define ll_iov_iter_fault_in_readable(iov, bytes) \
	iov_iter_fault_in_readable(iov, bytes)
#endif

#ifndef HAVE_KTHREAD_USE_MM
#define kthread_use_mm(mm) use_mm(mm)
#define kthread_unuse_mm(mm) unuse_mm(mm)
#endif

/* copy IO data to/from internal buffer and userspace iovec */
static ssize_t __ll_dio_user_copy(struct cl_sub_dio *sdio)
{
	struct iov_iter *iter = &sdio->csd_iter;
	struct cl_dio_pages *cdp = &sdio->csd_dio_pages;
	struct mm_struct *mm = sdio->csd_ll_aio->cda_mm;
	loff_t pos = cdp->cdp_file_offset;
	size_t count = sdio->csd_bytes;
	size_t original_count = count;
	int short_copies = 0;
	bool mm_used = false;
	bool locked = false;
	unsigned int i = 0;
	int status = 0;
	int rw;

	ENTRY;

	LASSERT(sdio->csd_unaligned);

	if (sdio->csd_write)
		rw = WRITE;
	else
		rw = READ;

	/* read copying is protected by the reference count on the sdio, since
	 * it's done as part of getting rid of the sdio, but write copying is
	 * done at the start, where there may be multiple ptlrpcd threads
	 * using this sdio, so we must lock and check if the copying has
	 * been done
	 */
	if (rw == WRITE) {
		spin_lock(&sdio->csd_lock);
		locked = true;
		if (sdio->csd_write_copied)
			GOTO(out, status = 0);
	}
	/* if there's no mm, io is being done from a kernel thread, so there's
	 * no need to transition to its mm context anyway.
	 *
	 * Also, if mm == current->mm, that means this is being handled in the
	 * thread which created it, and not in a separate kthread - so it is
	 * unnecessary (and incorrect) to do a use_mm here
	 *
	 * assert that if we have an mm and it's not ours, we're doing this
	 * copying from a kernel thread - otherwise kthread_use_mm will happily
	 * trash memory and crash later
	 */
	if (mm && mm != current->mm) {
		LASSERT(current->flags & PF_KTHREAD);
		kthread_use_mm(mm);
		mm_used = true;
	}

	/* fault in the entire userspace iovec */
	if (rw == WRITE) {
		if (unlikely(ll_iov_iter_fault_in_readable(iter, count)))
			GOTO(out, status = -EFAULT);
	}

	/* modeled on kernel generic_file_buffered_read/write()
	 *
	 * note we only have one 'chunk' of i/o here, so we do not copy the
	 * whole iovec here (except when the chunk is the whole iovec) so we
	 * use the count of bytes in the chunk, csd_bytes, instead of looking
	 * at the iovec
	 */
	while (true) {
		struct page *page = cdp->cdp_pages[i];
		unsigned long offset; /* offset into kernel buffer page */
		size_t copied; /* bytes successfully copied */
		size_t bytes; /* bytes to copy for this page */

		LASSERT(i < cdp->cdp_page_count);

		offset = pos & ~PAGE_MASK;
		bytes = min_t(unsigned long, PAGE_SIZE - offset, count);

		CDEBUG(D_VFSTRACE,
		       "count %zd, offset %lu, pos %lld, cdp_page_count %u\n",
		       count, offset, pos, cdp->cdp_page_count);

		if (fatal_signal_pending(current)) {
			status = -EINTR;
			break;
		}

		/* like btrfs, we do not have a mapping since this isn't
		 * a page cache page, so we must do this flush
		 * unconditionally
		 *
		 * NB: This is a noop on x86 but active on other
		 * architectures
		 */
		flush_dcache_page(page);

		/* write requires a few extra steps */
		if (rw == WRITE) {
#ifndef HAVE_COPY_PAGE_FROM_ITER_ATOMIC
			copied = iov_iter_copy_from_user_atomic(page, iter,
								offset, bytes);
			iov_iter_advance(iter, copied);
#else
			copied = copy_page_from_iter_atomic(page, offset, bytes,
							    iter);
#endif
			flush_dcache_page(page);

		} else /* READ */ {
			copied = copy_page_to_iter(page, offset, bytes, iter);
		}

		pos += copied;
		count -= copied;

		if (unlikely(copied < bytes)) {
			short_copies++;

			CDEBUG(D_VFSTRACE,
			       "short copy - copied only %zd of %lu, short %d times\n",
			       copied, bytes, short_copies);
			/* copies will very rarely be interrupted, but we
			 * should retry in those cases, since the other option
			 * is giving an IO error and this can occur in normal
			 * operation such as with racing unaligned AIOs
			 *
			 * but of course we should not retry indefinitely
			 */
			if (short_copies > 2) {
				CERROR("Unaligned DIO copy repeatedly short, count %zd, offset %lu, bytes %lu, copied %zd, pos %lld\n",
				count, offset, bytes, copied, pos);

				status = -EFAULT;
				break;
			}

			continue;
		}

		if (count == 0)
			break;

		i++;
	}

	if (rw == WRITE && status == 0)
		sdio->csd_write_copied = true;

	/* if we complete successfully, we should reach all of the pages */
	LASSERTF(ergo(status == 0, i == cdp->cdp_page_count - 1),
		 "status: %d, i: %d, cdp->cdp_page_count %u, count %zu\n",
		  status, i, cdp->cdp_page_count, count);

out:
	if (mm_used)
		kthread_unuse_mm(mm);

	if (locked)
		spin_unlock(&sdio->csd_lock);

	/* the total bytes copied, or status */
	RETURN(original_count - count ? original_count - count : status);
}

struct dio_user_copy_data {
	struct cl_sub_dio *ducd_sdio;
	struct completion ducd_completion;
	ssize_t ducd_result;
};

static int ll_dio_user_copy_helper(void *data)
{
	struct dio_user_copy_data *ducd = data;
	struct cl_sub_dio *sdio = ducd->ducd_sdio;

	ducd->ducd_result = __ll_dio_user_copy(sdio);
	complete(&ducd->ducd_completion);

	return 0;
}

ssize_t ll_dio_user_copy(struct cl_sub_dio *sdio)
{
	struct dio_user_copy_data ducd;
	struct task_struct *kthread;

	/* normal case - copy is being done by ptlrpcd */
	if (current->flags & PF_KTHREAD ||
	/* for non-parallel DIO, the submitting thread does the copy */
	    sdio->csd_ll_aio->cda_mm == current->mm)
		return __ll_dio_user_copy(sdio);

	/* this is a slightly unfortunate workaround; when doing an fsync, a
	 * user thread may pick up a DIO extent which is about to be written
	 * out.  we can't just ignore these, but we also can't handle them from
	 * the user thread, since user threads can't do data copying from
	 * another thread's memory.
	 *
	 * so we spawn a kthread to handle this case.
	 * this will be rare and is not a 'hot path', so the performance
	 * cost doesn't matter
	 */
	init_completion(&ducd.ducd_completion);
	ducd.ducd_sdio = sdio;

	kthread = kthread_run(ll_dio_user_copy_helper, &ducd,
			      "ll_ucp_%u", current->pid);
	if (IS_ERR_OR_NULL(kthread))
		return PTR_ERR(kthread);
	wait_for_completion(&ducd.ducd_completion);

	return ducd.ducd_result;
}
EXPORT_SYMBOL(ll_dio_user_copy);

/*
 * Indicate that transfer of a single page completed.
 */
void __cl_sync_io_note(const struct lu_env *env, struct cl_sync_io *anchor,
		       int count, int ioret)
{
	int sync_nr;

	ENTRY;

	if (anchor->csi_sync_rc == 0 && ioret < 0)
		anchor->csi_sync_rc = ioret;

	/* because there is no atomic_sub_and_lock, we have to do this slightly
	 * awkward subtraction when we have count > 1, handling all but 1 of
	 * our 'count' entries
	 */
	if (count > 1)
		sync_nr = atomic_sub_return(count - 1, &anchor->csi_sync_nr);
	else
		sync_nr = atomic_read(&anchor->csi_sync_nr);

	CDEBUG(D_VFSTRACE,
	       "Noting completion of %d items, %d items remaining.\n",
	       count, sync_nr - 1);
	/*
	 * Synchronous IO done without releasing page lock (e.g., as a part of
	 * ->{prepare,commit}_write(). Completion is used to signal the end of
	 * IO.
	 */
	LASSERT(sync_nr > 0);
	LASSERT(atomic_read(&anchor->csi_complete) == 0);
	if (atomic_dec_and_lock(&anchor->csi_sync_nr,
				&anchor->csi_waitq.lock)) {
		struct cl_sub_dio *sub_dio_aio = NULL;
		struct cl_dio_aio *dio_aio = NULL;
		void *csi_dio_aio = NULL;
		bool creator_free = true;

		cl_sync_io_end_t *end_io = anchor->csi_end_io;

		spin_unlock(&anchor->csi_waitq.lock);
		/* we cannot do end_io while holding a spin lock, because
		 * end_io may sleep
		 */
		if (end_io)
			end_io(env, anchor);

		spin_lock(&anchor->csi_waitq.lock);
		/* this tells the waiters we've completed, and can only be set
		 * after end_io() has been called and while we're holding the
		 * spinlock
		 */
		atomic_set(&anchor->csi_complete, 1);
		/*
		 * Holding the lock across both the decrement and
		 * the wakeup ensures cl_sync_io_wait() doesn't complete
		 * before the wakeup completes and the contents of
		 * of anchor become unsafe to access as the owner is free
		 * to immediately reclaim anchor when cl_sync_io_wait()
		 * completes.
		 */
		wake_up_locked(&anchor->csi_waitq);

		csi_dio_aio = anchor->csi_dio_aio;
		sub_dio_aio = csi_dio_aio;
		dio_aio = csi_dio_aio;

		if (csi_dio_aio && end_io == cl_dio_aio_end)
			creator_free = dio_aio->cda_creator_free;
		else if (csi_dio_aio && end_io == cl_sub_dio_end)
			creator_free = sub_dio_aio->csd_creator_free;

		spin_unlock(&anchor->csi_waitq.lock);

		if (csi_dio_aio && !creator_free) {
			if (end_io == cl_dio_aio_end)
				cl_dio_aio_free(env, dio_aio);
			else if (end_io == cl_sub_dio_end)
				cl_sub_dio_free(sub_dio_aio);
		}
	}
	EXIT;
}
EXPORT_SYMBOL(__cl_sync_io_note);

void cl_sync_io_note(const struct lu_env *env, struct cl_sync_io *anchor,
		     int ioret)
{
	__cl_sync_io_note(env, anchor, 1, ioret);
}
EXPORT_SYMBOL(cl_sync_io_note);

/* this function waits for completion of outstanding io and then re-initializes
 * the anchor used to track it.  This is used to wait to complete DIO before
 * returning to userspace, and is never called for true AIO
 */
int cl_sync_io_wait_recycle(const struct lu_env *env, struct cl_sync_io *anchor,
			    long timeout, int ioret)
{
	int rc = 0;

	/*
	 * @anchor was inited as 1 to prevent end_io to be
	 * called before we add all pages for IO, so drop
	 * one extra reference to make sure we could wait
	 * count to be zero.
	 */
	cl_sync_io_note(env, anchor, ioret);
	/* Wait for completion of outstanding dio before re-initializing for
	 * possible restart
	 */
	rc = cl_sync_io_wait(env, anchor, timeout);
	/**
	 * One extra reference again, as if @anchor is
	 * reused we assume it as 1 before using.
	 */
	atomic_add(1, &anchor->csi_sync_nr);
	/* we must also set this anchor as incomplete */
	atomic_set(&anchor->csi_complete, 0);

	return rc;
}
EXPORT_SYMBOL(cl_sync_io_wait_recycle);
