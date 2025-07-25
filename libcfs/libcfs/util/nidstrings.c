// SPDX-License-Identifier: GPL-2.0

/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2017, Intel Corporation.
 */

/*
 * This file is part of Lustre, http://www.lustre.org/
 *
 * Author: Phil Schwan <phil@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LNET

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <ctype.h>

#include <libcfs/util/string.h>
#include <linux/lnet/lnet-types.h>
#include <linux/lnet/nidstr.h>
#ifdef HAVE_NETDB_H
# include <netdb.h>
#endif

/* max value for numeric network address */
#define MAX_NUMERIC_VALUE 0xffffffff

#define IPSTRING_LENGTH 16

/* CAVEAT VENDITOR! Keep the canonical string representation of nets/nids
 * consistent in all conversion functions.  Some code fragments are copied
 * around for the sake of clarity...
 */

/* CAVEAT EMPTOR! Racey temporary buffer allocation!
 * Choose the number of nidstrings to support the MAXIMUM expected number of
 * concurrent users.  If there are more, the returned string will be volatile.
 * NB this number must allow for a process to be descheduled for a timeslice
 * between getting its string and using it.
 */

static char      libcfs_nidstrings[LNET_NIDSTR_COUNT][LNET_NIDSTR_SIZE];
static int       libcfs_nidstring_idx;

char *
libcfs_next_nidstring(void)
{
	char          *str;

	str = libcfs_nidstrings[libcfs_nidstring_idx++];
	if (libcfs_nidstring_idx ==
	    sizeof(libcfs_nidstrings)/sizeof(libcfs_nidstrings[0]))
		libcfs_nidstring_idx = 0;

	return str;
}

static int
libcfs_lo_str2addr(const char *str, int nob, __u32 *addr)
{
	*addr = 0;
	return 1;
}

static void
libcfs_ip_addr2str(__u32 addr, char *str, size_t size)
{
	snprintf(str, size, "%u.%u.%u.%u",
		 (addr >> 24) & 0xff, (addr >> 16) & 0xff,
		 (addr >> 8) & 0xff, addr & 0xff);
}

static void
libcfs_ip_addr2str_size(const __be32 *addr, size_t asize,
			char *str, size_t size)
{
	switch (asize) {
	case 4:
		inet_ntop(AF_INET, addr, str, size);
		break;
	case 16:
		inet_ntop(AF_INET6, addr, str, size);
		break;
	default:
		return;
	}
}

/* CAVEAT EMPTOR XscanfX
 * I use "%n" at the end of a sscanf format to detect trailing junk.  However
 * sscanf may return immediately if it sees the terminating '0' in a string, so
 * I initialise the %n variable to the expected length.  If sscanf sets it;
 * fine, if it doesn't, then the scan ended at the end of the string, which is
 * fine too :)
 */
static int
libcfs_ip_str2addr(const char *str, int nob, __u32 *addr)
{
	unsigned int a, b, c, d;
	int n = nob;
	int rc = 0;

	/* numeric IP? */
	if (sscanf(str, "%u.%u.%u.%u%n", &a, &b, &c, &d, &n) >= 4 &&
	    n == nob &&
	    (a & ~0xff) == 0 && (b & ~0xff) == 0 &&
	    (c & ~0xff) == 0 && (d & ~0xff) == 0) {
		*addr = ((a << 24) | (b << 16) | (c << 8) | d);
		return 1;
	}

	/* known hostname? */
	if (('a' <= str[0] && str[0] <= 'z') ||
	    ('A' <= str[0] && str[0] <= 'Z')) {
		char *tmp = NULL;
		struct addrinfo hints;
		struct addrinfo *ai = NULL;
		struct addrinfo *aip = NULL;

		tmp = (char *)alloca(nob + 1);
		memcpy(tmp, str, nob);
		tmp[nob] = 0;

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;

		if (getaddrinfo(tmp, NULL, &hints, &ai) != 0) {
			rc = 0;
			goto out;
		}

		for (aip = ai; aip; aip = aip->ai_next) {
			if (aip->ai_addr) {
				struct sockaddr_in *sin = (void *)ai->ai_addr;

				__u32 ip = (__u32)sin->sin_addr.s_addr;
				*addr = ntohl(ip);

				rc = 1;
				break;
			}
		}

		freeaddrinfo(ai);
	}
out:
	return rc;
}

static int
libcfs_ip_str2addr_size(const char *str, int nob,
			__be32 *addr, size_t *alen)
{
	char *tmp = malloc(nob+1);

	if (!tmp)
		return 0;
	memcpy(tmp, str, nob);
	tmp[nob] = '\0';

	*alen = 0;
	if (inet_pton(AF_INET, tmp, (struct in_addr *)addr) == 1) {
		struct in_addr *ipv4 = (struct in_addr *)addr;

		/* Don't allow using loopback */
		if (ipv4->s_addr != htonl(INADDR_LOOPBACK))
			*alen = 4;
		goto out;
	}
	if (inet_pton(AF_INET6, tmp, (struct in6_addr *)addr) == 1) {
		struct in6_addr *ipv6 = (struct in6_addr *)addr;

		/* Since link local doesn't allow forwarding packets
		 * for router don't allow those addresses as well.
		 * Site local is allowed since it similar to 10.0.0.0/8.
		 * Be aware site local is deprecated by unique local
		 * addresses.
		 */
		if (!IN6_IS_ADDR_LOOPBACK(ipv6->s6_addr) &&
		    !IN6_IS_ADDR_LINKLOCAL(ipv6->s6_addr))
			*alen = 16;
		goto out;
	}

	/* known hostname? */
	if (('a' <= str[0] && str[0] <= 'z') ||
	    ('A' <= str[0] && str[0] <= 'Z')) {
		struct addrinfo *ai = NULL;
		struct addrinfo hints;

		memset(&hints, 0, sizeof(struct addrinfo));
		hints.ai_family = AF_INET;

		if (getaddrinfo(tmp, NULL, &hints, &ai) == 0) {
			struct addrinfo *a;
			/* First look for an AF_INET address */
			for (a = ai; a; a = a->ai_next) {
				if (a->ai_family == AF_INET && a->ai_addr) {
					struct sockaddr_in *sin =
						(void *)ai->ai_addr;

					memcpy(addr, &sin->sin_addr, 4);
					*alen = 4;
					freeaddrinfo(ai);
					goto out;
				}
			}
			/* Now consider AF_INET6 */
			for (a = ai; a; a = a->ai_next) {
				if (a->ai_family == AF_INET6 && a->ai_addr) {
					struct sockaddr_in6 *sin6 =
						(void *)ai->ai_addr;

					memcpy(addr, &sin6->sin6_addr, 16);
					*alen = 16;
					freeaddrinfo(ai);
					goto out;
				}
			}
		}
		freeaddrinfo(ai);
	}
out:
	free(tmp);
	return *alen != 0;
}

int
cfs_ip_addr_parse(char *str, int len, struct list_head *list)
{
	struct cfs_expr_list *el;
	struct cfs_lstr src;
	int rc;
	int i;

	src.ls_str = str;
	src.ls_len = len;
	i = 0;

	while (src.ls_str != NULL) {
		struct cfs_lstr res;

		if (!cfs_gettok(&src, '.', &res)) {
			rc = -EINVAL;
			goto out;
		}

		rc = cfs_expr_list_parse(res.ls_str, res.ls_len, 0, 255, &el);
		if (rc != 0)
			goto out;

		list_add_tail(&el->el_link, list);
		i++;
	}

	if (i == 4)
		return 0;

	rc = -EINVAL;
out:
	cfs_expr_list_free_list(list);

	return rc;
}

