// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2007, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2015, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 */

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/random.h>
#include <libcfs/libcfs.h>

unsigned long cfs_fail_loc;
EXPORT_SYMBOL(cfs_fail_loc);

unsigned int cfs_fail_val;
EXPORT_SYMBOL(cfs_fail_val);

int cfs_fail_err;
EXPORT_SYMBOL(cfs_fail_err);

DECLARE_WAIT_QUEUE_HEAD(cfs_race_waitq);
EXPORT_SYMBOL(cfs_race_waitq);

int cfs_race_state;
EXPORT_SYMBOL(cfs_race_state);

int __cfs_fail_check_set(u32 id, u32 value, int set)
{
	static atomic_t cfs_fail_count = ATOMIC_INIT(0);

	LASSERT(!(id & CFS_FAIL_ONCE));

	if ((cfs_fail_loc & (CFS_FAILED | CFS_FAIL_ONCE)) ==
	    (CFS_FAILED | CFS_FAIL_ONCE)) {
		atomic_set(&cfs_fail_count, 0); /* paranoia */
		return 0;
	}

	/* Fail 1/cfs_fail_val times */
	if (cfs_fail_loc & CFS_FAIL_RAND) {
		if (cfs_fail_val < 2 || get_random_u32_below(cfs_fail_val) > 0)
			return 0;
	}

	/* Skip the first cfs_fail_val, then fail */
	if (cfs_fail_loc & CFS_FAIL_SKIP) {
		if (atomic_inc_return(&cfs_fail_count) <= cfs_fail_val)
			return 0;
	}

	/* check cfs_fail_val... */
	if (set == CFS_FAIL_LOC_VALUE) {
		if (cfs_fail_val != -1 && cfs_fail_val != value)
			return 0;
	}

	/* Fail cfs_fail_val times, overridden by FAIL_ONCE */
	if (cfs_fail_loc & CFS_FAIL_SOME &&
	    (!(cfs_fail_loc & CFS_FAIL_ONCE) || cfs_fail_val <= 1)) {
		int count = atomic_inc_return(&cfs_fail_count);

		if (count >= cfs_fail_val) {
			set_bit(CFS_FAIL_ONCE_BIT, &cfs_fail_loc);
			atomic_set(&cfs_fail_count, 0);
			/* we are lost race to increase  */
			if (count > cfs_fail_val)
				return 0;
		}
	}

	/* Take into account the current call for FAIL_ONCE for ORSET only,
	 * as RESET is a new fail_loc, it does not change the current call
	 */
	if ((set == CFS_FAIL_LOC_ORSET) && (value & CFS_FAIL_ONCE))
		set_bit(CFS_FAIL_ONCE_BIT, &cfs_fail_loc);
	/* Lost race to set CFS_FAILED_BIT. */
	if (test_and_set_bit(CFS_FAILED_BIT, &cfs_fail_loc)) {
		/* If CFS_FAIL_ONCE is valid, only one process can fail,
		 * otherwise multi-process can fail at the same time.
		 */
		if (cfs_fail_loc & CFS_FAIL_ONCE)
			return 0;
	}

	switch (set) {
	case CFS_FAIL_LOC_NOSET:
	case CFS_FAIL_LOC_VALUE:
		break;
	case CFS_FAIL_LOC_ORSET:
		cfs_fail_loc |= value & ~(CFS_FAILED | CFS_FAIL_ONCE);
		break;
	case CFS_FAIL_LOC_RESET:
		cfs_fail_loc = value;
		atomic_set(&cfs_fail_count, 0);
		break;
	default:
		LASSERTF(0, "called with bad set %u\n", set);
		break;
	}

	return 1;
}
EXPORT_SYMBOL(__cfs_fail_check_set);

int __cfs_fail_timeout_set(const char *file, const char *func, const int line,
			   u32 id, u32 value, int ms, int set)
{
	ktime_t till = ktime_add_ms(ktime_get(), ms);
	int ret;

	ret = __cfs_fail_check_set(id, value, set);
	if (ret && likely(ms > 0)) {
		CDEBUG_LIMIT_LOC(file, func, line, D_ERROR,
				 "cfs_fail_timeout id %x sleeping for %dms\n",
				 id, ms);
		while (ktime_before(ktime_get(), till)) {
			schedule_timeout_uninterruptible(cfs_time_seconds(1)
							 / 10);
			set_current_state(TASK_RUNNING);
			if (!cfs_fail_loc) {
				CDEBUG_LIMIT_LOC(file, func, line, D_ERROR,
					      "cfs_fail_timeout interrupted\n");
				break;
			}
		}
		if (cfs_fail_loc)
			CDEBUG_LIMIT_LOC(file, func, line, D_ERROR,
					"cfs_fail_timeout id %x awake\n", id);
	}
	return ret;
}
EXPORT_SYMBOL(__cfs_fail_timeout_set);
