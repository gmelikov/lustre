// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2010, 2017, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 */

/*
 * This file contains implementation of EXTENT lock type
 *
 * EXTENT lock type is for locking a contiguous range of values, represented
 * by 64-bit starting and ending offsets (inclusive). There are several extent
 * lock modes, some of which may be mutually incompatible. Extent locks are
 * considered incompatible if their modes are incompatible and their extents
 * intersect.  See the lock mode compatibility matrix in lustre_dlm.h.
 */

#define DEBUG_SUBSYSTEM S_LDLM

#include <libcfs/libcfs.h>
#include <lustre_dlm.h>
#include <obd_support.h>
#include <obd.h>
#include <obd_class.h>
#include <lustre_lib.h>

#include "ldlm_internal.h"
#include <linux/interval_tree_generic.h>

#define START(node) ((node)->l_policy_data.l_extent.start)
#define LAST(node) ((node)->l_policy_data.l_extent.end)
INTERVAL_TREE_DEFINE(struct ldlm_lock, l_rb, u64, l_subtree_last,
		     START, LAST, static inline, extent);

static inline struct ldlm_lock *extent_next_lock(struct ldlm_lock *lock)
{
	lock = list_next_entry(lock, l_same_extent);
	if (RB_EMPTY_NODE(&lock->l_rb))
		return lock;
	return extent_next(lock);
}

static inline struct ldlm_lock *extent_prev_lock(struct ldlm_lock *lock)
{
	lock = list_prev_entry(lock, l_same_extent);
	if (RB_EMPTY_NODE(&lock->l_rb))
		return lock;
	return extent_prev(lock);
}

static inline struct ldlm_lock *extent_insert_unique(
	struct ldlm_lock *node, struct interval_tree_root *root)
{
	struct rb_node **link, *rb_parent = NULL;
	u64 start = START(node), last = LAST(node);
	struct ldlm_lock *parent;
	bool leftmost = true;

#ifdef HAVE_INTERVAL_TREE_CACHED
	link = &root->rb_root.rb_node;
#else
	link = &root->rb_node;
#endif
	while (*link) {
		rb_parent = *link;
		parent = rb_entry(rb_parent, struct ldlm_lock, l_rb);
		if (parent->l_subtree_last < last)
			parent->l_subtree_last = last;
		if (start == START(parent)) {
			if (last == LAST(parent))
				return parent;
			if (last < LAST(parent)) {
				link = &parent->l_rb.rb_left;
			} else {
				link = &parent->l_rb.rb_right;
				leftmost = false;
			}
		} else if (start < START(parent))
			link = &parent->l_rb.rb_left;
		else {
			link = &parent->l_rb.rb_right;
			leftmost = false;
		}
	}

	node->l_subtree_last = last;
	rb_link_node(&node->l_rb, rb_parent, link);
#ifdef HAVE_INTERVAL_TREE_CACHED
	rb_insert_augmented_cached(&node->l_rb, root,
				   leftmost, &extent_augment);
#else
	rb_insert_augmented(&node->l_rb, root,
			    &extent_augment);
#endif
	return NULL;
}

static inline void extent_replace(struct ldlm_lock *in_tree,
				  struct ldlm_lock *new,
				  struct interval_tree_root *tree)
{
	struct rb_node *p = rb_parent(&in_tree->l_rb);

	/* place 'new' in the rbtree replacing in_tree */
	new->l_rb.rb_left = in_tree->l_rb.rb_left;
	new->l_rb.rb_right = in_tree->l_rb.rb_right;

	if (new->l_rb.rb_left)
		rb_set_parent_color(new->l_rb.rb_left, &new->l_rb,
				    rb_color(new->l_rb.rb_left));
	if (new->l_rb.rb_right)
		rb_set_parent_color(new->l_rb.rb_right, &new->l_rb,
				    rb_color(new->l_rb.rb_right));
	rb_set_parent_color(&new->l_rb, p, rb_color(&in_tree->l_rb));

	if (!p) {
#ifdef HAVE_INTERVAL_TREE_CACHED
		tree->rb_root.rb_node = &new->l_rb;
#else
		tree->rb_node = &new->l_rb;
#endif
	} else if (p->rb_left == &in_tree->l_rb) {
		p->rb_left = &new->l_rb;
	} else {
		p->rb_right = &new->l_rb;
	}
#ifdef HAVE_INTERVAL_TREE_CACHED
	if (tree->rb_leftmost == &in_tree->l_rb)
		tree->rb_leftmost = &new->l_rb;
#endif
}

#ifdef HAVE_SERVER_SUPPORT
# define LDLM_MAX_GROWN_EXTENT (32 * 1024 * 1024 - 1)

/**
 * Fix up the ldlm_extent after expanding it.
 *
 * After expansion has been done, we might still want to do certain adjusting
 * based on overall contention of the resource and the like to avoid granting
 * overly wide locks.
 */