int
cfs_expr2str(struct list_head *list, char *str, size_t size)
{
	struct cfs_expr_list *expr;
	struct cfs_range_expr *range;
	char tmp[LNET_NIDSTR_SIZE];
	size_t len;
	bool first;
	bool bracket = false;
	char *format;
	char *tmpc;

	list_for_each_entry(expr, list, el_link) {
		first = true;
		list_for_each_entry(range, &expr->el_exprs, re_link) {
			if (range->re_lo == range->re_hi) {
				snprintf(tmp,
					 LNET_NIDSTR_SIZE,
					 "%u.", range->re_lo);
			} else if (range->re_lo < range->re_hi) {
				if (range->re_stride > 1) {
					if (first)
						format = "[%u-%u/%u,";
					else
						format = "%u-%u/%u,";
					snprintf(tmp, LNET_NIDSTR_SIZE,
						format, range->re_lo,
						range->re_hi, range->re_stride);
					bracket = true;
				} else {
					if (first)
						format = "[%u-%u,";
					else
						format = "%u-%u,";
					snprintf(tmp, LNET_NIDSTR_SIZE,
						format, range->re_lo,
						range->re_hi);
					bracket = true;
				}
			} else {
				return -EINVAL;
			}
			len = strlen(tmp);
			size -= (len + 1);
			if (size < 0)
				return -ENOBUFS;

			strncat(str, tmp, size + len);
			first = false;
		}
		if (bracket) {
			tmpc = str + (strlen(str) - 1);
			size -= 1;
			if (size < 0)
				return -ENOBUFS;
			*tmpc = ']';
			*(tmpc+1) = '.';
			bracket = false;
		}
	}

	/*
	 * get rid of the trailing '.' at the end of the string
	 * only if we actually had something on the list passed in.
	 * otherwise we could write outside the array
	 */
	if (!list_empty(list))
		str[strlen(str)-1] = '\0';
	return size;
}

static int
libcfs_num_addr_range_expand(struct list_head *addrranges, __u32 *addrs,
			     int max_addrs)
{
	struct cfs_expr_list *expr_list;
	struct cfs_range_expr *range;
	int i;
	int max_idx = max_addrs - 1;
	int addrs_idx = max_idx;

	list_for_each_entry(expr_list, addrranges, el_link) {
		list_for_each_entry(range, &expr_list->el_exprs, re_link) {
			for (i = range->re_lo; i <= range->re_hi;
			     i += range->re_stride) {
				if (addrs_idx < 0)
					return -1;

				addrs[addrs_idx] = i;
				addrs_idx--;
			}
		}
	}

	return max_idx - addrs_idx;
}

static int
libcfs_ip_addr_range_expand(struct list_head *addrranges, __u32 *addrs,
			    int max_addrs)
{
	int rc = 0;

	rc = cfs_ip_addr_range_gen(addrs, max_addrs, addrranges);

	if (rc == -1)
		return rc;
	else
		return max_addrs - rc - 1;
}

static int
libcfs_ip_addr_range_print(char *buffer, int count, struct list_head *list)
{
	int i = 0, j = 0;
	struct cfs_expr_list *el;

	list_for_each_entry(el, list, el_link) {
		assert(j++ < 4);
		if (i != 0)
			i += scnprintf(buffer + i, count - i, ".");
		i += cfs_expr_list_print(buffer + i, count - i, el);
	}
	return i;
}

static int
cfs_ip_addr_range_gen_recurse(__u32 *ip_list, int *count, int shift,
			      __u32 result, struct list_head *head_el,
			      struct cfs_expr_list *octet_el)
{
	__u32 value = 0;
	int i;
	struct cfs_expr_list *next_octet_el;
	struct cfs_range_expr *octet_expr;

	/*
	 * each octet can have multiple expressions so we need to traverse
	 * all of the expressions
	 */
	list_for_each_entry(octet_expr, &octet_el->el_exprs, re_link) {
		for (i = octet_expr->re_lo; i <= octet_expr->re_hi; i++) {
			if (((i - octet_expr->re_lo) % octet_expr->re_stride) == 0) {
				/*
				 * we have a hit calculate the result and
				 * pass it forward to the next iteration
				 * of the recursion.
				 */
				next_octet_el =
					list_entry(octet_el->el_link.next,
							typeof(*next_octet_el),
							el_link);
				value = result | (i << (shift * 8));
				if (next_octet_el->el_link.next != head_el) {
					/*
					 * We still have more octets in
					 * the IP address so traverse
					 * that. We're doing a depth first
					 * recursion here.
					 */
					if (cfs_ip_addr_range_gen_recurse(ip_list, count,
									  shift - 1, value,
									  head_el,
									  next_octet_el) == -1)
						return -1;
				} else {
					/*
					 * We have hit a leaf so store the
					 * calculated IP address in the
					 * list. If we have run out of
					 * space stop the recursion.
					 */
					if (*count == -1)
						return -1;
					/* add ip to the list */
					ip_list[*count] = value;
					(*count)--;
				}
			}
		}
	}
	return 0;
}

/*
 * only generate maximum of count ip addresses from the given expression
 */
int
cfs_ip_addr_range_gen(__u32 *ip_list, int count, struct list_head *ip_addr_expr)
{
	struct cfs_expr_list *octet_el;
	int idx = count - 1;

	octet_el = list_first_entry(ip_addr_expr, typeof(*octet_el), el_link);

	(void) cfs_ip_addr_range_gen_recurse(ip_list, &idx, 3, 0, &octet_el->el_link, octet_el);

	return idx;
}

/**
 * Matches value (\a value) against ranges expression list \a expr_list.
 *
 * \retval 1 if \a value matches
 * \retval 0 otherwise
 */
static int
cfs_expr_list_match(__u32 value, struct cfs_expr_list *expr_list)
{
	struct cfs_range_expr	*expr;

	list_for_each_entry(expr, &expr_list->el_exprs, re_link) {
		if (value >= expr->re_lo && value <= expr->re_hi &&
		    ((value - expr->re_lo) % expr->re_stride) == 0)
			return 1;
	}

	return 0;
}

/**
 * Matches address (\a addr) against address set encoded in \a list.
 *
 * \retval 1 if \a addr matches
 * \retval 0 otherwise
 */
int
cfs_ip_addr_match(__u32 addr, struct list_head *list)
{
	struct cfs_expr_list *el;
	int i = 0;

	list_for_each_entry_reverse(el, list, el_link) {
		if (!cfs_expr_list_match(addr & 0xff, el))
			return 0;
		addr >>= 8;
		i++;
	}

	return i == 4;
}

/**
 * Matches address (\a addr) against the netmask encoded in \a netmask and
 * \a netaddr.
 *
 * \retval 1 if \a addr matches
 * \retval 0 otherwise
 */
int
libcfs_ip_in_netmask(const __be32 *addr, size_t asize, const __be32 *netmask,
		     const __be32 *netaddr)
{
	if (asize == 4) {
		struct in_addr nid_addr, masked_addr;

		memcpy(&nid_addr.s_addr, addr, asize);

		masked_addr.s_addr = nid_addr.s_addr &
				     (*(struct in_addr *)netmask).s_addr;

		return memcmp(&masked_addr, netaddr, sizeof(masked_addr)) == 0;
	} else if (asize == 16) {
		struct in6_addr nid_addr, masked_addr;
		int i;

		memcpy(&nid_addr.s6_addr, addr, asize);

		for (i = 0; i < 16; i++)
			masked_addr.s6_addr[i] =
				nid_addr.s6_addr[i] &
				(*(struct in6_addr *)netmask).s6_addr[i];

		return memcmp(&masked_addr, netaddr, sizeof(masked_addr)) == 0;
	}

	return 0;
}

static void
libcfs_decnum_addr2str(__u32 addr, char *str, size_t size)
{
	snprintf(str, size, "%u", addr);
}

static int
libcfs_num_str2addr(const char *str, int nob, __u32 *addr)
{
	int     n;

	n = nob;
	if (sscanf(str, "0x%x%n", addr, &n) >= 1 && n == nob)
		return 1;

	n = nob;
	if (sscanf(str, "0X%x%n", addr, &n) >= 1 && n == nob)
		return 1;

	n = nob;
	if (sscanf(str, "%u%n", addr, &n) >= 1 && n == nob)
		return 1;

	return 0;
}

/**
 * Nf_parse_addrlist method for networks using numeric addresses.
 *
 * Examples of such networks are gm and elan.
 *
 * \retval 0 if \a str parsed to numeric address
 * \retval errno otherwise
 */
int
libcfs_num_parse(char *str, int len, struct list_head *list)
{
	struct cfs_expr_list *el;
	int	rc;

	rc = cfs_expr_list_parse(str, len, 0, MAX_NUMERIC_VALUE, &el);
	if (rc == 0)
		list_add_tail(&el->el_link, list);

	return rc;
}

