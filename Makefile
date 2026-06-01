prefix		?= /usr/local
BUILD		?= release
exec_prefix	?= $(prefix)
bindir		?= $(exec_prefix)/bin
datadir		?= $(prefix)/share
pkgdatadir	?= $(datadir)/wwvsim
mandir		?= $(datadir)/man
wwvdir		?= $(pkgdatadir)/wwv
wwvhdir		?= $(pkgdatadir)/wwvh
incdir		?= $(prefix)/include
docdir		?= $(datadir)/doc/wwvsim

export prefix exec_prefix bindir
export mandir wwvdir wwvhdir

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

.PHONY: all clean install clips

all:	wwvsim clips

clips: $(SUBDIRS)
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d DESTDIR=$(DESTDIR) || exit $$?; \
	done

clean:
	rm -f *.o wwvsim paths.h
	for d in $(SUBDIRS); do \
		$(MAKE) -C $$d clean DESTDIR=$(DESTDIR) || exit $$?; \
	done

install: wwvsim
	install -d -m 0755 $(DESTDIR)$(bindir) $(DESTDIR)$(docdir)
	install -d -m 0755 $(DESTDIR)$(wwvdir) $(DESTDIR)$(wwvdir)/minute $(DESTDIR)$(wwvdir)/announce
	install -d -m 0755 $(DESTDIR)$(wwvhdir) $(DESTDIR)$(wwvhdir)/minute $(DESTDIR)$(wwvhdir)/announce
	install -m 0644 NIST-250-67.pdf $(DESTDIR)$(docdir)
	install -m 0755 wwvsim $(DESTDIR)$(bindir)
	rsync -vaR wwv/*/*.raw wwvh/*/*.raw $(DESTDIR)$(pkgdatadir)

wwvsim: wwvsim.o timecode.o
	$(CC) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# handle quotes inside GIT summary messages, etc. Suggested by ChatGPT
esc = sed 's/\\/\\\\/g; s/"/\\"/g'
paths.h: Makefile
	echo "make $@"
	@printf '#ifndef _CONFIG_PATHS_H\n' > $@
	@printf '#define _CONFIG_PATHS_H 1\n' >> $@
	@printf '#define WWV_DIR "%s"\n' '$(wwvdir)' >> $@
	@printf '#define WWVH_DIR "%s"\n' '$(wwvhdir)' >> $@
	@printf '#endif\n' >> $@

%.o: %.c paths.h
	$(CC) $(CFLAGS) -c -o $@ $<

DEPS = $(CFILES:.c=.d) $(OBJS:.o=.d)
-include $(DEPS)