static void ldlm_extent_internal_policy_fixup(struct ldlm_lock *req,
					      struct ldlm_extent *new_ex,
					      int conflicting)
{
	enum ldlm_mode req_mode = req->l_req_mode;
	__u64 req_start = req->l_req_extent.start;
	__u64 req_end = req->l_req_extent.end;
	__u64 req_align, mask;

	if (conflicting > 32 && (req_mode == LCK_PW || req_mode == LCK_CW)) {
		if (req_end < req_start + LDLM_MAX_GROWN_EXTENT)
			new_ex->end = min(req_start + LDLM_MAX_GROWN_EXTENT,
					  new_ex->end);
	}

	if (new_ex->start == 0 && new_ex->end == OBD_OBJECT_EOF) {
		EXIT;
		return;
	}

	/* we need to ensure that the lock extent is properly aligned to what
	 * the client requested. Also we need to make sure it's also server
	 * page size aligned otherwise a server page can be covered by two
	 * write locks.
	 */
	mask = PAGE_SIZE;
	req_align = (req_end + 1) | req_start;
	if (req_align != 0 && (req_align & (mask - 1)) == 0) {
		while ((req_align & mask) == 0)
			mask <<= 1;
	}
	mask -= 1;
	/* We can only shrink the lock, not grow it.
	 * This should never cause lock to be smaller than requested,
	 * since requested lock was already aligned on these boundaries.
	 */
	new_ex->start = ((new_ex->start - 1) | mask) + 1;
	new_ex->end = ((new_ex->end + 1) & ~mask) - 1;
	LASSERTF(new_ex->start <= req_start,
		 "mask %#llx grant start %llu req start %llu\n",
		 mask, new_ex->start, req_start);
	LASSERTF(new_ex->end >= req_end,
		 "mask %#llx grant end %llu req end %llu\n",
		 mask, new_ex->end, req_end);
}

/**
 * Return the maximum extent that:
 * - contains the requested extent
 * - does not overlap existing conflicting extents outside the requested one
 *
 * This allows clients to request a small required extent range, but if there
 * is no contention on the lock the full lock can be granted to the client.
 * This avoids the need for many smaller lock requests to be granted in the
 * common (uncontended) case.
 *
 * Use interval tree to expand the lock extent for granted lock.
 */
static void ldlm_extent_internal_policy_granted(struct ldlm_lock *req,
						struct ldlm_extent *new_ex)
{
	struct ldlm_resource *res = req->l_resource;
	enum ldlm_mode req_mode = req->l_req_mode;
	__u64 req_start = req->l_req_extent.start;
	__u64 req_end = req->l_req_extent.end;
	struct ldlm_interval_tree *tree;
	int conflicting = 0;
	int idx;

	ENTRY;

	lockmode_verify(req_mode);

	/* Using interval tree to handle the LDLM extent granted locks. */
	for (idx = 0; idx < LCK_MODE_NUM; idx++) {
		tree = &res->lr_itree[idx];
		if (lockmode_compat(tree->lit_mode, req_mode))
			continue;

		conflicting += tree->lit_size;

		if (!INTERVAL_TREE_EMPTY(&tree->lit_root)) {
			struct ldlm_lock *lck = NULL;

			LASSERTF(!extent_iter_first(&tree->lit_root,
						    req_start, req_end),
				 "req_mode=%d, start=%llu, end=%llu\n",
				 req_mode, req_start, req_end);
			/*
			 * If any tree is non-empty we don't bother
			 * expanding backwards, it won't be worth
			 * the effort.
			 */
			new_ex->start = req_start;

			if (req_end < (U64_MAX - 1))
				/* lck is the lock with the lowest endpoint
				 * which covers anything after 'req'
				 */
				lck = extent_iter_first(&tree->lit_root,
							req_end + 1, U64_MAX);
			if (lck)
				new_ex->end = min(new_ex->end, START(lck) - 1);
		}

		if (new_ex->start == req_start && new_ex->end == req_end)
			break;
	}

	LASSERT(new_ex->start <= req_start);
	LASSERT(new_ex->end >= req_end);

	ldlm_extent_internal_policy_fixup(req, new_ex, conflicting);
	EXIT;
}

/* The purpose of this function is to return:
 * - the maximum extent
 * - containing the requested extent
 * - and not overlapping existing conflicting extents outside the requested one
 */
static void
ldlm_extent_internal_policy_waiting(struct ldlm_lock *req,
				    struct ldlm_extent *new_ex)
{
	struct ldlm_resource *res = req->l_resource;
	enum ldlm_mode req_mode = req->l_req_mode;
	__u64 req_start = req->l_req_extent.start;
	__u64 req_end = req->l_req_extent.end;
	struct ldlm_lock *lock;
	int conflicting = 0;

	ENTRY;

	lockmode_verify(req_mode);

	/* for waiting locks */
	list_for_each_entry(lock, &res->lr_waiting, l_res_link) {
		struct ldlm_extent *l_extent = &lock->l_policy_data.l_extent;

		/* We already hit the minimum requested size, search no more */
		if (new_ex->start == req_start && new_ex->end == req_end) {
			EXIT;
			return;
		}

		/* Don't conflict with ourselves */
		if (req == lock)
			continue;

		/* Locks are compatible, overlap doesn't matter
		 * Until bug 20 is fixed, try to avoid granting overlapping
		 * locks on one client (they take a long time to cancel)
		 */
		if (lockmode_compat(lock->l_req_mode, req_mode) &&
		    lock->l_export != req->l_export)
			continue;

		/* If this is a high-traffic lock, don't grow downwards at all
		 * or grow upwards too much
		 */
		++conflicting;
		if (conflicting > 4)
			new_ex->start = req_start;

		/* If lock doesn't overlap new_ex, skip it. */
		if (!ldlm_extent_overlap(l_extent, new_ex))
			continue;

		/* Locks conflicting in requested extents and we can't satisfy
		 * both locks, so ignore it.  Either we will ping-pong this
		 * extent (we would regardless of what extent we granted) or
		 * lock is unused and it shouldn't limit our extent growth.
		 */
		if (ldlm_extent_overlap(&lock->l_req_extent,
					 &req->l_req_extent))
			continue;

		/* We grow extents downwards only as far as they don't overlap
		 * with already-granted locks, on the assumption that clients
		 * will be writing beyond the initial requested end and would
		 * then need to enqueue a new lock beyond previous request.
		 * l_req_extent->end strictly < req_start, checked above.
		 */
		if (l_extent->start < req_start && new_ex->start != req_start) {
			if (l_extent->end >= req_start)
				new_ex->start = req_start;
			else
				new_ex->start = min(l_extent->end+1, req_start);
		}

		/* If we need to cancel this lock anyways because our request
		 * overlaps the granted lock, we grow up to its requested
		 * extent start instead of limiting this extent, assuming that
		 * clients are writing forwards and the lock had over grown
		 * its extent downwards before we enqueued our request.
		 */
		if (l_extent->end > req_end) {
			if (l_extent->start <= req_end)
				new_ex->end = max(lock->l_req_extent.start - 1,
						  req_end);
			else
				new_ex->end = max(l_extent->start - 1, req_end);
		}
	}

	ldlm_extent_internal_policy_fixup(req, new_ex, conflicting);
	EXIT;
}


/* In order to determine the largest possible extent we can grant, we need
 * to scan all of the queues.
 */
static void ldlm_extent_policy(struct ldlm_resource *res,
			       struct ldlm_lock *lock, __u64 *flags)
{
	struct ldlm_extent new_ex = { .start = 0, .end = OBD_OBJECT_EOF };

	if (lock->l_export == NULL)
		/* this is a local lock taken by server (e.g., as a part of
		 * OST-side locking, or unlink handling). Expansion doesn't
		 * make a lot of sense for local locks, because they are
		 * dropped immediately on operation completion and would only
		 * conflict with other threads.
		 */
		return;

	if (lock->l_policy_data.l_extent.start == 0 &&
	    lock->l_policy_data.l_extent.end == OBD_OBJECT_EOF)
		/* fast-path whole file locks */
		return;

	/* Because reprocess_queue zeroes flags and uses it to return
	 * LDLM_FL_LOCK_CHANGED, we must check for the NO_EXPANSION flag
	 * in the lock flags rather than the 'flags' argument
	 */
	if (likely(!(lock->l_flags & LDLM_FL_NO_EXPANSION))) {
		ldlm_extent_internal_policy_granted(lock, &new_ex);
		ldlm_extent_internal_policy_waiting(lock, &new_ex);
	} else {
		LDLM_DEBUG(lock, "Not expanding manually requested lock");
		new_ex.start = lock->l_policy_data.l_extent.start;
		new_ex.end = lock->l_policy_data.l_extent.end;
		/* In case the request is not on correct boundaries, we call
		 * fixup. (normally called in ldlm_extent_internal_policy_*)
		 */
		ldlm_extent_internal_policy_fixup(lock, &new_ex, 0);
	}

	if (!ldlm_extent_equal(&new_ex, &lock->l_policy_data.l_extent)) {
		*flags |= LDLM_FL_LOCK_CHANGED;
		lock->l_policy_data.l_extent.start = new_ex.start;
		lock->l_policy_data.l_extent.end = new_ex.end;
	}
}

static bool ldlm_check_contention(struct ldlm_lock *lock, int contended_locks)
{
	struct ldlm_resource *res = lock->l_resource;
	time64_t now = ktime_get_seconds();

	if (CFS_FAIL_CHECK(OBD_FAIL_LDLM_SET_CONTENTION))
		return true;

	CDEBUG(D_DLMTRACE, "contended locks = %d\n", contended_locks);
	if (contended_locks > ldlm_res_to_ns(res)->ns_contended_locks)
		res->lr_contention_time = now;

	return now < res->lr_contention_time +
		ldlm_res_to_ns(res)->ns_contention_time;
}

