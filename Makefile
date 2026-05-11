prefix		?= /usr/local
exec_prefix	?= $(prefix)
bindir		?= $(exec_prefix)/bin
datadir		?= $(prefix)/share
pkgdatadir	?= $(datadir)/wwvsim
mandir		?= $(datadir)/man
wwv_dir		?= $(pkgdatadir)/wwv
wwvh_dir	?= $(pkgdatadir)/wwvh
incdir		?= $(prefix)/include
docdir		?= $(datadir)/doc/wwvsim

UNAME_S := $(shell uname -s)

export prefix exec_prefix bindir
export mandir wwv_dir wwvh_dir

CFLAGS=-g -O3 -I $(incdir)

all:	wwvsim

clean:
	rm -f *.o wwvsim

install: wwvsim	
	install -d -m 0755 $(DESTDIR)$(bindir) $(DESTDIR)$(wwv_dir) $(DESTDIR)$(wwvh_dir) $(DESTDIR)$(docdir)
	install -m 0644 NIST-250-67.pdf $(DESTDIR)$(docdir)
	install -m 0755 wwvsim $(DESTDIR)$(bindir)
	install -m 0644 wwv-id.txt wwv-id.raw $(DESTDIR)$(wwv_dir)
	install -m 0644 wwvh-id.txt wwvh-id.raw $(DESTDIR)$(wwvh_dir)
	install -m 0644 test.raw $(DESTDIR)$(wwv_dir)
	ln $(DESTDIR)$(wwv_dir)/test.raw $(DESTDIR)$(wwvh_dir)
	(cd $(DESTDIR)$(wwv_dir); ln wwv-id.raw 0.raw; ln wwv-id.raw 30.raw; ln wwv-id.txt 0.txt; ln wwv-id.txt 30.txt; ln test.raw 8.raw)
	(cd $(DESTDIR)$(wwvh_dir); ln wwvh-id.raw 0.raw; ln wwvh-id.raw 30.raw; ln wwvh-id.txt 0.txt; ln wwvh-id.txt 30.txt; ln test.raw 48.raw)
	install -d -m 0755 $(DESTDIR)/

wwvsim: wwvsim.o
	$(CC) -g -o $@ $^ -lportaudio -lm -lpthread
