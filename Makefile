LDLIBS =		-lncursesw
PREFIX =		/usr/local
MANPREFIX =		${PREFIX}/man
BINDIR =		${DESTDIR}${PREFIX}/bin
MANDIR =		${DESTDIR}${MANPREFIX}/man1

INSTALL =		install
INSTALL_PROGRAM =	${INSTALL} -m 0555
INSTALL_MAN =		${INSTALL} -m 0444

all: fm

fm: fm.o
	${CC} ${CFLAGS} -o $@ fm.o ${LDFLAGS} ${LDLIBS}

fm.o: fm.c config.h

install: fm
	mkdir -p ${BINDIR}
	${INSTALL_PROGRAM} fm ${BINDIR}/fm
	mkdir -p ${MANDIR}
	${INSTALL_MAN} fm.1 ${MANDIR}/fm.1

uninstall:
	rm -f $(BINDIR)/fm
	rm -f $(MANDIR)/fm.1

clean:
	rm -f fm