struct ldlm_extent_compat_args {
	struct list_head *work_list;
	struct ldlm_lock *lock;
	enum ldlm_mode mode;
	int *locks;
	int *compat;
};

static bool ldlm_extent_compat_cb(struct ldlm_lock *lock, void *data)
{
	struct ldlm_extent_compat_args *priv = data;
	struct list_head *work_list = priv->work_list;
	struct ldlm_lock *enq = priv->lock;
	enum ldlm_mode mode = priv->mode;

	ENTRY;
	/* interval tree is for granted lock */
	LASSERTF(mode == lock->l_granted_mode,
		 "mode = %s, lock->l_granted_mode = %s\n",
		 ldlm_lockname[mode],
		 ldlm_lockname[lock->l_granted_mode]);
	if (lock->l_blocking_ast && lock->l_granted_mode != LCK_GROUP)
		ldlm_add_ast_work_item(lock, enq, work_list);

	/* don't count conflicting glimpse locks */
	if (!(mode == LCK_PR && lock->l_policy_data.l_extent.start == 0 &&
	      lock->l_policy_data.l_extent.end == OBD_OBJECT_EOF))
		*priv->locks += 1;

	if (priv->compat)
		*priv->compat = 0;

	RETURN(false);
}

/**
 * Determine if the lock is compatible with all locks on the queue.
 *
 * If \a work_list is provided, conflicting locks are linked there.
 * If \a work_list is not provided, we exit this function on first conflict.
 *
 * \retval 0 if the lock is not compatible
 * \retval 1 if the lock is compatible
 * \retval 2 if \a req is a group lock and it is compatible and requires
 *           no further checking
 * \retval negative error, such as EAGAIN for group locks
 */
