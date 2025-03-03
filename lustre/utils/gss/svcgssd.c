/*
 * gssd.c
 *
 * Copyright (c) 2000 The Regents of the University of Michigan.
 * All rights reserved.
 *
 * Copyright (c) 2000 Dug Song <dugsong@UMICH.EDU>.
 * Copyright (c) 2002 Andy Adamson <andros@UMICH.EDU>.
 * Copyright (c) 2002 Marius Aamodt Eriksen <marius@UMICH.EDU>.
 * Copyright (c) 2002 J. Bruce Fields <bfields@UMICH.EDU>.
 *
 * Copyright (c) 2016, Intel Corporation.
 *
 * All rights reserved, all wrongs reversed.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <errno.h>

#include <unistd.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <dirent.h>
#include "svcgssd.h"
#include "gss_util.h"
#include "err_util.h"
#include "lsupport.h"
#include <linux/lustre/lustre_ver.h>

int null_enabled;
int krb_enabled;
int sk_enabled;

static void
closeall(int min)
{
	DIR *dir = opendir("/proc/self/fd");
	if (dir != NULL) {
		int dfd = dirfd(dir);
		struct dirent *d;

		while ((d = readdir(dir)) != NULL) {
			char *endp;
			long n = strtol(d->d_name, &endp, 10);
			if (*endp != '\0' && n >= min && n != dfd)
				(void) close(n);
		}
		closedir(dir);
	} else {
		int fd = sysconf(_SC_OPEN_MAX);
		while (--fd >= min)
			(void) close(fd);
	}
}

/*
 * mydaemon creates a pipe between the partent and child
 * process. The parent process will wait until the
 * child dies or writes a '1' on the pipe signaling
 * that it started successfully.
 */
int pipefds[2] = { -1, -1};

static void
mydaemon(int nochdir, int noclose)
{
	int pid, status, tempfd;

	if (pipe(pipefds) < 0) {
		printerr(LL_ERR, "%s: pipe() failed: errno %d (%s)\n",
			 __func__, errno, strerror(errno));
		exit(1);
	}
	if ((pid = fork ()) < 0) {
		printerr(LL_ERR, "%s: fork() failed: errno %d (%s)\n",
			 __func__, errno, strerror(errno));
		exit(1);
	}

	if (pid != 0) {
		/*
		 * Parent. Wait for status from child.
		 */
		close(pipefds[1]);
		if (read(pipefds[0], &status, 1) != 1)
			exit(1);
		exit (0);
	}
	/* Child.	*/
	close(pipefds[0]);
	setsid ();
	if (nochdir == 0) {
		if (chdir ("/") == -1) {
			printerr(LL_ERR,
				 "%s: chdir() failed: errno %d (%s)\n",
				 __func__, errno, strerror(errno));
			exit(1);
		}
	}

	while (pipefds[1] <= 2) {
		pipefds[1] = dup(pipefds[1]);
		if (pipefds[1] < 0) {
			printerr(LL_ERR, "%s: dup() failed: errno %d (%s)\n",
				 __func__, errno, strerror(errno));
			exit(1);
		}
	}

	if (noclose == 0) {
		tempfd = open("/dev/null", O_RDWR);
		if (tempfd < 0) {
			printerr(LL_ERR, "%s: open() failed: errno %d (%s)\n",
				 __func__, errno, strerror(errno));
			exit(1);
		}
		dup2(tempfd, 0);
		dup2(tempfd, 1);
		dup2(tempfd, 2);
		closeall(3);
	}
}

static void
release_parent()
{
	int status;
	ssize_t sret __attribute__ ((unused));

	if (pipefds[1] > 0) {
		sret = write(pipefds[1], &status, 1);
		close(pipefds[1]);
		pipefds[1] = -1;
	}
}

static void
sig_die(int signal)
{
	/* cleanup allocated strings for realms */
	gssd_cleanup_realms();
	/* remove socket */
	unlink(GSS_SOCKET_PATH);
	printerr(LL_WARN, "exiting on signal %d\n", signal);
	if (signal == SIGTERM)
		exit(EXIT_SUCCESS);
	else
		exit(EXIT_FAILURE);
}

static void
sig_hup(int signal)
{
	/* don't exit on SIGHUP */
	printerr(LL_WARN, "Received SIGHUP... Ignoring.\n");
}