static int
libcfs_num_addr_range_print(char *buffer, int count, struct list_head *list)
{
	struct cfs_expr_list *el;
	int i = 0, j = 0;

	list_for_each_entry(el, list, el_link) {
		assert(j++ < 1);
		i += cfs_expr_list_print(buffer + i, count - i, el);
	}
	return i;
}

/*
 * Nf_match_addr method for networks using numeric addresses
 *
 * \retval 1 on match
 * \retval 0 otherwise
 */
static int
libcfs_num_match(__u32 addr, struct list_head *numaddr)
{
	struct cfs_expr_list *el;

	assert(!list_empty(numaddr));
	el = list_first_entry(numaddr, struct cfs_expr_list, el_link);

	return cfs_expr_list_match(addr, el);
}

static int cfs_ip_min_max(struct list_head *nidlist, __u32 *min, __u32 *max);
static int cfs_num_min_max(struct list_head *nidlist, __u32 *min, __u32 *max);

static struct netstrfns libcfs_netstrfns[] = {
	{
		.nf_type		= LOLND,
		.nf_name		= "lo",
		.nf_modname		= "klolnd",
		.nf_addr2str		= libcfs_decnum_addr2str,
		.nf_str2addr		= libcfs_lo_str2addr,
		.nf_parse_addrlist	= libcfs_num_parse,
		.nf_print_addrlist	= libcfs_num_addr_range_print,
		.nf_match_addr		= libcfs_num_match,
		.nf_min_max		= cfs_num_min_max,
		.nf_expand_addrrange	= libcfs_num_addr_range_expand
	},
	{
		.nf_type		= SOCKLND,
		.nf_name		= "tcp",
		.nf_modname		= "ksocklnd",
		.nf_addr2str		= libcfs_ip_addr2str,
		.nf_addr2str_size	= libcfs_ip_addr2str_size,
		.nf_str2addr		= libcfs_ip_str2addr,
		.nf_str2addr_size	= libcfs_ip_str2addr_size,
		.nf_parse_addrlist	= cfs_ip_addr_parse,
		.nf_print_addrlist	= libcfs_ip_addr_range_print,
		.nf_match_addr		= cfs_ip_addr_match,
		.nf_min_max		= cfs_ip_min_max,
		.nf_expand_addrrange	= libcfs_ip_addr_range_expand,
		.nf_match_netmask	= libcfs_ip_in_netmask
	},
	{
		.nf_type		= O2IBLND,
		.nf_name		= "o2ib",
		.nf_modname		= "ko2iblnd",
		.nf_addr2str		= libcfs_ip_addr2str,
		.nf_str2addr		= libcfs_ip_str2addr,
		.nf_parse_addrlist	= cfs_ip_addr_parse,
		.nf_print_addrlist	= libcfs_ip_addr_range_print,
		.nf_match_addr		= cfs_ip_addr_match,
		.nf_min_max		= cfs_ip_min_max,
		.nf_expand_addrrange	= libcfs_ip_addr_range_expand,
		.nf_match_netmask	= libcfs_ip_in_netmask
	},
	{
		.nf_type		= GNILND,
		.nf_name		= "gni",
		.nf_modname		= "kgnilnd",
		.nf_addr2str		= libcfs_decnum_addr2str,
		.nf_str2addr		= libcfs_num_str2addr,
		.nf_parse_addrlist	= libcfs_num_parse,
		.nf_print_addrlist	= libcfs_num_addr_range_print,
		.nf_match_addr		= libcfs_num_match,
		.nf_min_max		= cfs_num_min_max,
		.nf_expand_addrrange	= libcfs_num_addr_range_expand
	},
	{
		.nf_type		= GNIIPLND,
		.nf_name		= "gip",
		.nf_modname		= "kgnilnd",
		.nf_addr2str		= libcfs_ip_addr2str,
		.nf_str2addr		= libcfs_ip_str2addr,
		.nf_parse_addrlist	= cfs_ip_addr_parse,
		.nf_print_addrlist	= libcfs_ip_addr_range_print,
		.nf_match_addr		= cfs_ip_addr_match,
		.nf_min_max		= cfs_ip_min_max,
		.nf_expand_addrrange	= libcfs_ip_addr_range_expand
	},
	{
		.nf_type		= PTL4LND,
		.nf_name		= "ptlf",
		.nf_modname		= "kptl4lnd",
		.nf_addr2str		= libcfs_decnum_addr2str,
		.nf_str2addr		= libcfs_num_str2addr,
		.nf_parse_addrlist	= libcfs_num_parse,
		.nf_print_addrlist	= libcfs_num_addr_range_print,
		.nf_match_addr		= libcfs_num_match,
		.nf_min_max		= cfs_num_min_max,
		.nf_expand_addrrange	= libcfs_num_addr_range_expand
	},
	{
		.nf_type		= KFILND,
		.nf_name		= "kfi",
		.nf_modname		= "kkfilnd",
		.nf_addr2str		= libcfs_decnum_addr2str,
		.nf_str2addr		= libcfs_num_str2addr,
		.nf_parse_addrlist	= libcfs_num_parse,
		.nf_print_addrlist	= libcfs_num_addr_range_print,
		.nf_match_addr		= libcfs_num_match,
		.nf_min_max		= cfs_num_min_max,
		.nf_expand_addrrange	= libcfs_num_addr_range_expand
	},
	{
		.nf_type		= EFALND,
		.nf_name		= "efa",
		.nf_modname		= "kefalnd",
		.nf_addr2str		= libcfs_ip_addr2str,
		.nf_addr2str_size	= libcfs_ip_addr2str_size,
		.nf_str2addr		= libcfs_ip_str2addr,
		.nf_str2addr_size	= libcfs_ip_str2addr_size,
		.nf_parse_addrlist	= cfs_ip_addr_parse,
		.nf_print_addrlist	= libcfs_ip_addr_range_print,
		.nf_match_addr		= cfs_ip_addr_match,
		.nf_min_max		= cfs_ip_min_max,
		.nf_expand_addrrange	= libcfs_ip_addr_range_expand
	},
	{
		.nf_type		= BXI3LND,
		.nf_name		= "bxi3f",
		.nf_modname		= "kbxi3lnd",
		.nf_addr2str		= libcfs_decnum_addr2str,
		.nf_str2addr		= libcfs_num_str2addr,
		.nf_parse_addrlist	= libcfs_num_parse,
		.nf_print_addrlist	= libcfs_num_addr_range_print,
		.nf_match_addr		= libcfs_num_match,
		.nf_min_max		= cfs_num_min_max,
		.nf_expand_addrrange	= libcfs_num_addr_range_expand
	},
};

static const size_t libcfs_nnetstrfns =
	sizeof(libcfs_netstrfns)/sizeof(libcfs_netstrfns[0]);

static struct netstrfns *
libcfs_lnd2netstrfns(__u32 lnd)
{
	int	i;

	for (i = 0; i < libcfs_nnetstrfns; i++)
		if (lnd == libcfs_netstrfns[i].nf_type)
			return &libcfs_netstrfns[i];

	return NULL;
}

static struct netstrfns *
libcfs_namenum2netstrfns(const char *name)
{
	struct netstrfns *nf;
	int               i;

	for (i = 0; i < libcfs_nnetstrfns; i++) {
		nf = &libcfs_netstrfns[i];
		if (!strncmp(name, nf->nf_name, strlen(nf->nf_name)))
			return nf;
	}
	return NULL;
}

static struct netstrfns *
libcfs_name2netstrfns(const char *name)
{
	int    i;

	for (i = 0; i < libcfs_nnetstrfns; i++)
		if (!strcmp(libcfs_netstrfns[i].nf_name, name))
			return &libcfs_netstrfns[i];

	return NULL;
}

int
libcfs_isknown_lnd(__u32 lnd)
{
	return libcfs_lnd2netstrfns(lnd) != NULL;
}

char *
libcfs_lnd2modname(__u32 lnd)
{
	struct netstrfns *nf = libcfs_lnd2netstrfns(lnd);

	return (nf == NULL) ? NULL : nf->nf_modname;
}

int
libcfs_str2lnd(const char *str)
{
	struct netstrfns *nf = libcfs_name2netstrfns(str);

	if (nf != NULL)
		return nf->nf_type;

	return -1;
}

char *
libcfs_lnd2str_r(__u32 lnd, char *buf, size_t buf_size)
{
	struct netstrfns *nf;

	nf = libcfs_lnd2netstrfns(lnd);
	if (nf == NULL)
		snprintf(buf, buf_size, "?%u?", lnd);
	else
		snprintf(buf, buf_size, "%s", nf->nf_name);

	return buf;
}

