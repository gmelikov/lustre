// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Test client & Server
 *
 * Author: Liang Zhen <liangzhen@clusterfs.com>
 */

#include "selftest.h"

#define LST_PING_TEST_MAGIC     0xbabeface

static int ping_srv_workitems = SFW_TEST_WI_MAX;
module_param(ping_srv_workitems, int, 0644);
MODULE_PARM_DESC(ping_srv_workitems, "# PING server workitems");

struct lst_ping_data {
	spinlock_t	pnd_lock;	/* serialize */
	int		pnd_counter;	/* sequence counter */
};

static struct lst_ping_data lst_ping_data;

static int
ping_client_init(struct sfw_test_instance *tsi)
{
	struct sfw_session *sn = tsi->tsi_batch->bat_session;

	LASSERT(tsi->tsi_is_client);
	LASSERT(sn != NULL && (sn->sn_features & ~LST_FEATS_MASK) == 0);

	spin_lock_init(&lst_ping_data.pnd_lock);
	lst_ping_data.pnd_counter = 0;

	return 0;
}

static void
ping_client_fini(struct sfw_test_instance *tsi)
{
	struct sfw_session *sn = tsi->tsi_batch->bat_session;
	int errors;

	LASSERT(sn != NULL);
	LASSERT(tsi->tsi_is_client);

	errors = atomic_read(&sn->sn_ping_errors);
	if (errors)
		CWARN("%d pings have failed.\n", errors);
	else
		CDEBUG(D_NET, "Ping test finished OK.\n");
}

static int
ping_client_prep_rpc(struct sfw_test_unit *tsu, struct lnet_process_id dest,
		     struct srpc_client_rpc **rpc)
{
	struct srpc_ping_reqst *req;
	struct sfw_test_instance *tsi = tsu->tsu_instance;
	struct sfw_session *sn = tsi->tsi_batch->bat_session;
	struct timespec64 ts;
	int rc;

	LASSERT(sn != NULL);
	LASSERT((sn->sn_features & ~LST_FEATS_MASK) == 0);

	rc = sfw_create_test_rpc(tsu, dest, sn->sn_features, 0, 0, rpc);
	if (rc != 0)
		return rc;

	req = &(*rpc)->crpc_reqstmsg.msg_body.ping_reqst;

	req->pnr_magic = LST_PING_TEST_MAGIC;

	spin_lock(&lst_ping_data.pnd_lock);
	req->pnr_seq = lst_ping_data.pnd_counter++;
	spin_unlock(&lst_ping_data.pnd_lock);

	ktime_get_real_ts64(&ts);
	req->pnr_time_sec  = ts.tv_sec;
	req->pnr_time_nsec = ts.tv_nsec;

	return rc;
}

static void
ping_client_done_rpc(struct sfw_test_unit *tsu, struct srpc_client_rpc *rpc)
{
	struct sfw_test_instance *tsi = tsu->tsu_instance;
	struct sfw_session *sn = tsi->tsi_batch->bat_session;
	struct srpc_ping_reqst *reqst = &rpc->crpc_reqstmsg.msg_body.ping_reqst;
	struct srpc_ping_reply *reply = &rpc->crpc_replymsg.msg_body.ping_reply;
	struct timespec64 ts;

	LASSERT(sn != NULL);

	if (rpc->crpc_status != 0) {
		if (!tsi->tsi_stopping) /* rpc could have been aborted */
			atomic_inc(&sn->sn_ping_errors);
		CERROR("Unable to ping %s (%d): %d\n",
		       libcfs_id2str(rpc->crpc_dest),
		       reqst->pnr_seq, rpc->crpc_status);
		return;
	}

	if (rpc->crpc_replymsg.msg_magic != SRPC_MSG_MAGIC) {
		__swab32s(&reply->pnr_seq);
		__swab32s(&reply->pnr_magic);
		__swab32s(&reply->pnr_status);
	}

	if (reply->pnr_magic != LST_PING_TEST_MAGIC) {
		rpc->crpc_status = -EBADMSG;
		atomic_inc(&sn->sn_ping_errors);
		CERROR("Bad magic %u from %s, %u expected.\n",
		       reply->pnr_magic, libcfs_id2str(rpc->crpc_dest),
		       LST_PING_TEST_MAGIC);
		return;
	}

	if (reply->pnr_seq != reqst->pnr_seq) {
		rpc->crpc_status = -EBADMSG;
		atomic_inc(&sn->sn_ping_errors);
		CERROR("Bad seq %u from %s, %u expected.\n",
		       reply->pnr_seq, libcfs_id2str(rpc->crpc_dest),
		       reqst->pnr_seq);
		return;
	}

	ktime_get_real_ts64(&ts);
	CDEBUG(D_NET, "%d reply in %llu nsec\n", reply->pnr_seq,
	       (u64)((ts.tv_sec - reqst->pnr_time_sec) * NSEC_PER_SEC +
		    (ts.tv_nsec - reqst->pnr_time_nsec)));
}

static int
ping_server_handle(struct srpc_server_rpc *rpc)
{
	struct srpc_service *sv  = rpc->srpc_scd->scd_svc;
	struct srpc_msg *reqstmsg = &rpc->srpc_reqstbuf->buf_msg;
	struct srpc_msg *replymsg = &rpc->srpc_replymsg;
	struct srpc_ping_reqst *req = &reqstmsg->msg_body.ping_reqst;
	struct srpc_ping_reply *rep = &rpc->srpc_replymsg.msg_body.ping_reply;

	LASSERT(sv->sv_id == SRPC_SERVICE_PING);

	if (reqstmsg->msg_magic != SRPC_MSG_MAGIC) {
		LASSERT(reqstmsg->msg_magic == __swab32(SRPC_MSG_MAGIC));

		__swab32s(&req->pnr_seq);
		__swab32s(&req->pnr_magic);
		__swab64s(&req->pnr_time_sec);
		__swab64s(&req->pnr_time_nsec);
	}
	LASSERT(reqstmsg->msg_type == srpc_service2request(sv->sv_id));

	if (req->pnr_magic != LST_PING_TEST_MAGIC) {
		CERROR("Unexpect magic %08x from %s\n",
		       req->pnr_magic, libcfs_id2str(rpc->srpc_peer));
		return -EINVAL;
	}

	rep->pnr_seq   = req->pnr_seq;
	rep->pnr_magic = LST_PING_TEST_MAGIC;

	if ((reqstmsg->msg_ses_feats & ~LST_FEATS_MASK) != 0) {
		replymsg->msg_ses_feats = LST_FEATS_MASK;
		rep->pnr_status = EPROTO;
		return 0;
	}

	replymsg->msg_ses_feats = reqstmsg->msg_ses_feats;

	CDEBUG(D_NET, "Get ping %d from %s\n",
	       req->pnr_seq, libcfs_id2str(rpc->srpc_peer));
	return 0;
}

struct sfw_test_client_ops ping_test_client;

void ping_init_test_client(void)
{
	ping_test_client.tso_init     = ping_client_init;
	ping_test_client.tso_fini     = ping_client_fini;
	ping_test_client.tso_prep_rpc = ping_client_prep_rpc;
	ping_test_client.tso_done_rpc = ping_client_done_rpc;
}

struct srpc_service ping_test_service;

void ping_init_test_service(void)
{
	ping_test_service.sv_id       = SRPC_SERVICE_PING;
	ping_test_service.sv_name     = "ping_test";
	ping_test_service.sv_handler  = ping_server_handle;
	ping_test_service.sv_wi_total = ping_srv_workitems;
}