static void
usage(FILE *fp, char *progname)
{
	fprintf(fp, "usage: %s [ -fnvmogkRsz ]\n",
		progname);
	fprintf(stderr, "-f		- Run in foreground\n");
	fprintf(stderr, "-g		- Service MGS\n");
	fprintf(stderr, "-k		- Enable kerberos support\n");
	fprintf(stderr, "-m		- Service MDS\n");
	fprintf(stderr, "-n		- Don't establish kerberos credentials\n");
	fprintf(stderr, "-o		- Service OSS\n");
	fprintf(stderr, "-R REALM	- Kerberos Realm to use, instead of default\n");
#ifdef HAVE_OPENSSL_SSK
	fprintf(stderr, "-s		- Enable shared secret key support\n");
#endif
	fprintf(stderr, "-v		- Verbosity\n");
	fprintf(stderr, "-z		- Enable gssnull support\n");

	exit(fp == stderr);
}

int
main(int argc, char *argv[])
{
	int get_creds = 1;
	int fg = 0;
	int verbosity = 0;
	int opt;
	int must_srv_mds = 0, must_srv_oss = 0, must_srv_mgs = 0;
	char *realm = NULL;
	char *progname;

	while ((opt = getopt(argc, argv, "fgkmnoR:svz")) != -1) {
		switch (opt) {
		case 'f':
			fg = 1;
			break;
		case 'g':
			get_creds = 1;
			must_srv_mgs = 1;
			break;
		case 'h':
			usage(stdout, argv[0]);
			break;
		case 'k':
			krb_enabled = 1;
			break;
		case 'm':
			get_creds = 1;
			must_srv_mds = 1;
			break;
		case 'n':
			get_creds = 0;
			break;
		case 'o':
			get_creds = 1;
			must_srv_oss = 1;
			break;
		case 'R':
			realm = optarg;
			break;
		case 's':
#ifdef HAVE_OPENSSL_SSK
			sk_enabled = 1;
#else
			fprintf(stderr, "error: request for SSK but service "
				"support not enabled\n");
			usage(stderr, argv[0]);
#endif
			break;
		case 'v':
			verbosity++;
			break;
		case 'z':
			null_enabled = 1;
			break;
		default:
			usage(stderr, argv[0]);
			break;
		}
	}

	if ((progname = strrchr(argv[0], '/')))
		progname++;
	else
		progname = argv[0];

	if (!sk_enabled && !krb_enabled && !null_enabled) {
#if LUSTRE_VERSION_CODE < OBD_OCD_VERSION(3, 0, 53, 0)
		fprintf(stderr, "warning: no -k, -s, or -z option given, "
			"assume -k for backward compatibility\n");
		krb_enabled = 1;
#else
		fprintf(stderr, "error: need one of -k, -s, or -z options\n");
		usage(stderr, argv[0]);

#endif
	}

	if (realm && !krb_enabled) {
		fprintf(stderr, "error: need -k option if -R is used\n");
		usage(stderr, argv[0]);
	}

	initerr(progname, verbosity, fg);

	/* For kerberos use gss mechanisms but ignore for sk and null */
	if (krb_enabled) {
		int ret;

		if (gssd_check_mechs()) {
			printerr(LL_ERR,
				 "ERROR: problem with gssapi library\n");
			ret = -1;
			goto err_krb;
		}
		ret = gss_get_realm(realm);
		if (ret) {
			printerr(LL_ERR, "ERROR: no Kerberos realm: %s\n",
				 error_message(ret));
			goto err_krb;
		}
		printerr(LL_WARN, "Kerberos realm: %s\n", krb5_this_realm);
		if (get_creds &&
		    gssd_prepare_creds(must_srv_mgs, must_srv_mds,
				       must_srv_oss)) {
			printerr(LL_ERR,
				 "unable to obtain root (machine) credentials\n");
			printerr(LL_ERR,
				 "do you have a keytab entry for <lustre_xxs>/<your.host>@<YOUR.REALM> in /etc/krb5.keytab?\n");
			ret = -1;
			goto err_krb;
		}

err_krb:
		if (ret) {
			krb_enabled = 0;
			printerr(LL_ERR, "ERROR: disabling Kerberos support\n");
			if (!sk_enabled && !krb_enabled && !null_enabled)
				exit(EXIT_FAILURE);
		}
	}

	if (!fg)
		mydaemon(0, 0);

	/*
	 * XXX: There is risk of memory leak for missing call
	 *	cleanup_mapping() for SIGKILL and SIGSTOP.
	 */
	signal(SIGINT, sig_die);
	signal(SIGTERM, sig_die);
	signal(SIGHUP, sig_hup);

	if (!fg)
		release_parent();

	gssd_init_unique(GSSD_SVC);

	svcgssd_run();
	gssd_cleanup_realms();
	printerr(LL_ERR, "svcgssd_run returned!\n");
	abort();
}