static int
ldlm_extent_compat_queue(struct list_head *queue, struct ldlm_lock *req,
			 __u64 *flags, struct list_head *work_list,
			 int *contended_locks)
{
	struct ldlm_resource *res = req->l_resource;
	enum ldlm_mode req_mode = req->l_req_mode;
	__u64 req_start = req->l_req_extent.start;
	__u64 req_end = req->l_req_extent.end;
	struct ldlm_lock *lock;
	int check_contention;
	int compat = 1;

	ENTRY;

	lockmode_verify(req_mode);

	/* Using interval tree for granted lock */
	if (queue == &res->lr_granted) {
		struct ldlm_interval_tree *tree;
		struct ldlm_extent_compat_args data = {.work_list = work_list,
						       .lock = req,
						       .locks = contended_locks,
						       .compat = &compat };
		int idx;

		for (idx = 0; idx < LCK_MODE_NUM; idx++) {
			tree = &res->lr_itree[idx];
			if (INTERVAL_TREE_EMPTY(&tree->lit_root))
				continue;

			data.mode = tree->lit_mode;
			if (lockmode_compat(req_mode, tree->lit_mode)) {
				struct ldlm_lock *lock;

				if (req_mode != LCK_GROUP)
					continue;

				/* group lock, grant it immediately if
				 * compatible
				 */
				lock = extent_top(tree);
				if (req->l_policy_data.l_extent.gid ==
				    lock->l_policy_data.l_extent.gid)
					RETURN(2);
			}

			if (tree->lit_mode == LCK_GROUP) {
				if (*flags & (LDLM_FL_BLOCK_NOWAIT |
				    LDLM_FL_SPECULATIVE)) {
					compat = -EAGAIN;
					goto destroylock;
				}

				if (!work_list)
					RETURN(0);

				/* if work list is not NULL, add all
				 * locks in the tree to work list.
				 */
				compat = 0;
				for (lock = extent_first(tree);
				     lock;
				     lock = extent_next_lock(lock))
					ldlm_extent_compat_cb(lock, &data);
				continue;
			}

			/* We've found a potentially blocking lock, check
			 * compatibility.  This handles locks other than GROUP
			 * locks, which are handled separately above.
			 *
			 * Locks with FL_SPECULATIVE are asynchronous requests
			 * which must never wait behind another lock, so they
			 * fail if any conflicting lock is found.
			 */
			if (!work_list || (*flags & LDLM_FL_SPECULATIVE)) {
				if (extent_iter_first(&tree->lit_root,
						      req_start, req_end)) {
					if (!work_list) {
						RETURN(0);
					} else {
						compat = -EAGAIN;
						goto destroylock;
					}
				}
			} else {
				ldlm_extent_search(&tree->lit_root,
						   req_start, req_end,
						   ldlm_extent_compat_cb,
						   &data);
				if (!list_empty(work_list) && compat)
					compat = 0;
			}
		}
	} else { /* for waiting queue */
		list_for_each_entry(lock, queue, l_res_link) {
			check_contention = 1;

			/* We stop walking the queue if we hit ourselves so
			 * we don't take conflicting locks enqueued after us
			 * into account, or we'd wait forever.
			 */
			if (req == lock)
				break;

			/* locks are compatible, overlap doesn't matter */
			if (lockmode_compat(lock->l_req_mode, req_mode)) {
				if (req_mode == LCK_PR &&
				    ((lock->l_policy_data.l_extent.start <=
				      req->l_policy_data.l_extent.start) &&
				     (lock->l_policy_data.l_extent.end >=
				      req->l_policy_data.l_extent.end))) {
					/* If we met a PR lock just like us or
					 * wider, and nobody down the list
					 * conflicted with it, that means we
					 * can skip processing of the rest of
					 * the list and safely place ourselves
					 * at the end of the list, or grant
					 * (dependent if we met an conflicting
					 * locks before in the list).  In case
					 * of 1st enqueue only we continue
					 * traversing if there is something
					 * conflicting down the list because
					 * we need to make sure that something
					 * is marked as AST_SENT as well, in
					 * case of empy worklist we would exit
					 * on first conflict met.
					 */

					/* There IS a case where such flag is
					 * not set for a lock, yet it blocks
					 * something.  Luckily for us this is
					 * only during destroy, so lock is
					 * exclusive.  So here we are safe
					 */
					if (!ldlm_is_ast_sent(lock))
						RETURN(compat);
				}

				/* non-group locks are compatible,
				 * overlap doesn't matter
				 */
				if (likely(req_mode != LCK_GROUP))
					continue;

				/* If we are trying to get a GROUP lock and
				 * there is another one of this kind, we need
				 * to compare gid
				 */
				if (req->l_policy_data.l_extent.gid ==
				lock->l_policy_data.l_extent.gid) {
					/* If existing lock with matched gid
					 * is granted, we grant new one too.
					 */
					if (ldlm_is_granted(lock))
						RETURN(2);

					/* Otherwise we are scanning queue of
					 * waiting locks and it means current
					 * request would block along with
					 * existing lock (that is already
					 * blocked.  If we are in nonblocking
					 * mode - return immediately
					 */
					if (*flags & (LDLM_FL_BLOCK_NOWAIT |
						      LDLM_FL_SPECULATIVE)) {
						compat = -EAGAIN;
						goto destroylock;
					}
					/* If this group lock is compatible with
					 * another group lock on the waiting
					 * list, they must be together in the
					 * list, so they can be granted at the
					 * same time.  Otherwise the later lock
					 * can get stuck behind another,
					 * incompatible, lock.
					 */
					ldlm_resource_insert_lock_after(lock,
									req);
					/* Because 'lock' is not granted, we can
					 * stop processing this queue and return
					 * immediately. There is no need to
					 * check the rest of the list.
					 */
					RETURN(0);
				}
			}

			if (unlikely(req_mode == LCK_GROUP &&
			    !ldlm_is_granted(lock))) {
				compat = 0;
				if (lock->l_req_mode != LCK_GROUP) {
					/* Ok, we hit non-GROUP lock,
					 * there
					 * should be no more GROUP
					 * locks later
					 * on, queue in
					 * front of first
					 * non-GROUP lock
					 */
					ldlm_resource_insert_lock_before(lock,
									 req);
					break;
				}
				LASSERT(req->l_policy_data.l_extent.gid !=
					lock->l_policy_data.l_extent.gid);
				continue;
			}

			if (unlikely(lock->l_req_mode == LCK_GROUP)) {
				/* If compared lock is GROUP, then requested is
				 * PR/PW so this is not compatible; extent
				 * range does not matter
				 */
				if (*flags & (LDLM_FL_BLOCK_NOWAIT |
				    LDLM_FL_SPECULATIVE)) {
					compat = -EAGAIN;
					goto destroylock;
				}
			} else if (lock->l_policy_data.l_extent.end < req_start ||
				   lock->l_policy_data.l_extent.start > req_end) {
				/* if non group lock doesn't overlap skip it */
				continue;
			} else if (lock->l_req_extent.end < req_start ||
				   lock->l_req_extent.start > req_end) {
				/* false contention, the requests doesn't
				 * really overlap
				 */
				check_contention = 0;
			}

			if (!work_list)
				RETURN(0);

			if (*flags & LDLM_FL_SPECULATIVE) {
				compat = -EAGAIN;
				goto destroylock;
			}

			/* don't count conflicting glimpse locks */
			if (lock->l_req_mode == LCK_PR &&
			    lock->l_policy_data.l_extent.start == 0 &&
			     lock->l_policy_data.l_extent.end == OBD_OBJECT_EOF)
				check_contention = 0;

			*contended_locks += check_contention;

			compat = 0;
			if (lock->l_blocking_ast &&
			    lock->l_req_mode != LCK_GROUP)
				ldlm_add_ast_work_item(lock, req, work_list);
		}
	}

	if (ldlm_check_contention(req, *contended_locks) &&
	    compat == 0 &&
	    (*flags & LDLM_FL_DENY_ON_CONTENTION) &&
	    req->l_req_mode != LCK_GROUP &&
	    req_end - req_start <=
	    ldlm_res_to_ns(req->l_resource)->ns_max_nolock_size)
		GOTO(destroylock, compat = -EUSERS);

	RETURN(compat);
destroylock:
	list_del_init(&req->l_res_link);
	if (ldlm_is_local(req))
		ldlm_lock_decref_internal_nolock(req, req_mode);
	ldlm_lock_destroy_nolock(req);
	RETURN(compat);
}