char *
libcfs_net2str_r(__u32 net, char *buf, size_t buf_size)
{
	__u32		  nnum = LNET_NETNUM(net);
	__u32		  lnd  = LNET_NETTYP(net);
	struct netstrfns *nf;

	nf = libcfs_lnd2netstrfns(lnd);
	if (nf == NULL)
		snprintf(buf, buf_size, "<%u:%u>", lnd, nnum);
	else if (nnum == 0)
		snprintf(buf, buf_size, "%s", nf->nf_name);
	else
		snprintf(buf, buf_size, "%s%u", nf->nf_name, nnum);

	return buf;
}

char *
libcfs_nid2str_r(lnet_nid_t nid, char *buf, size_t buf_size)
{
	__u32		  addr = LNET_NIDADDR(nid);
	__u32		  net  = LNET_NIDNET(nid);
	__u32		  nnum = LNET_NETNUM(net);
	__u32		  lnd  = LNET_NETTYP(net);
	struct netstrfns *nf;

	if (nid == LNET_NID_ANY) {
		strncpy(buf, "<?>", buf_size);
		buf[buf_size - 1] = '\0';
		return buf;
	}

	nf = libcfs_lnd2netstrfns(lnd);
	if (nf == NULL) {
		snprintf(buf, buf_size, "%x@<%u:%u>", addr, lnd, nnum);
	} else {
		size_t addr_len;

		nf->nf_addr2str(addr, buf, buf_size);
		addr_len = strlen(buf);
		if (nnum == 0)
			snprintf(buf + addr_len, buf_size - addr_len, "@%s",
				 nf->nf_name);
		else
			snprintf(buf + addr_len, buf_size - addr_len, "@%s%u",
				 nf->nf_name, nnum);
	}

	return buf;
}

char *
libcfs_nidstr_r(const struct lnet_nid *nid, char *buf, size_t buf_size)
{
	__u32 nnum;
	__u32 lnd;
	struct netstrfns *nf;

	if (LNET_NID_IS_ANY(nid)) {
		strncpy(buf, "<?>", buf_size);
		buf[buf_size - 1] = '\0';
		return buf;
	}

	nnum = __be16_to_cpu(nid->nid_num);
	lnd = nid->nid_type;
	nf = libcfs_lnd2netstrfns(lnd);
	if (nf) {
		size_t addr_len;
		/* Avoid take address in packed array */
		__u32 addr[4] = { nid->nid_addr[0], nid->nid_addr[1],
				 nid->nid_addr[2], nid->nid_addr[3]};

		if (nf->nf_addr2str_size)
			nf->nf_addr2str_size(addr, NID_ADDR_BYTES(nid),
					     buf, buf_size);
		else
			nf->nf_addr2str(ntohl(nid->nid_addr[0]), buf, buf_size);
		addr_len = strlen(buf);
		if (nnum == 0)
			snprintf(buf + addr_len, buf_size - addr_len, "@%s",
				 nf->nf_name);
		else
			snprintf(buf + addr_len, buf_size - addr_len, "@%s%u",
				 nf->nf_name, nnum);
	} else {
		int l = 0;
		int words = (NID_ADDR_BYTES(nid) + 3) / 4;
		int i;

		for (i = 0; i < words && i < 4; i++)
			l = snprintf(buf+l, buf_size-l, "%s%x",
				     i ? ":" : "", ntohl(nid->nid_addr[i]));
		snprintf(buf+l, buf_size-l, "@<%u:%u>", lnd, nnum);
	}

	return buf;
}

static struct netstrfns *
libcfs_str2net_internal(const char *str, __u32 *net)
{
	struct netstrfns *nf = NULL;
	int		  nob;
	unsigned int	  netnum;
	int		  i;

	for (i = 0; i < libcfs_nnetstrfns; i++) {
		nf = &libcfs_netstrfns[i];
		if (!strncmp(str, nf->nf_name, strlen(nf->nf_name)))
			break;
	}

	if (i == libcfs_nnetstrfns)
		return NULL;

	nob = strlen(nf->nf_name);

	if (strlen(str) == (unsigned int)nob) {
		netnum = 0;
	} else {
		if (nf->nf_type == LOLND) /* net number not allowed */
			return NULL;

		str += nob;
		i = strlen(str);
		if (sscanf(str, "%u%n", &netnum, &i) < 1 ||
		    i != (int)strlen(str))
			return NULL;
	}

	*net = LNET_MKNET(nf->nf_type, netnum);
	return nf;
}

__u32
libcfs_str2net(const char *str)
{
	__u32  net;

	if (libcfs_str2net_internal(str, &net) != NULL)
		return net;

	return LNET_NET_ANY;
}

lnet_nid_t
libcfs_str2nid(const char *str)
{
	const char       *sep = strchr(str, '@');
	struct netstrfns *nf;
	__u32             net;
	__u32             addr;

	if (sep != NULL) {
		nf = libcfs_str2net_internal(sep + 1, &net);
		if (nf == NULL)
			return LNET_NID_ANY;
	} else {
		sep = str + strlen(str);
		net = LNET_MKNET(SOCKLND, 0);
		nf = libcfs_lnd2netstrfns(SOCKLND);
		assert(nf != NULL);
	}

	if (!nf->nf_str2addr(str, (int)(sep - str), &addr))
		return LNET_NID_ANY;

	return LNET_MKNID(net, addr);
}

int
libcfs_strnid(struct lnet_nid *nid, const char *str)
{
	const char *sep = strchr(str, '@');
	struct netstrfns *nf;
	__u32 net;

	if (sep != NULL) {
		nf = libcfs_str2net_internal(sep + 1, &net);
		if (nf == NULL)
			return -EINVAL;
	} else {
		sep = str + strlen(str);
		net = LNET_MKNET(SOCKLND, 0);
		nf = libcfs_lnd2netstrfns(SOCKLND);
		assert(nf != NULL);
	}

	memset(nid, 0, sizeof(*nid));
	nid->nid_type = LNET_NETTYP(net);
	nid->nid_num = htons(LNET_NETNUM(net));
	if (nf->nf_str2addr_size) {
		size_t asize = 0;
		__u32 addr[4];

		if (!nf->nf_str2addr_size(str, (int)(sep - str),
					  addr, &asize))
			return -EINVAL;

		/* Avoid take address in packed array */
		nid->nid_addr[0] = addr[0];
		nid->nid_addr[1] = addr[1];
		nid->nid_addr[2] = addr[2];
		nid->nid_addr[3] = addr[3];
		nid->nid_size = asize - 4;
	} else {
		__u32 addr;

		if (!nf->nf_str2addr(str, (int)(sep - str), &addr))
			return -EINVAL;
		nid->nid_addr[0] = htonl(addr);
		nid->nid_size = 0;
	}
	return 0;
}

char *
libcfs_id2str(struct lnet_process_id id)
{
	char *str = libcfs_next_nidstring();

	if (id.pid == LNET_PID_ANY) {
		snprintf(str, LNET_NIDSTR_SIZE,
			 "LNET_PID_ANY-%s", libcfs_nid2str(id.nid));
		return str;
	}

	snprintf(str, LNET_NIDSTR_SIZE, "%s%u-%s",
		 ((id.pid & LNET_PID_USERFLAG) != 0) ? "U" : "",
		 (id.pid & ~LNET_PID_USERFLAG), libcfs_nid2str(id.nid));
	return str;
}

int
libcfs_str2anynid(lnet_nid_t *nidp, const char *str)
{
	if (!strcmp(str, "*")) {
		*nidp = LNET_NID_ANY;
		return 1;
	}

	*nidp = libcfs_str2nid(str);
	return *nidp != LNET_NID_ANY;
}

int
libcfs_stranynid(struct lnet_nid *nid, const char *str)
{
	if (!strcmp(str, "*")) {
		*nid = LNET_ANY_NID;
		return 1;
	}

	if (libcfs_strnid(nid, str) < 0)
		*nid = LNET_ANY_NID;

	return !LNET_NID_IS_ANY(nid);
}

