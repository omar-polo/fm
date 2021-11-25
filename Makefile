LDLIBS=-lncursesw
PREFIX=/usr/local
MANPREFIX=$(PREFIX)/man
BINDIR=$(DESTDIR)$(PREFIX)/bin
MANDIR=$(DESTDIR)$(MANPREFIX)/man1

all: fm

fm: fm.c config.h
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) $(LDLIBS)

install: fm
	rm -f $(BINDIR)/fm
	mkdir -p $(BINDIR)
	cp fm $(BINDIR)/fm
	mkdir -p $(MANDIR)
	cp fm.1 $(MANDIR)/fm.1

uninstall:
	rm -f $(BINDIR)/fm
	rm -f $(MANDIR)/fm.1

clean:
	rm -f fm
