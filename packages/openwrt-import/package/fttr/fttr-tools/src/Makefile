# SPDX-License-Identifier: GPL-2.0-only
#
# miniolt, built from the sources beside this file.
#
# fmcs_uapi.h sits here too: it is the userspace ABI, and the copy in
# fttr-fmcs/src/ must stay byte-identical to it.

CC	?= $(CROSS_COMPILE)gcc
CFLAGS	?= -O2 -Wall -Wextra

PREFIX	?= /usr
BINDIR	?= $(PREFIX)/bin

PROGRAMS := miniolt

all: $(PROGRAMS)

miniolt: miniolt.c fmcs_uapi.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ miniolt.c

install: $(PROGRAMS)
	install -d $(DESTDIR)$(BINDIR)
	install -m 0755 $(PROGRAMS) $(DESTDIR)$(BINDIR)/

clean:
	rm -f $(PROGRAMS)

.PHONY: all install clean