/* NID range list syntax.
 * \verbatim
 *
 * <nidlist>	     :== <nidrange> [ ' ' <nidrange> ]
 * <nidrange>	     :== <addrrange> '@' <net>
 * <addrrange>	     :== '*' |
 *			 <netmask> |
 *			 <ipv6_addr> |
 *			 <ipv4_addr_range> |
 *			 <numaddr_range>
 * <netmask>	     :== An IPv4 or IPv6 network mask in CIDR notation.
 *			 e.g. 192.168.1.0/24 or 2001:0db8::/32
 * <ipv6_addr>	     :== A single IPv6 address
 * <ipv4_addr_range> :==
 *	<numaddr_range>.<numaddr_range>.<numaddr_range>.<numaddr_range>
 * <numaddr_range>   :== <number> |
 *			 <expr_list>
 * <expr_list>	     :== '[' <range_expr> [ ',' <range_expr>] ']'
 * <range_expr>	     :== <number> |
 *			 <number> '-' <number> |
 *			 <number> '-' <number> '/' <number>
 * <net>	     :== <netname> | <netname><number>
 * <netname>	     :== "lo" | "tcp" | "o2ib" | "gni" | "gip" | "ptlf" | "kfi"
 * \endverbatim
 */

/**
 * Structure to represent \<nidrange\> token of the syntax.
 *
 * One of this is created for each \<net\> parsed.
 */
struct nidrange {
	/**
	 * Link to list of this structures which is built on nid range
	 * list parsing.
	 */
	struct list_head nr_link;
	/**
	 * List head for addrrange::ar_link.
	 */
	struct list_head nr_addrranges;
	/**
	 * List head for nidmask::nm_link.
	 */
	struct list_head nr_nidmasks;
	/**
	 * Flag indicating that *@<net> is found.
	 */
	int nr_all;
	/**
	 * Pointer to corresponding element of libcfs_netstrfns.
	 */
	struct netstrfns *nr_netstrfns;
	/**
	 * Number of network. E.g. 5 if \<net\> is "elan5".
	 */
	int nr_netnum;
};

/**
 * Structure to represent \<netmask\> token of the syntax
 */
struct nidmask {
	/* Link to nidrange::nr_nidmasks */
	struct list_head nm_link;

	/* This is the base address that was parsed */
	union {
		struct in_addr ipv4;
		struct in6_addr ipv6;
	} nm_addr;

	/* Netmask derived from the prefix length */
	union {
		struct in_addr ipv4;
		struct in6_addr ipv6;
	} nm_netmask;

	/* Network address derived from the base address and the netmask */
	union {
		struct in_addr ipv4;
		struct in6_addr ipv6;
	} nm_netaddr;

	/* Address family */
	sa_family_t nm_family;

	/* Prefix length */
	__u8 nm_prefix_len;
};

/**
 * Structure to represent \<addrrange\> token of the syntax.
 */
struct addrrange {
	/**
	 * Link to nidrange::nr_addrranges.
	 */
	struct list_head ar_link;
	/**
	 * List head for cfs_expr_list::el_list.
	 */
	struct list_head ar_numaddr_ranges;
};

/**
 * Parses \<addrrange\> token on the syntax.
 *
 * Allocates struct addrrange and links to \a nidrange via
 * (nidrange::nr_addrranges)
 *
 * \retval 0 if \a src parses to '*' | \<ipaddr_range\> | \<cfs_expr_list\>
 * \retval -errno otherwise
 */
static int
parse_addrange(char *str, struct nidrange *nidrange)
{
	struct addrrange *addrrange;

	if (strcmp(str, "*") == 0) {
		nidrange->nr_all = 1;
		return 0;
	}

	addrrange = calloc(1, sizeof(struct addrrange));
	if (!addrrange)
		return -ENOMEM;
	list_add_tail(&addrrange->ar_link, &nidrange->nr_addrranges);
	INIT_LIST_HEAD(&addrrange->ar_numaddr_ranges);

	return nidrange->nr_netstrfns->nf_parse_addrlist(str, strlen(str),
						&addrrange->ar_numaddr_ranges);
}

static void
init_ipv4_nidmask(struct in_addr *ipv4, struct nidmask *nm)
{
	memcpy(&nm->nm_addr.ipv4, &ipv4->s_addr, sizeof(struct in_addr));

	nm->nm_netmask.ipv4.s_addr =
		htonl(~((1U << (32 - nm->nm_prefix_len)) - 1));
	nm->nm_netaddr.ipv4.s_addr = ipv4->s_addr & nm->nm_netmask.ipv4.s_addr;
}

/* Note: The memory of struct nidmask is allocated via calloc, so it has been
 * set to zero as required by this function.
 */
static void
init_ipv6_nidmask(struct in6_addr *ipv6, struct nidmask *nm)
{
	int i, j;

	memcpy(&nm->nm_addr.ipv6, &ipv6->s6_addr, sizeof(struct in6_addr));

	for (i = nm->nm_prefix_len, j = 0; i > 0; i -= 8, j++) {
		if (i >= 8)
			nm->nm_netmask.ipv6.s6_addr[j] = 0xff;
		else
			nm->nm_netmask.ipv6.s6_addr[j] =
					(unsigned long)(0xffU << (8 - i));
	}

	for (i = 0; i < sizeof(struct in6_addr); i++)
		nm->nm_netaddr.ipv6.s6_addr[i] = ipv6->s6_addr[i] &
						 nm->nm_netmask.ipv6.s6_addr[i];
}

static __u8
parse_prefix_len(char *str)
{
	unsigned int max;
	unsigned int prefix_len;
	char *slash = strchr(str, '/');

	/* IPv4 netmask must include an explicit prefix length */
	if (!(slash || strchr(str, ':')))
		return 0;

	/* We treat an IPv6 address without a prefix length as having /128 */
	if (!slash)
		return 128;

	if (strchr(str, ':'))
		max = 128;
	else
		max = 32;

	slash++;
	if (!cfs_str2num_check(slash, strlen(slash), &prefix_len, 1, max))
		return 0;

	return (__u8)prefix_len;
}

static int
parse_nidmask(char *str, struct nidrange *nr)
{
	struct nidmask *nm;
	char *addrstr;
	struct netstrfns *nf = nr->nr_netstrfns;
	size_t asize;
	__u32 addr[4];

	nm = calloc(1, sizeof(struct nidmask));
	if (!nm) {
		fprintf(stderr, "Failed to allocate memory for nidmask\n");
		return -ENOMEM;
	}

	/* Add to nr_nidmasks so that our caller can free us on error */
	list_add_tail(&nm->nm_link, &nr->nr_nidmasks);

	nm->nm_prefix_len = parse_prefix_len(str);
	if (!nm->nm_prefix_len) {
		fprintf(stderr, "Failed to parse prefix length from \"%s\"\n",
			str);
		return -EINVAL;
	}

	if (!nf->nf_str2addr_size) {
		fprintf(stderr, "Network type doesn't support nidmasks\n");
		return -EINVAL;
	}

	addrstr = strsep(&str, "/");
	if (!nf->nf_str2addr_size(addrstr, strlen(addrstr), addr, &asize)) {
		fprintf(stderr, "Failed to convert \"%s\" to address\n",
			addrstr);
		return -EINVAL;
	}

	if (asize == 4) {
		nm->nm_family = AF_INET;
		init_ipv4_nidmask((struct in_addr *)&addr, nm);
	} else if (asize == 16) {
		nm->nm_family = AF_INET6;
		init_ipv6_nidmask((struct in6_addr *)&addr, nm);
	} else {
		return -EINVAL;
	}

	return 0;
}

/**
 * Finds or creates struct nidrange.
 *
 * Checks if \a src is a valid network name, looks for corresponding
 * nidrange on the ist of nidranges (\a nidlist), creates new struct
 * nidrange if it is not found.
 *
 * \retval pointer to struct nidrange matching network specified via \a src
 * \retval NULL if \a src does not match any network
 */