/**
 * This function refresh eviction timer for cancelled lock.
 * \param[in] lock		ldlm lock for refresh
 * \param[in] arg		ldlm prolong arguments, timeout, export, extent
 *				and counter are used
 */
void ldlm_lock_prolong_one(struct ldlm_lock *lock,
			   struct ldlm_prolong_args *arg)
{
	timeout_t timeout;

	CFS_FAIL_TIMEOUT(OBD_FAIL_LDLM_PROLONG_PAUSE, 3);

	if (arg->lpa_export != lock->l_export ||
	    lock->l_flags & LDLM_FL_DESTROYED)
		/* ignore unrelated locks */
		return;

	arg->lpa_locks_cnt++;

	if (!(lock->l_flags & LDLM_FL_AST_SENT))
		/* ignore locks not being cancelled */
		return;

	arg->lpa_blocks_cnt++;

	/* OK. this is a possible lock the user holds doing I/O
	 * let's refresh eviction timer for it.
	 */
	timeout = ptlrpc_export_prolong_timeout(arg->lpa_req, false);
	LDLM_DEBUG(lock, "refreshed to %ds. ", timeout);
	ldlm_refresh_waiting_lock(lock, timeout);
}
EXPORT_SYMBOL(ldlm_lock_prolong_one);

static bool ldlm_resource_prolong_cb(struct ldlm_lock *lock, void *data)
{
	struct ldlm_prolong_args *arg = data;

	ENTRY;
	ldlm_lock_prolong_one(lock, arg);

	RETURN(false);
}

/**
 * Walk through granted tree and prolong locks if they overlaps extent.
 *
 * \param[in] arg		prolong args
 */
void ldlm_resource_prolong(struct ldlm_prolong_args *arg)
{
	struct ldlm_interval_tree *tree;
	struct ldlm_resource *res;
	int idx;

	ENTRY;

	res = ldlm_resource_get(arg->lpa_export->exp_obd->obd_namespace,
				&arg->lpa_resid, LDLM_EXTENT, 0);
	if (IS_ERR(res)) {
		CDEBUG(D_DLMTRACE, "Failed to get resource for resid %llu/%llu\n",
		arg->lpa_resid.name[0], arg->lpa_resid.name[1]);
		RETURN_EXIT;
	}

	lock_res(res);
	for (idx = 0; idx < LCK_MODE_NUM; idx++) {
		tree = &res->lr_itree[idx];
		if (INTERVAL_TREE_EMPTY(&tree->lit_root))
			continue;

		/* There is no possibility to check for the groupID
		 * so all the group locks are considered as valid
		 * here, especially because the client is supposed
		 * to check it has such a lock before sending an RPC.
		 */
		if (!(tree->lit_mode & arg->lpa_mode))
			continue;

		ldlm_extent_search(&tree->lit_root, arg->lpa_extent.start,
				 arg->lpa_extent.end,
				 ldlm_resource_prolong_cb, arg);
	}
	unlock_res(res);
	ldlm_resource_putref(res);

	EXIT;
}
EXPORT_SYMBOL(ldlm_resource_prolong);

/**
 * Process a granting attempt for extent lock.
 * Must be called with ns lock held.
 *
 * This function looks for any conflicts for \a lock in the granted or
 * waiting queues. The lock is granted if no conflicts are found in
 * either queue.
 */
int ldlm_process_extent_lock(struct ldlm_lock *lock, __u64 *flags,
		enum ldlm_process_intention intention,
		enum ldlm_error *err, struct list_head *work_list)
{
	struct ldlm_resource *res = lock->l_resource;
	int rc, rc2 = 0;
	int contended_locks = 0;
	struct list_head *grant_work = intention == LDLM_PROCESS_ENQUEUE ?
							NULL : work_list;
	ENTRY;

	LASSERT(!ldlm_is_granted(lock));
	LASSERT(!(*flags & LDLM_FL_DENY_ON_CONTENTION) ||
		!ldlm_is_ast_discard_data(lock));
	check_res_locked(res);
	*err = ELDLM_OK;

