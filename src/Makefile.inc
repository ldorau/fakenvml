# Copyright (c) 2014, Intel Corporation
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
#     * Redistributions of source code must retain the above copyright
#       notice, this list of conditions and the following disclaimer.
#
#     * Redistributions in binary form must reproduce the above copyright
#       notice, this list of conditions and the following disclaimer in
#       the documentation and/or other materials provided with the
#       distribution.
#
#     * Neither the name of Intel Corporation nor the names of its
#       contributors may be used to endorse or promote products derived
#       from this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#
# src/Makefile.inc -- common Makefile rules for NVM library
#

MACHINE := $(shell uname -m)

ifeq ($(MACHINE), x86_64)
ifneq ($(wildcard /usr/lib64),)
LIBDIR = usr/lib64
else
LIBDIR = usr/lib
endif
else
LIBDIR = usr/lib
endif

LIBS_DESTDIR = $(DESTDIR)/$(LIBDIR)/$(VARIANT_DESTDIR)

vpath %.c ..
vpath %.h .. ../include
INCS = -I.. -I../include

CFLAGS = -std=gnu99 -ggdb -Wall -Werror
LDFLAGS = -Wl,-z,relro

LN = ln
PMEMSOVERSION = 1
PMEMLIBVERSION = 0.0
LIBPMEMSO=libpmem.so
LIBPMEMAR=libpmem.a
LIBPMEM_SONAME=$(LIBPMEMSO).$(PMEMSOVERSION)
LIBPMEM_REALNAME=$(LIBPMEM_SONAME).$(PMEMLIBVERSION)

COMMONOBJS = out.o util.o
PMEMOBJS = libpmem.o blk.o btt.o log.o obj.o obj_list.o pmem.o allocator.o $(COMMONOBJS)
PMEMMAPFILE = ../libpmem.map
TARGET_LIBS = $(LIBPMEMAR) $(LIBPMEM_REALNAME)
TARGET_LINKS= $(LIBPMEMSO) $(LIBPMEM_SONAME)
TARGETS = $(TARGET_LIBS) $(TARGET_LINKS)

$(LIBPMEM_AR) $(LIBPMEM_REALNAME): LIBS += -luuid -lrt

out.o: CFLAGS += -DSRCVERSION='"$(SRCVERSION)"'

all: $(TARGETS)

$(LIBPMEMAR): $(PMEMOBJS)
	$(LD) -o $*_unscoped.o -r $(PMEMOBJS)
	objcopy --localize-hidden `sed -n 's/^	*\([a-zA-Z0-9_]*\);$$/-G \1/p' $(PMEMMAPFILE)` $*_unscoped.o $*_all.o
	$(AR) rv $@ $*_all.o

$(LIBPMEM_REALNAME): $(PMEMOBJS)
	$(CC) $(LDFLAGS) -shared -Wl,--version-script=$(PMEMMAPFILE),-soname,$(LIBPMEM_SONAME) -o $@ $(PMEMOBJS) $(LIBS)

$(LIBPMEM_SONAME): $(LIBPMEM_REALNAME)
	$(LN) -sf $< $@

$(LIBPMEMSO): $(LIBPMEM_SONAME)
	$(LN) -sf $< $@


.c.o:
	$(CC) -c -o $@ $(CFLAGS) $(INCS) -fPIC $<

clean:
	$(RM) *.o core a.out

clobber: clean
	$(RM) $(TARGETS)

.PHONY: all clean clobber

libpmem.o: libpmem.c libpmem.h pmem.h util.h out.h
blk.o: blk.c libpmem.h pmem.h blk.h util.h out.h
btt.o: btt.c util.h btt.h btt_layout.h
log.o: log.c libpmem.h pmem.h log.h util.h out.h
pmem.o: pmem.c libpmem.h pmem.h out.h
obj.o: obj.c libpmem.h pmem.h obj.h util.h out.h allocator.h
obj_list.o: obj_list.c libpmem.h obj_list.h
allocator.o: allocator.c
out.o: out.c out.h
util.o: util.c util.h out.h
