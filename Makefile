prefix		?= /usr/local
BUILD		?= release
exec_prefix	?= $(prefix)
bindir		?= $(exec_prefix)/bin
datadir		?= $(prefix)/share
pkgdatadir	?= $(datadir)/wwvsim
mandir		?= $(datadir)/man
incdir		?= $(prefix)/include
docdir		?= $(datadir)/doc/wwvsim
cachedir	?= /var/cache/wwvsim

export prefix exec_prefix bindir
export mandir wwvdir wwvhdir wwvcache wwvhcache

CFILES = wwvsim.c timecode.c

LDLIBS += -lportaudio -lm -lpthread 

UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S),Darwin)
	CPPFLAGS += -I/opt/local/include
	LDFLAGS  += -L/opt/local/lib
else
	LDLIBS += -lbsd
endif

ifeq ($(BUILD),debug)
	DOPTS = -g -Og -fno-omit-frame-pointer
else
	DOPTS = -DNDEBUG=1 -O3
endif

SUBDIRS=wwv/minute wwvh/minute

CFLAGS=$(DOPTS) $(CPPFLAGS)

.PHONY: all clean install

all:	wwvsim

clean:
	rm -rf *.o wwvsim paths.h debian/tmp debian/.debhelper debian/wwvsim

install: wwvsim
	install -d -m 0755 $(DESTDIR)$(bindir) $(DESTDIR)$(pkgdatadir)
	install -d -m 02775 -g radio $(DESTDIR)$(cachedir) $(DESTDIR)$(cachedir)/wwv $(DESTDIR)$(cachedir)/wwvh
	install -d -m 02775 -g radio $(DESTDIR)$(cachedir)/wwv/announce $(DESTDIR)$(cachedir)/wwv/minute
	install -d -m 02775 -g radio $(DESTDIR)$(cachedir)/wwvh/announce $(DESTDIR)$(cachedir)/wwvh/minute
	install -m 02755 -g radio wwvsim $(DESTDIR)$(bindir)
	rsync -vaRH wwv wwvh $(DESTDIR)$(pkgdatadir)

wwvsim: wwvsim.o timecode.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# handle quotes inside GIT summary messages, etc. Suggested by ChatGPT
esc = sed 's/\\/\\\\/g; s/"/\\"/g'
paths.h: Makefile
	@printf '#ifndef _CONFIG_PATHS_H\n' > $@
	@printf '#define _CONFIG_PATHS_H 1\n' >> $@
	@printf '#define CACHE_DIR "%s"\n' '$(cachedir)' >> $@
	@printf '#define SHARE_DIR "%s"\n' '$(pkgdatadir)' >> $@
	@printf '#endif\n' >> $@

%.o: %.c paths.h
	$(CC) $(CFLAGS) -c -o $@ $<

DEPS = $(CFILES:.c=.d) $(OBJS:.o=.d)
-include $(DEPS)