static struct nidrange *
add_nidrange(char *str, struct list_head *nidlist)
{
	struct netstrfns *nf;
	struct nidrange *nr;
	char *end;
	unsigned int netnum;

	nf = libcfs_namenum2netstrfns(str);
	if (!nf)
		return NULL;

	end = str + strlen(nf->nf_name);
	if (!*end) {
		/* network name only, e.g. "elan" or "tcp" */
		netnum = 0;
	} else {
		/* e.g. "elan25" or "tcp23", refuse to parse if
		 * network name is not appended with decimal or
		 * hexadecimal number */
		if (!cfs_str2num_check(end, strlen(end), &netnum, 0,
				       MAX_NUMERIC_VALUE))
			return NULL;
	}

	list_for_each_entry(nr, nidlist, nr_link) {
		if (nr->nr_netstrfns != nf)
			continue;
		if (nr->nr_netnum != netnum)
			continue;
		return nr;
	}

	nr = calloc(1, sizeof(struct nidrange));
	if (!nr)
		return NULL;
	list_add_tail(&nr->nr_link, nidlist);
	INIT_LIST_HEAD(&nr->nr_addrranges);
	INIT_LIST_HEAD(&nr->nr_nidmasks);
	nr->nr_netstrfns = nf;
	nr->nr_all = 0;
	nr->nr_netnum = netnum;

	return nr;
}

/**
 * Parses \<nidrange\> token of the syntax.
 *
 * \retval 0 if \a src parses to \<addrrange\> '@' \<net\>
 * \retval -errno otherwise
 */
static int
parse_nidrange(char *str, struct list_head *nidlist)
{
	char *addrrange;
	char *net;
	char *slash;
	struct nidrange *nr;
	int rc;

	addrrange = strsep(&str, "@");
	if (!str) {
		fprintf(stderr, "nidrange \"%s\" doesn't contain '@'\n",
			addrrange);
		return -EINVAL;
	}

	net = strim(str);
	if (strchr(net, '@') || !*net) {
		fprintf(stderr, "net \"%s\" is malformed\n", net);
		return -EINVAL;
	}

	nr = add_nidrange(net, nidlist);
	if (!nr) {
		fprintf(stderr, "failed to add nidrange for network \"%s\"\n",
			net);
		return -EINVAL;
	}

	/* Check for an IPv6 address or a '/' outside of '[]' */
	slash = strchr(addrrange, '/');
	if (strchr(addrrange, ':') || (slash && !strchr(slash, ']')))
		rc = parse_nidmask(addrrange, nr);
	else
		rc = parse_addrange(addrrange, nr);

	if (rc)
		fprintf(stderr, "Failed to parse addrrange \"%s\" rc = %d\n",
			addrrange, rc);
	return rc;
}

static __u32
libcfs_net_str_len(const char *str)
{
	int i;
	struct netstrfns *nf = NULL;

	for (i = 0; i < libcfs_nnetstrfns; i++) {
		nf = &libcfs_netstrfns[i];
		if (!strncmp(str, nf->nf_name, strlen(nf->nf_name)))
			return strlen(nf->nf_name);
	}

	return 0;
}

static int
parse_net_range(char *str, __u32 len, struct list_head *net_num,
		__u32 *net_type)
{
	struct cfs_lstr next;
	__u32 net_type_len;
	__u32 net;
	char *bracket;
	char *star;

	if (!str)
		return -EINVAL;

	next.ls_str = str;
	next.ls_len = len;

	net_type_len = libcfs_net_str_len(str);

	if (net_type_len < len) {
		char c = str[net_type_len];

		str[net_type_len] = '\0';
		net = libcfs_str2net(str);
		str[net_type_len] = c;
	} else {
		net = libcfs_str2net(str);
	}

	if (net == LNET_NIDNET(LNET_NID_ANY))
		return -EINVAL;

	*net_type = LNET_NETTYP(net);

	/*
	 * the net is either followed with an absolute number, *, or an
	 * expression enclosed in []
	 */
	bracket = strchr(next.ls_str, '[');
	star = strchr(next.ls_str, '*');

	/* "*[" pattern not allowed */
	if (bracket && star && star < bracket)
		return -EINVAL;

	if (!bracket) {
		next.ls_str = str + net_type_len;
		next.ls_len = strlen(next.ls_str);
	} else {
		next.ls_str = bracket;
		next.ls_len = strlen(bracket);
	}

	/* if there is no net number just return */
	if (next.ls_len == 0)
		return 0;

	return libcfs_num_parse(next.ls_str, next.ls_len,
				net_num);
}

static int
parse_address(struct cfs_lstr *src, const __u32 net_type,
	      struct list_head *addr)
{
	int i;
	struct netstrfns *nf = NULL;

	for (i = 0; i < libcfs_nnetstrfns; i++) {
		nf = &libcfs_netstrfns[i];
		if (net_type == nf->nf_type)
			return nf->nf_parse_addrlist(src->ls_str, src->ls_len,
						     addr);
	}

	return -EINVAL;
}

int
cfs_parse_nid_parts(char *str, struct list_head *addr,
		    struct list_head *net_num, __u32 *net_type)
{
	struct cfs_lstr next;
	struct cfs_lstr addrrange;
	bool found = false;
	int rc;

	if (!str)
		return -EINVAL;

	next.ls_str = str;
	next.ls_len = strlen(str);

	rc = cfs_gettok(&next, '@', &addrrange);
	if (!rc)
		return -EINVAL;

	if (!next.ls_str) {
		/* only net is present */
		next.ls_str = str;
		next.ls_len = strlen(str);
	} else {
		found = true;
	}

	/* assume only net is present */
	rc = parse_net_range(next.ls_str, next.ls_len, net_num, net_type);

	/*
	 * if we successfully parsed the net range and there is no
	 * address, or if we fail to parse the net range then return
	 */
	if ((!rc && !found) || rc)
		return rc;

	return parse_address(&addrrange, *net_type, addr);
}

/**
 * Frees addrrange structures of \a list.
 *
 * For each struct addrrange structure found on \a list it frees
 * cfs_expr_list list attached to it and frees the addrrange itself.
 *
 * \retval none
 */
static void
free_addrranges(struct list_head *list)
{
	while (!list_empty(list)) {
		struct addrrange *ar;

		ar = list_first_entry(list, struct addrrange, ar_link);

		cfs_expr_list_free_list(&ar->ar_numaddr_ranges);
		list_del(&ar->ar_link);
		free(ar);
	}
}

static void
free_nidmasks(struct list_head *list)
{
	struct nidmask *nm;

	while (!list_empty(list)) {
		nm = list_first_entry(list, struct nidmask, nm_link);
		list_del(&nm->nm_link);
		free(nm);
	}
}

/**
 * Frees nidrange strutures of \a list.
 *
 * For each struct nidrange structure found on \a list it frees
 * addrrange list attached to it and frees the nidrange itself.
 *
 * \retval none
 */
void
cfs_free_nidlist(struct list_head *list)
{
	struct list_head *pos, *next;
	struct nidrange *nr;

	list_for_each_safe(pos, next, list) {
		nr = list_entry(pos, struct nidrange, nr_link);
		free_addrranges(&nr->nr_addrranges);
		free_nidmasks(&nr->nr_nidmasks);
		list_del(pos);
		free(nr);
	}
}

/**
 * Parses nid range list.
 *
 * Parses with rigorous syntax and overflow checking \a str into
 * \<nidrange\> [ ' ' \<nidrange\> ], compiles \a str into set of
 * structures and links that structure to \a nidlist. The resulting
 * list can be used to match a NID againts set of NIDS defined by \a
 * str.
 * \see cfs_match_nid
 *
 * \retval 1 on success
 * \retval 0 otherwise
 */
int
cfs_parse_nidlist(char *orig, int len, struct list_head *nidlist)
{
	int rc = 0;
	char *str;

	orig = strndup(orig, len);
	if (!orig)
		return 0;

	INIT_LIST_HEAD(nidlist);
	str = orig;
	while (rc == 0 && str) {
		char *tok = strsep(&str, " ");

		if (*tok)
			rc = parse_nidrange(tok, nidlist);
	}
	free(orig);
	if (rc)
		cfs_free_nidlist(nidlist);
	else if (list_empty(nidlist))
		rc = -EINVAL;

	return rc ? 0 : 1;
}

static int
match_nidmask(const struct lnet_nid *nid, struct nidmask *nm,
	      struct netstrfns *nf)
{
	__be32 addr[4] = { nid->nid_addr[0], nid->nid_addr[1],
			   nid->nid_addr[2], nid->nid_addr[3] };
	__be32 *netmask, *netaddr;

	if (!nf->nf_match_netmask)
		return 0;

