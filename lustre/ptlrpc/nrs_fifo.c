// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2011, 2014, Intel Corporation.
 *
 * Copyright 2012 Xyratex Technology Limited
 */

/*
 * Network Request Scheduler (NRS) FIFO policy
 *
 * Handles RPCs in a FIFO manner, as received from the network. This policy is
 * a logical wrapper around previous, non-NRS functionality. It is used as the
 * default and fallback policy for all types of RPCs on all PTLRPC service
 * partitions, for both regular and high-priority NRS heads. Default here means
 * the policy is the one enabled at PTLRPC service partition startup time, and
 * fallback means the policy is used to handle RPCs that are not handled
 * successfully or are not handled at all by any primary policy that may be
 * enabled on a given NRS head.
 *
 * Author: Liang Zhen <liang@whamcloud.com>
 * Author: Nikitas Angelinas <nikitas_angelinas@xyratex.com>
 */

#define DEBUG_SUBSYSTEM S_RPC
#include <obd_support.h>
#include <obd_class.h>
#include <libcfs/libcfs.h>
#include "ptlrpc_internal.h"

/*
 * fifo
 *
 * The FIFO policy is a logical wrapper around previous, non-NRS functionality.
 * It schedules RPCs in the same order as they are queued from LNet.
 */

#define NRS_POL_NAME_FIFO	"fifo"

/**
 * nrs_fifo_start() - Policy start
 * @policy: The policy to start
 * @arg: unused
 *
 * Is called before the policy transitions into
 * ptlrpc_nrs_pol_state::NRS_POL_STATE_STARTED; allocates and initializes a
 * policy-specific private data structure.
 * see nrs_policy_register()
 * see nrs_policy_ctl()
 *
 * Return:
 * * %0 on success
 * * %-ENOMEM on OOM error
 */
static int nrs_fifo_start(struct ptlrpc_nrs_policy *policy, char *arg)
{
	struct nrs_fifo_head *head;

	OBD_CPT_ALLOC_PTR(head, nrs_pol2cptab(policy), nrs_pol2cptid(policy));
	if (head == NULL)
		return -ENOMEM;

	INIT_LIST_HEAD(&head->fh_list);
	policy->pol_private = head;
	return 0;
}

/**
 * nrs_fifo_stop() - Stop policy
 * @policy: The policy to stop
 *
 * Is called before the policy transitions into
 * ptlrpc_nrs_pol_state::NRS_POL_STATE_STOPPED; deallocates the policy-specific
 * private data structure.  see nrs_policy_stop0()
 */
static void nrs_fifo_stop(struct ptlrpc_nrs_policy *policy)
{
	struct nrs_fifo_head *head = policy->pol_private;

	LASSERT(head != NULL);
	LASSERT(list_empty(&head->fh_list));

	OBD_FREE_PTR(head);
}

/**
 * nrs_fifo_res_get() - Is called for obtaining a FIFO policy resource.
 * @policy: The policy on which the request is being asked for
 * @nrq: The request for which resources are being taken
 * @parent: Parent resource, unused in this policy
 * @resp: Resources references are placed in this array [out]
 * @moving_req: Signifies limited caller context; unused in this policy
 *
 * see nrs_resource_get_safe()
 *
 * Return:
 * * %1 The FIFO policy only has a one-level resource hierarchy, as since
 *	it implements a simple scheduling algorithm in which request
 *	priority is determined on the request arrival order, it does not
 *	need to maintain a set of resources that would otherwise be used
 *	to calculate a request's priority.
 */
static int nrs_fifo_res_get(struct ptlrpc_nrs_policy *policy,
			    struct ptlrpc_nrs_request *nrq,
			    const struct ptlrpc_nrs_resource *parent,
			    struct ptlrpc_nrs_resource **resp, bool moving_req)
{
	/**
	 * Just return the resource embedded inside nrs_fifo_head, and end this
	 * resource hierarchy reference request.
	 */
	*resp = &((struct nrs_fifo_head *)policy->pol_private)->fh_res;
	return 1;
}

/**
 * nrs_fifo_req_get() - Get a rquest from the FIFO policy
 * @policy: The policy
 * @peek: When set, signifies that we just want to examine the request, and not
 *	handle it, so the request is not removed
 *	from the policy.
 * @force: Force the policy to return a request; unused in this policy
 *
 * Called when getting a request from the FIFO policy for handling, or just
 * peeking; removes the request from the policy when it is to be handled.
 * see ptlrpc_nrs_req_get_nolock()
 * see nrs_request_get()
 *
 * Returns The request to be handled; this is the next request in FIFO queue
 *
 */