	if (intention == LDLM_PROCESS_RESCAN) {
		/* Careful observers will note that we don't handle -EAGAIN
		 * here, but it's ok for a non-obvious reason -- compat_queue
		 * can only return -EAGAIN if (flags & BLOCK_NOWAIT |
		 * SPECULATIVE). flags should always be zero here, and if that
		 * ever stops being true, we want to find out. */
		LASSERT(*flags == 0);
		rc = ldlm_extent_compat_queue(&res->lr_granted, lock, flags,
					NULL, &contended_locks);
		if (rc == 1) {
			rc = ldlm_extent_compat_queue(&res->lr_waiting, lock,
						flags, NULL,
						&contended_locks);
		}
		if (rc == 0)
			RETURN(LDLM_ITER_STOP);

		ldlm_resource_unlink_lock(lock);

		if (!CFS_FAIL_CHECK(OBD_FAIL_LDLM_CANCEL_EVICT_RACE))
			ldlm_extent_policy(res, lock, flags);
		ldlm_grant_lock(lock, grant_work);
		RETURN(LDLM_ITER_CONTINUE);
	}

	contended_locks = 0;
	rc = ldlm_extent_compat_queue(&res->lr_granted, lock, flags,
				work_list, &contended_locks);
	if (rc < 0)
		GOTO(out, *err = rc);

	if (rc != 2) {
		rc2 = ldlm_extent_compat_queue(&res->lr_waiting, lock,
					flags, work_list,
					&contended_locks);
		if (rc2 < 0)
			GOTO(out, *err = rc = rc2);
	}

	if (rc + rc2 == 2) {
		ldlm_extent_policy(res, lock, flags);
		ldlm_resource_unlink_lock(lock);
		ldlm_grant_lock(lock, grant_work);
	} else {
		/* Adding LDLM_FL_NO_TIMEOUT flag to granted lock to
		 * force client to wait for the lock endlessly once
		 * the lock is enqueued -bzzz */
		*flags |= LDLM_FL_NO_TIMEOUT;
	}

	RETURN(LDLM_ITER_CONTINUE);
out:
	return rc;
}
#endif /* HAVE_SERVER_SUPPORT */

/* When a lock is cancelled by a client, the KMS may undergo change if this
 * is the "highest lock".  This function returns the new KMS value, updating
 * it only if we were the highest lock.
 *
 * Caller must hold lr_lock already.
 *
 * NB: A lock on [x,y] protects a KMS of up to y + 1 bytes!
 */
__u64 ldlm_extent_shift_kms(struct ldlm_lock *lock, __u64 old_kms)
{
	struct ldlm_resource *res = lock->l_resource;
	struct ldlm_interval_tree *tree;
	struct ldlm_lock *lck;
	__u64 kms = 0;
	int idx = 0;
	bool complete = false;

	ENTRY;

	/* don't let another thread in ldlm_extent_shift_kms race in
	 * just after we finish and take our lock into account in its
	 * calculation of the kms
	 */
	ldlm_set_kms_ignore(lock);

	/* We iterate over the lock trees, looking for the largest kms
	 * smaller than the current one.
	 */
	for (idx = 0; idx < LCK_MODE_NUM; idx++) {
		tree = &res->lr_itree[idx];

		/* If our already known kms is >= than the highest 'end' in
		 * this tree, we don't need to check this tree, because
		 * the kms from a tree can be lower than in_max_high (due to
		 * kms_ignore), but it can never be higher. */
		lck = extent_top(tree);
		if (!lck || kms >= lck->l_subtree_last)
			continue;

		for (lck = extent_last(tree);
		     lck;
		     lck = extent_prev(lck)) {
			if (ldlm_is_kms_ignore(lck)) {
				struct ldlm_lock *lk;

				list_for_each_entry(lk, &lck->l_same_extent,
						    l_same_extent)
					if (!ldlm_is_kms_ignore(lk)) {
						lk = NULL;
						break;
					}
				if (lk)
					continue;
			}

			/* If this lock has a greater or equal kms, we are not
			 * the highest lock (or we share that distinction
			 * with another lock), and don't need to update KMS.
			 * Record old_kms and stop looking.
			 */
			if (lck->l_policy_data.l_extent.end == OBD_OBJECT_EOF ||
			    lck->l_policy_data.l_extent.end + 1 >= old_kms) {
				kms = old_kms;
				complete = true;
				break;
			}
			if (lck->l_policy_data.l_extent.end + 1 > kms)
				kms = lck->l_policy_data.l_extent.end + 1;

			/* Since we start with the highest lock and work
			 * down, for PW locks, we only need to check if we
			 * should update the kms, then stop walking the tree.
			 * PR locks are not exclusive, so the highest start
			 * does not imply the highest end and we must
			 * continue.  (Only one group lock is allowed per
			 * resource, so this is irrelevant for group locks.)
			 */
			if (lck->l_granted_mode == LCK_PW)
				break;
		}

		/* This tells us we're not the highest lock, so we don't need
		 * to check the remaining trees
		 */
		if (complete)
			break;
	}

	LASSERTF(kms <= old_kms, "kms %llu old_kms %llu\n", kms, old_kms);

	RETURN(kms);
}
EXPORT_SYMBOL(ldlm_extent_shift_kms);