	if (nid_is_nid4(nid) && nm->nm_family == AF_INET) {
		netmask = (__be32 *)&nm->nm_netmask.ipv4.s_addr;
		netaddr = (__be32 *)&nm->nm_netaddr.ipv4.s_addr;
	} else if (!nid_is_nid4(nid) && nm->nm_family == AF_INET6) {
		netmask = (__be32 *)&nm->nm_netmask.ipv6.s6_addr;
		netaddr = (__be32 *)&nm->nm_netaddr.ipv6.s6_addr;
	} else {
		return 0;
	}

	return nf->nf_match_netmask(addr, NID_ADDR_BYTES(nid), netmask,
				    netaddr);
}

/**
 * Matches a nid (\a nid) against the compiled list of nidranges (\a nidlist).
 *
 * \see cfs_parse_nidlist()
 *
 * \retval 1 on match
 * \retval 0  otherwises
 */
int cfs_match_nid(const struct lnet_nid *nid, struct list_head *nidlist)
{
	struct nidrange *nr;
	struct nidmask *nm;
	struct addrrange *ar;

	list_for_each_entry(nr, nidlist, nr_link) {
		if (nr->nr_netstrfns->nf_type != nid->nid_type)
			continue;
		if (nr->nr_netnum != __be16_to_cpu(nid->nid_num))
			continue;
		if (nr->nr_all)
			return 1;

		list_for_each_entry(nm, &nr->nr_nidmasks, nm_link)
			if (match_nidmask(nid, nm, nr->nr_netstrfns))
				return 1;

		list_for_each_entry(ar, &nr->nr_addrranges, ar_link)
			if (nr->nr_netstrfns->nf_match_addr(
				    __be32_to_cpu(nid->nid_addr[0]),
				    &ar->ar_numaddr_ranges))
				return 1;
	}
	return 0;
}

int
cfs_match_net(__u32 net_id, __u32 net_type, struct list_head *net_num_list)
{
	__u32 net_num;

	if (!net_num_list)
		return 0;

	if (net_type != LNET_NETTYP(net_id))
		return 0;

	net_num = LNET_NETNUM(net_id);

	/*
	 * if there is a net number but the list passed in is empty, then
	 * there is no match.
	 */
	if (!net_num && list_empty(net_num_list))
		return 1;
	else if (list_empty(net_num_list))
		return 0;

	if (!libcfs_num_match(net_num, net_num_list))
		return 0;

	return 1;
}

/**
 * Print the network part of the nidrange \a nr into the specified \a buffer.
 *
 * \retval number of characters written
 */
static int
cfs_print_network(char *buffer, int count, struct nidrange *nr)
{
	struct netstrfns *nf = nr->nr_netstrfns;

	if (nr->nr_netnum == 0)
		return scnprintf(buffer, count, "@%s", nf->nf_name);
	else
		return scnprintf(buffer, count, "@%s%u",
				 nf->nf_name, nr->nr_netnum);
}


/**
 * Print a list of addrrange (\a addrranges) into the specified \a buffer.
 * At max \a count characters can be printed into \a buffer.
 *
 * \retval number of characters written
 */
static int
cfs_print_addrranges(char *buffer, int count, struct list_head *addrranges,
		     struct nidrange *nr)
{
	int i = 0;
	struct addrrange *ar;
	struct netstrfns *nf = nr->nr_netstrfns;

	list_for_each_entry(ar, addrranges, ar_link) {
		if (i != 0)
			i += scnprintf(buffer + i, count - i, " ");
		i += nf->nf_print_addrlist(buffer + i, count - i,
					   &ar->ar_numaddr_ranges);
		i += cfs_print_network(buffer + i, count - i, nr);
	}
	return i;
}

static int
cfs_print_nidmasks(char *buffer, int count, struct list_head *nidmasks,
		   struct nidrange *nr)
{
	int i = 0;
	struct nidmask *nm;
	__u8 max;

	list_for_each_entry(nm, nidmasks, nm_link) {
		if (i != 0)
			i += scnprintf(buffer + i, count - i, " ");

		/* parse_nidmask() ensures nm_family is set to either AF_INET
		 * or AF_INET6
		 */
		if (nm->nm_family == AF_INET) {
			inet_ntop(nm->nm_family, &nm->nm_addr.ipv4.s_addr,
				  buffer + i, count - i);
			max = 32;
		} else {
			inet_ntop(nm->nm_family, &nm->nm_addr.ipv6.s6_addr,
				  buffer + i, count - i);
			max = 128;
		}
		i += strlen(buffer + i);
		if (nm->nm_prefix_len < max)
			i += scnprintf(buffer + i, count - i, "/%u",
				       nm->nm_prefix_len);
		i += cfs_print_network(buffer + i, count - i, nr);
	}

	return i;
}

/**
 * Print a list of nidranges (\a nidlist) into the specified \a buffer.
 * At max \a count characters can be printed into \a buffer.
 * Nidranges are separated by a space character.
 *
 * \retval number of characters written
 */
int cfs_print_nidlist(char *buffer, int count, struct list_head *nidlist)
{
	int i = 0;
	struct nidrange *nr;
	bool need_space = false;

	if (count <= 0)
		return 0;

	list_for_each_entry(nr, nidlist, nr_link) {
		if (i != 0)
			i += scnprintf(buffer + i, count - i, " ");

		if (nr->nr_all != 0) {
			assert(list_empty(&nr->nr_addrranges));
			assert(list_empty(&nr->nr_nidmasks));
			i += scnprintf(buffer + i, count - i, "*");
			i += cfs_print_network(buffer + i, count - i, nr);
			continue;
		}

		if (!list_empty(&nr->nr_nidmasks)) {
			i += cfs_print_nidmasks(buffer + i, count - i,
						&nr->nr_nidmasks, nr);
			need_space = true;
		}

		if (!list_empty(&nr->nr_addrranges)) {
			if (need_space)
				i += scnprintf(buffer + i, count - i, " ");

			i += cfs_print_addrranges(buffer + i, count - i,
						  &nr->nr_addrranges, nr);
		}
		need_space = false;
	}
	return i;
}

/**
 * Determines minimum and maximum addresses for a single
 * numeric address range
 *
 * \param	ar
 * \param[out]	*min_nid __u32 representation of min NID
 * \param[out]	*max_nid __u32 representation of max NID
 * \retval	-EINVAL unsupported LNET range
 * \retval	-ERANGE non-contiguous LNET range
 */
static int cfs_ip_ar_min_max(struct addrrange *ar, __u32 *min_nid,
			      __u32 *max_nid)
{
	struct cfs_expr_list *expr_list;
	struct cfs_range_expr *range;
	unsigned int min_ip[4] = {0};
	unsigned int max_ip[4] = {0};
	int cur_octet = 0;
	bool expect_full_octet = false;

	list_for_each_entry(expr_list, &ar->ar_numaddr_ranges, el_link) {
		int re_count = 0;

		list_for_each_entry(range, &expr_list->el_exprs, re_link) {
			/* XXX: add support for multiple & non-contig. re's */
			if (re_count > 0)
				return -EINVAL;

			/* if a previous octet was ranged, then all remaining
			 * octets must be full for contiguous range */
			if (expect_full_octet && (range->re_lo != 0 ||
						  range->re_hi != 255))
				return -ERANGE;

			if (range->re_stride != 1)
				return -ERANGE;

			if (range->re_lo > range->re_hi)
				return -EINVAL;

			if (range->re_lo != range->re_hi)
				expect_full_octet = true;

			min_ip[cur_octet] = range->re_lo;
			max_ip[cur_octet] = range->re_hi;

			re_count++;
		}

		cur_octet++;
	}

	if (min_nid != NULL)
		*min_nid = ((min_ip[0] << 24) | (min_ip[1] << 16) |
			    (min_ip[2] << 8) | min_ip[3]);

	if (max_nid != NULL)
		*max_nid = ((max_ip[0] << 24) | (max_ip[1] << 16) |
			    (max_ip[2] << 8) | max_ip[3]);

	return 0;
}

/**
 * Determines minimum and maximum addresses for a single
 * numeric address range
 *
 * \param	ar
 * \param[out]	*min_nid __u32 representation of min NID
 * \param[out]	*max_nid __u32 representation of max NID
 * \retval	-EINVAL unsupported LNET range
 */
static int cfs_num_ar_min_max(struct addrrange *ar, __u32 *min_nid,
			       __u32 *max_nid)
{
	struct cfs_expr_list *el;
	struct cfs_range_expr *re;
	unsigned int min_addr = 0;
	unsigned int max_addr = 0;

	list_for_each_entry(el, &ar->ar_numaddr_ranges, el_link) {
		int re_count = 0;

		list_for_each_entry(re, &el->el_exprs, re_link) {
			if (re_count > 0)
				return -EINVAL;
			if (re->re_lo > re->re_hi)
				return -EINVAL;

			if (re->re_lo < min_addr || min_addr == 0)
				min_addr = re->re_lo;
			if (re->re_hi > max_addr)
				max_addr = re->re_hi;

			re_count++;
		}
	}

	if (min_nid != NULL)
		*min_nid = min_addr;
	if (max_nid != NULL)
		*max_nid = max_addr;

	return 0;
}

/**
 * Takes a linked list of nidrange expressions, determines the minimum
 * and maximum nid and creates appropriate nid structures
 *
 * \param	*nidlist
 * \param[out]	*min_nid string representation of min NID
 * \param[out]	*max_nid string representation of max NID
 * \retval	-EINVAL unsupported LNET range
 * \retval	-ERANGE non-contiguous LNET range
 */
int cfs_nidrange_find_min_max(struct list_head *nidlist, char *min_nid,
			      char *max_nid, size_t nidstr_length)
{
	struct nidrange *first_nidrange;
	int netnum;
	struct netstrfns *nf;
	char *lndname;
	__u32 min_addr;
	__u32 max_addr;
	char min_addr_str[IPSTRING_LENGTH];
	char max_addr_str[IPSTRING_LENGTH];
	int rc;

	first_nidrange = list_first_entry(nidlist, struct nidrange, nr_link);

	netnum = first_nidrange->nr_netnum;
	nf = first_nidrange->nr_netstrfns;
	lndname = nf->nf_name;

	rc = nf->nf_min_max(nidlist, &min_addr, &max_addr);
	if (rc < 0)
		return rc;

	nf->nf_addr2str(min_addr, min_addr_str, sizeof(min_addr_str));
	nf->nf_addr2str(max_addr, max_addr_str, sizeof(max_addr_str));

	snprintf(min_nid, nidstr_length, "%s@%s%d", min_addr_str, lndname,
		 netnum);
	snprintf(max_nid, nidstr_length, "%s@%s%d", max_addr_str, lndname,
		 netnum);

	return 0;
}

/**
 * Determines the min and max NID values for num LNDs
 *
 * \param	*nidlist
 * \param[out]	*min_nid if provided, returns string representation of min NID
 * \param[out]	*max_nid if provided, returns string representation of max NID
 * \retval	-EINVAL unsupported LNET range
 * \retval	-ERANGE non-contiguous LNET range
 */
static int cfs_num_min_max(struct list_head *nidlist, __u32 *min_nid,
			    __u32 *max_nid)
{
	struct nidrange *nr;
	struct addrrange *ar;
	unsigned int tmp_min_addr = 0;
	unsigned int tmp_max_addr = 0;
	unsigned int min_addr = 0;
	unsigned int max_addr = 0;
	int nidlist_count = 0;
	int rc;

	list_for_each_entry(nr, nidlist, nr_link) {
		if (nidlist_count > 0)
			return -EINVAL;

		list_for_each_entry(ar, &nr->nr_addrranges, ar_link) {
			rc = cfs_num_ar_min_max(ar, &tmp_min_addr,
						&tmp_max_addr);
			if (rc < 0)
				return rc;

			if (tmp_min_addr < min_addr || min_addr == 0)
				min_addr = tmp_min_addr;
			if (tmp_max_addr > max_addr)
				max_addr = tmp_max_addr;
		}
	}
	if (max_nid != NULL)
		*max_nid = max_addr;
	if (min_nid != NULL)
		*min_nid = min_addr;

	return 0;
}

/**
 * Takes an nidlist and determines the minimum and maximum
 * ip addresses.
 *
 * \param	*nidlist
 * \param[out]	*min_nid if provided, returns string representation of min NID
 * \param[out]	*max_nid if provided, returns string representation of max NID
 * \retval	-EINVAL unsupported LNET range
 * \retval	-ERANGE non-contiguous LNET range
 */
static int cfs_ip_min_max(struct list_head *nidlist, __u32 *min_nid,
			   __u32 *max_nid)
{
	struct nidrange *nr;
	struct addrrange *ar;
	__u32 tmp_min_ip_addr = 0;
	__u32 tmp_max_ip_addr = 0;
	__u32 min_ip_addr = 0;
	__u32 max_ip_addr = 0;
	int nidlist_count = 0;
	int rc;

	list_for_each_entry(nr, nidlist, nr_link) {
		if (nidlist_count > 0)
			return -EINVAL;

		if (nr->nr_all) {
			min_ip_addr = 0;
			max_ip_addr = 0xffffffff;
			break;
		}

		list_for_each_entry(ar, &nr->nr_addrranges, ar_link) {
			rc = cfs_ip_ar_min_max(ar, &tmp_min_ip_addr,
					       &tmp_max_ip_addr);
			if (rc < 0)
				return rc;

			if (tmp_min_ip_addr < min_ip_addr || min_ip_addr == 0)
				min_ip_addr = tmp_min_ip_addr;
			if (tmp_max_ip_addr > max_ip_addr)
				max_ip_addr = tmp_max_ip_addr;
		}

		nidlist_count++;
	}

	if (max_nid != NULL)
		*max_nid = max_ip_addr;
	if (min_nid != NULL)
		*min_nid = min_ip_addr;

	return 0;
}

static int
libcfs_expand_nidrange(struct nidrange *nr, __u32 *addrs, int max_nids)
{
	struct addrrange *ar;
	int rc = 0, count = max_nids;
	struct netstrfns *nf = nr->nr_netstrfns;

	list_for_each_entry(ar, &nr->nr_addrranges, ar_link) {
		rc = nf->nf_expand_addrrange(&ar->ar_numaddr_ranges, addrs,
					     count);
		if (rc < 0)
			return rc;

		count -= rc;
	}

	return max_nids - count;
}

int cfs_expand_nidlist(struct list_head *nidlist, lnet_nid_t *lnet_nidlist,
		       int max_nids)
{
	struct nidrange *nr;
	int rc = 0, count = max_nids;
	int i, j = 0;
	__u32 *addrs;
	struct netstrfns *nf;
	__u32 net;

	addrs = calloc(max_nids, sizeof(__u32));
	if (!addrs)
		return -ENOMEM;

	list_for_each_entry(nr, nidlist, nr_link) {
		rc = libcfs_expand_nidrange(nr, addrs, count);

		if (rc < 0) {
			free(addrs);
			return rc;
		}

		nf = nr->nr_netstrfns;
		net = LNET_MKNET(nf->nf_type, nr->nr_netnum);

		for (i = count - 1; i >= count - rc; i--)
			lnet_nidlist[j++] = LNET_MKNID(net, addrs[i]);

		count -= rc;
	}

	free(addrs);
	return max_nids - count;
}

/* returns pointer to the next delimiter in nidstr or if no delimiters are
 * found then it returns a pointer to the termining NUL character
 * A delimiter can be a comma, colon, or space.
 */
char *cfs_nidstr_find_delimiter(char *nidstr)
{
	char *delimiter = nidstr;
	int hex_count = 0;
	int hex_sections = 0;
	bool is_ipv6 = true;

	/* address parsing */
	while (*delimiter != ',' && *delimiter != ' ' && *delimiter != '\0') {
		/* Need to skip : in IPv6 / GUID NIDs. Lustre also uses
		 * ':' as a separator, which makes this complicated.
		 */
		if (*delimiter == '@') {
			while (*delimiter != ':' && *delimiter != ',' &&
			       *delimiter != ' ' && *delimiter != '\0')
				delimiter++;
			break;
		}
		/* IPv6 addresses are in 0-4 hex digit groups */
		else if ((isxdigit(*delimiter) || *delimiter == ':') &&
			 hex_count <= 4 && is_ipv6) {
			if (*delimiter == ':') {
				hex_sections++;
				hex_count = 0;
			} else {
				hex_count++;
			}
		} else { /* NID is not IPv6 */
			is_ipv6 = false;
			if (*delimiter == ':')
				break;

		}
		delimiter++;
	}
	return delimiter;
}
