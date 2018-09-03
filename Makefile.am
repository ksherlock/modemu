# SUBDIRS = src
packextrasdir=@docdir@
EXTRA_DIST = 	COPYING README
packextras_DATA=$(EXTRA_DIST)

LEX	= flex

PACKAGE = @PACKAGE@
VERSION = @VERSION@

ACLOCAL_AMFLAGS = --install -I m4

distdir = $(PACKAGE)-$(VERSION)

AUTOMAKE_OPTIONS = subdir-objects foreign no-dependencies

bin_PROGRAMS = modemu
AM_CFLAGS = -std=c99 -Wall -g -D_GNU_SOURCE
modemu_SOURCES =atcmd.c   commx.c   sockbuf.c  stty.c    timeval.c \
verbose.c cmdarg.c  modemu.c  sock.c     telopt.c  ttybuf.c lex.yy.c

lex.yy.c:
	$(LEX) $(LFLAGS) cmdlex.l