static inline int ldlm_mode_to_index(enum ldlm_mode mode)
{
	int index;

	LASSERT(mode != 0);
	LASSERT(is_power_of_2(mode));
	index = ilog2(mode);
	LASSERT(index < LCK_MODE_NUM);
	return index;
}

/** Add newly granted lock into interval tree for the resource. */
void ldlm_extent_add_lock(struct ldlm_resource *res,
			  struct ldlm_lock *lock)
{
	struct ldlm_interval_tree *tree;
	struct ldlm_lock *orig;
	int idx;

	LASSERT(ldlm_is_granted(lock));

	LASSERT(RB_EMPTY_NODE(&lock->l_rb));
	LASSERT(list_empty(&lock->l_same_extent));

	idx = ldlm_mode_to_index(lock->l_granted_mode);
	LASSERT(lock->l_granted_mode == BIT(idx));
	LASSERT(lock->l_granted_mode == res->lr_itree[idx].lit_mode);

	tree = &res->lr_itree[idx];
	orig = extent_insert_unique(lock, &tree->lit_root);
	if (orig)
		list_add(&lock->l_same_extent, &orig->l_same_extent);
	tree->lit_size++;

	/* even though we use interval tree to manage the extent lock, we also
	 * add the locks into grant list, for debug purpose, ..
	 */
	ldlm_resource_add_lock(res, &res->lr_granted, lock);

	if (CFS_FAIL_CHECK(OBD_FAIL_LDLM_GRANT_CHECK)) {
		struct ldlm_lock *lck;

		list_for_each_entry_reverse(lck, &res->lr_granted, l_res_link) {
			if (lck == lock)
				continue;
			if (lockmode_compat(lck->l_granted_mode,
					lock->l_granted_mode))
				continue;
			if (ldlm_extent_overlap(&lck->l_req_extent,
						&lock->l_req_extent)) {
				CDEBUG(D_ERROR,
				       "granting conflicting lock %p %p\n",
				       lck, lock);
				ldlm_resource_dump(D_ERROR, res);
				LBUG();
			}
		}
	}
}

/** Remove cancelled lock from resource interval tree. */
void ldlm_extent_unlink_lock(struct ldlm_lock *lock)
{
	struct ldlm_resource *res = lock->l_resource;
	struct ldlm_interval_tree *tree;
	int idx;

	if (RB_EMPTY_NODE(&lock->l_rb) &&
	    list_empty(&lock->l_same_extent)) /* duplicate unlink */
		return;

	idx = ldlm_mode_to_index(lock->l_granted_mode);
	LASSERT(lock->l_granted_mode == BIT(idx));
	tree = &res->lr_itree[idx];

	LASSERT(!INTERVAL_TREE_EMPTY(&tree->lit_root));

	tree->lit_size--;

	if (RB_EMPTY_NODE(&lock->l_rb)) {
		list_del_init(&lock->l_same_extent);
	} else if (list_empty(&lock->l_same_extent)) {
		extent_remove(lock, &tree->lit_root);
		RB_CLEAR_NODE(&lock->l_rb);
	} else {
		struct ldlm_lock *next = list_next_entry(lock, l_same_extent);
		list_del_init(&lock->l_same_extent);
		extent_remove(lock, &tree->lit_root);
		RB_CLEAR_NODE(&lock->l_rb);
		extent_insert(next, &tree->lit_root);
	}
}

void ldlm_extent_policy_wire_to_local(const union ldlm_wire_policy_data *wpolicy,
				      union ldlm_policy_data *lpolicy)
{
	lpolicy->l_extent.start = wpolicy->l_extent.start;
	lpolicy->l_extent.end = wpolicy->l_extent.end;
	lpolicy->l_extent.gid = wpolicy->l_extent.gid;
}

void ldlm_extent_policy_local_to_wire(const union ldlm_policy_data *lpolicy,
				      union ldlm_wire_policy_data *wpolicy)
{
	memset(wpolicy, 0, sizeof(*wpolicy));
	wpolicy->l_extent.start = lpolicy->l_extent.start;
	wpolicy->l_extent.end = lpolicy->l_extent.end;
	wpolicy->l_extent.gid = lpolicy->l_extent.gid;
}
void ldlm_extent_search(struct interval_tree_root *root,
			u64 start, u64 end,
			bool (*matches)(struct ldlm_lock *lock, void *data),
			void *data)
{
	struct ldlm_lock *lock, *lock2;

	for (lock = extent_iter_first(root, start, end);
	     lock;
	     lock = extent_iter_next(lock, start, end)) {
		if (matches(lock, data))
			return;
		list_for_each_entry(lock2, &lock->l_same_extent, l_same_extent)
			if (matches(lock2, data))
				return;
	}
}