static
struct ptlrpc_nrs_request * nrs_fifo_req_get(struct ptlrpc_nrs_policy *policy,
					     bool peek, bool force)
{
	struct nrs_fifo_head	  *head = policy->pol_private;
	struct ptlrpc_nrs_request *nrq;

	nrq = list_first_entry_or_null(&head->fh_list,
				       struct ptlrpc_nrs_request,
				       nr_u.fifo.fr_list);

	if (likely(!peek && nrq != NULL)) {
		struct ptlrpc_request *req = container_of(nrq,
							  struct ptlrpc_request,
							  rq_nrq);

		list_del_init(&nrq->nr_u.fifo.fr_list);

		CDEBUG(D_RPCTRACE, "NRS start %s request from %s, seq: %llu"
		       "\n", policy->pol_desc->pd_name,
		       libcfs_idstr(&req->rq_peer), nrq->nr_u.fifo.fr_sequence);
	}

	return nrq;
}

/**
 * nrs_fifo_req_add() - Adds request @nrq to @policy's list of queued requests
 * @policy: The policy
 * @nrq: The request to add
 *
 * Returns 0 on success; nrs_request_enqueue() assumes this function will always
 *	succeed
 */
static int nrs_fifo_req_add(struct ptlrpc_nrs_policy *policy,
			    struct ptlrpc_nrs_request *nrq)
{
	struct nrs_fifo_head *head;

	head = container_of(nrs_request_resource(nrq), struct nrs_fifo_head,
			    fh_res);
	/**
	 * Only used for debugging
	 */
	nrq->nr_u.fifo.fr_sequence = head->fh_sequence++;
	list_add_tail(&nrq->nr_u.fifo.fr_list, &head->fh_list);

	return 0;
}

/**
 * nrs_fifo_req_del() - Removes request @nrq from @policy list of queued request
 * @policy: The policy
 * @nrq: The request to remove
 */
static void nrs_fifo_req_del(struct ptlrpc_nrs_policy *policy,
			     struct ptlrpc_nrs_request *nrq)
{
	LASSERT(!list_empty(&nrq->nr_u.fifo.fr_list));
	list_del_init(&nrq->nr_u.fifo.fr_list);
}

/**
 * nrs_fifo_req_stop() - Prints a debug statement right before the request @nrq
 * stops being handled.
 * @policy: The policy handling the request
 * @nrq: The request being handled
 *
 * see ptlrpc_server_finish_request()
 * see ptlrpc_nrs_req_stop_nolock()
 */
static void nrs_fifo_req_stop(struct ptlrpc_nrs_policy *policy,
			      struct ptlrpc_nrs_request *nrq)
{
	struct ptlrpc_request *req = container_of(nrq, struct ptlrpc_request,
						  rq_nrq);

	CDEBUG(D_RPCTRACE, "NRS stop %s request from %s, seq: %llu\n",
	       policy->pol_desc->pd_name, libcfs_idstr(&req->rq_peer),
	       nrq->nr_u.fifo.fr_sequence);
}

/*
 * FIFO policy operations
 */
static const struct ptlrpc_nrs_pol_ops nrs_fifo_ops = {
	.op_policy_start	= nrs_fifo_start,
	.op_policy_stop		= nrs_fifo_stop,
	.op_res_get		= nrs_fifo_res_get,
	.op_req_get		= nrs_fifo_req_get,
	.op_req_enqueue		= nrs_fifo_req_add,
	.op_req_dequeue		= nrs_fifo_req_del,
	.op_req_stop		= nrs_fifo_req_stop,
};

/*
 * FIFO policy configuration
 */
struct ptlrpc_nrs_pol_conf nrs_conf_fifo = {
	.nc_name		= NRS_POL_NAME_FIFO,
	.nc_ops			= &nrs_fifo_ops,
	.nc_compat		= nrs_policy_compat_all,
	.nc_flags		= PTLRPC_NRS_FL_FALLBACK |
				  PTLRPC_NRS_FL_REG_START
};

/** @} fifo */

/** @} nrs */

