# SPDX-License-Identifier: GPL-2.0

#
# This file is part of Lustre, http://www.lustre.org/
#

MANFILES =					\
	ldev.conf.5				\
	nids.5

if MANPAGES
if UTILS
man_MANS = $(MANFILES)
endif
endif

CLEANFILES = *.aux *.tex *.log *.pdf
EXTRA_DIST = $(MANFILES)

all:
