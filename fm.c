/* needed for some ncurses stuff */
#define _XOPEN_SOURCE_EXTENDED
#define _FILE_OFFSET_BITS   64

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <ctype.h>
#include <curses.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <locale.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#include <wctype.h>

#include "config.h"

struct option opts[] = {
	{"help",	no_argument,	NULL,	'h'},
	{"version",	no_argument,	NULL,	'v'},
	{NULL,		0,		NULL,	0}
};

/* String buffers. */
#define BUFLEN  PATH_MAX
static char BUF1[BUFLEN];
static char BUF2[BUFLEN];
static char INPUT[BUFLEN];
static char CLIPBOARD[BUFLEN];
static wchar_t WBUF[BUFLEN];

/* Paths to external programs. */
static char *user_shell;
static char *user_pager;
static char *user_editor;
static char *user_open;

/* Listing view parameters. */
#define HEIGHT      (LINES-4)
#define STATUSPOS   (COLS-16)

/* Listing view flags. */
#define SHOW_FILES      0x01u
#define SHOW_DIRS       0x02u
#define SHOW_HIDDEN     0x04u

/* Marks parameters. */
#define BULK_INIT   5
#define BULK_THRESH 256

/* Information associated to each entry in listing. */
struct row {
	char *name;
	off_t size;
	mode_t mode;
	int islink;
	int marked;
};

/* Dynamic array of marked entries. */
struct marks {
	char dirpath[PATH_MAX];
	int bulk;
	int nentries;
	char **entries;
};

/* Line editing state. */
struct edit {
	wchar_t buffer[BUFLEN + 1];
	int left, right;
};

/* Each tab only stores the following information. */
struct tab {
	int scroll;
	int esel;
	uint8_t flags;
	char cwd[PATH_MAX];
};

struct prog {
	off_t partial;
	off_t total;
	const char *msg;
};

/* Global state. */
static struct state {
	int tab;
	int nfiles;
	struct row *rows;
	WINDOW *window;
	struct marks marks;
	struct edit edit;
	int edit_scroll;
	volatile sig_atomic_t pending_usr1;
	volatile sig_atomic_t pending_winch;
	struct prog prog;
	struct tab tabs[10];
} fm;

/* Macros for accessing global state. */
#define ENAME(I)    fm.rows[I].name
#define ESIZE(I)    fm.rows[I].size
#define EMODE(I)    fm.rows[I].mode
#define ISLINK(I)   fm.rows[I].islink
#define MARKED(I)   fm.rows[I].marked
#define SCROLL      fm.tabs[fm.tab].scroll
#define ESEL        fm.tabs[fm.tab].esel
#define FLAGS       fm.tabs[fm.tab].flags
#define CWD         fm.tabs[fm.tab].cwd

/* Helpers. */
#define MIN(A, B)   ((A) < (B) ? (A) : (B))
#define MAX(A, B)   ((A) > (B) ? (A) : (B))
#define ISDIR(E)    (strchr((E), '/') != NULL)
#define CTRL(x)     ((x) & 0x1f)
#define nitems(a)   (sizeof(a)/sizeof(a[0]))

/* Line Editing Macros. */
#define EDIT_FULL(E)       ((E).left == (E).right)
#define EDIT_CAN_LEFT(E)   ((E).left)
#define EDIT_CAN_RIGHT(E)  ((E).right < BUFLEN-1)
#define EDIT_LEFT(E)       (E).buffer[(E).right--] = (E).buffer[--(E).left]
#define EDIT_RIGHT(E)      (E).buffer[(E).left++] = (E).buffer[++(E).right]
#define EDIT_INSERT(E, C)  (E).buffer[(E).left++] = (C)
#define EDIT_BACKSPACE(E)  (E).left--
#define EDIT_DELETE(E)     (E).right++
#define EDIT_CLEAR(E)      do { (E).left = 0; (E).right = BUFLEN-1; } while(0)

enum editstate { CONTINUE, CONFIRM, CANCEL };
enum color { DEFAULT, RED, GREEN, YELLOW, BLUE, CYAN, MAGENTA, WHITE, BLACK };

typedef int (*PROCESS)(const char *path);

#ifndef __dead
#define __dead __attribute__((noreturn))
#endif

static inline __dead void
quit(const char *reason)
{
	int saved_errno;

	saved_errno = errno;
	endwin();
	nocbreak();
	fflush(stderr);
	errno = saved_errno;
	err(1, "%s", reason);
}

static inline void *
xmalloc(size_t size)
{
	void *d;

	if ((d = malloc(size)) == NULL)
		quit("malloc");
	return d;
}

static inline void *
xcalloc(size_t nmemb, size_t size)
{
	void *d;

	if ((d = calloc(nmemb, size)) == NULL)
		quit("calloc");
	return d;
}

static inline void *
xrealloc(void *p, size_t size)
{
	void *d;

	if ((d = realloc(p, size)) == NULL)
		quit("realloc");
	return d;
}

static void
init_marks(struct marks *marks)
{
	strcpy(marks->dirpath, "");
	marks->bulk = BULK_INIT;
	marks->nentries = 0;
	marks->entries = xcalloc(marks->bulk, sizeof(*marks->entries));
}

/* Unmark all entries. */
static void
mark_none(struct marks *marks)
{
	int i;

	strcpy(marks->dirpath, "");
	for (i = 0; i < marks->bulk && marks->nentries; i++)
		if (marks->entries[i]) {
			free(marks->entries[i]);
			marks->entries[i] = NULL;
			marks->nentries--;
		}
	if (marks->bulk > BULK_THRESH) {
	        /* Reset bulk to free some memory. */
		free(marks->entries);
		marks->bulk = BULK_INIT;
		marks->entries = xcalloc(marks->bulk, sizeof(*marks->entries));
	}
}

static void
add_mark(struct marks *marks, char *dirpath, char *entry)
{
	int i;

	if (!strcmp(marks->dirpath, dirpath)) {
        	/* Append mark to directory. */
		if (marks->nentries == marks->bulk) {
			/* Expand bulk to accomodate new entry. */
			int extra = marks->bulk / 2;
			marks->bulk += extra; /* bulk *= 1.5; */
			marks->entries = xrealloc(marks->entries,
			    marks->bulk * sizeof(*marks->entries));
			memset(&marks->entries[marks->nentries], 0,
			    extra * sizeof(*marks->entries));
			i = marks->nentries;
		} else {
			/* Search for empty slot (there must be one). */
			for (i = 0; i < marks->bulk; i++)
				if (!marks->entries[i])
					break;
		}
	} else {
		/* Directory changed. Discard old marks. */
		mark_none(marks);
		strcpy(marks->dirpath, dirpath);
		i = 0;
	}
	marks->entries[i] = xmalloc(strlen(entry) + 1);
	strcpy(marks->entries[i], entry);
	marks->nentries++;
}

static void
del_mark(struct marks *marks, char *entry)
{
	int i;

	if (marks->nentries > 1) {
		for (i = 0; i < marks->bulk; i++)
			if (marks->entries[i] &&
			    !strcmp(marks->entries[i], entry))
				break;
		free(marks->entries[i]);
		marks->entries[i] = NULL;
		marks->nentries--;
	} else
		mark_none(marks);
}

static void
free_marks(struct marks *marks)
{
	int i;

	for (i = 0; i < marks->bulk && marks->nentries; i++)
		if (marks->entries[i]) {
			free(marks->entries[i]);
			marks->nentries--;
		}
	free(marks->entries);
}

static void
handle_usr1(int sig)
{
	fm.pending_usr1 = 1;
}

static void
handle_winch(int sig)
{
	fm.pending_winch = 1;
}

static void
enable_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = handle_usr1;
	sigaction(SIGUSR1, &sa, NULL);
	sa.sa_handler = handle_winch;
	sigaction(SIGWINCH, &sa, NULL);
}

static void
disable_handlers(void)
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(struct sigaction));
	sa.sa_handler = SIG_DFL;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGWINCH, &sa, NULL);
}

static void reload(void);
static void update_view(void);

/* Handle any signals received since last call. */
static void
sync_signals(void)
{
	if (fm.pending_usr1) {
		/* SIGUSR1 received: refresh directory listing. */
		reload();
		fm.pending_usr1 = 0;
	}
	if (fm.pending_winch) {
		/* SIGWINCH received: resize application accordingly. */
		delwin(fm.window);
		endwin();
		refresh();
		clear();
		fm.window = subwin(stdscr, LINES - 2, COLS, 1, 0);
		if (HEIGHT < fm.nfiles && SCROLL + HEIGHT > fm.nfiles)
			SCROLL = ESEL - HEIGHT;
		update_view();
		fm.pending_winch = 0;
	}
}

/*
 * This function must be used in place of getch().  It handles signals
 * while waiting for user input.
 */
static int
fm_getch()
{
	int ch;

	while ((ch = getch()) == ERR)
		sync_signals();
	return ch;
}

/*
 * This function must be used in place of get_wch().  It handles
 * signals while waiting for user input.
 */
static int
fm_get_wch(wint_t *wch)
{
	wint_t ret;

	while ((ret = get_wch(wch)) == (wint_t)ERR)
		sync_signals();
	return ret;
}

/* Get user programs from the environment. */

#define FM_ENV(dst, src) if ((dst = getenv("FM_" #src)) == NULL)	\
		dst = getenv(#src);

static void
get_user_programs()
{
	FM_ENV(user_shell, SHELL);
	FM_ENV(user_pager, PAGER);
	FM_ENV(user_editor, VISUAL);
	if (!user_editor)
		FM_ENV(user_editor, EDITOR);
	FM_ENV(user_open, OPEN);
}

/* Do a fork-exec to external program (e.g. $EDITOR). */
static void
spawn(const char *argv0, ...)
{
	pid_t pid;
	int status;
	size_t i;
	const char *argv[16], *last;
	va_list ap;

	memset(argv, 0, sizeof(argv));

	va_start(ap, argv0);
	argv[0] = argv0;
	for (i = 1; i < nitems(argv); ++i) {
		last = va_arg(ap, const char *);
		if (last == NULL)
			break;
		argv[i] = last;
	}
	va_end(ap);

	if (last != NULL)
		abort();

	disable_handlers();
	endwin();

	switch (pid = fork()) {
	case -1:
		quit("fork");
	case 0: /* child */
		setenv("RVSEL", fm.nfiles ? ENAME(ESEL) : "", 1);
		execvp(argv[0], (char *const *)argv);
		quit("execvp");
	default:
		waitpid(pid, &status, 0);
		enable_handlers();
		kill(getpid(), SIGWINCH);
	}
}

static void
shell_escaped_cat(char *buf, char *str, size_t n)
{
	char *p = buf + strlen(buf);
	*p++ = '\'';
	for (n--; n; n--, str++) {
		switch (*str) {
		case '\'':
			if (n < 4)
				goto done;
			strcpy(p, "'\\''");
			n -= 4;
			p += 4;
			break;
		case '\0':
			goto done;
		default:
			*p = *str;
			p++;
		}
	}
done:
	strncat(p, "'", n);
}

static int
open_with_env(char *program, char *path)
{
	if (program) {
#ifdef RV_SHELL
		strncpy(BUF1, program, BUFLEN - 1);
		strncat(BUF1, " ", BUFLEN - strlen(program) - 1);
		shell_escaped_cat(BUF1, path, BUFLEN - strlen(program) - 2);
		spawn(RV_SHELL, "-c", BUF1, NULL );
#else
		spawn(program, path, NULL);
#endif
		return 1;
	}
	return 0;
}

/* Curses setup. */
static void
init_term()
{
	setlocale(LC_ALL, "");
	initscr();
	cbreak(); /* Get one character at a time. */
	timeout(100); /* For getch(). */
	noecho();
	nonl(); /* No NL->CR/NL on output. */
	intrflush(stdscr, FALSE);
	keypad(stdscr, TRUE);
	curs_set(FALSE); /* Hide blinking cursor. */
	if (has_colors()) {
		short bg;
		start_color();
#ifdef NCURSES_EXT_FUNCS
		use_default_colors();
		bg = -1;
#else
		bg = COLOR_BLACK;
#endif
		init_pair(RED, COLOR_RED, bg);
		init_pair(GREEN, COLOR_GREEN, bg);
		init_pair(YELLOW, COLOR_YELLOW, bg);
		init_pair(BLUE, COLOR_BLUE, bg);
		init_pair(CYAN, COLOR_CYAN, bg);
		init_pair(MAGENTA, COLOR_MAGENTA, bg);
		init_pair(WHITE, COLOR_WHITE, bg);
		init_pair(BLACK, COLOR_BLACK, bg);
	}
	atexit((void (*)(void))endwin);
	enable_handlers();
}

/* Update the listing view. */
static void
update_view()
{
	int i, j;
	int numsize;
	int ishidden;
	int marking;

	mvhline(0, 0, ' ', COLS);
	attr_on(A_BOLD, NULL);
	color_set(RVC_TABNUM, NULL);
	mvaddch(0, COLS - 2, fm.tab + '0');
	attr_off(A_BOLD, NULL);
	if (fm.marks.nentries) {
		numsize = snprintf(BUF1, BUFLEN, "%d", fm.marks.nentries);
		color_set(RVC_MARKS, NULL);
		mvaddstr(0, COLS - 3 - numsize, BUF1);
	} else
		numsize = -1;
	color_set(RVC_CWD, NULL);
	mbstowcs(WBUF, CWD, PATH_MAX);
	mvaddnwstr(0, 0, WBUF, COLS - 4 - numsize);
	wcolor_set(fm.window, RVC_BORDER, NULL);
	wborder(fm.window, 0, 0, 0, 0, 0, 0, 0, 0);
	ESEL = MAX(MIN(ESEL, fm.nfiles - 1), 0);

	/*
	 * Selection might not be visible, due to cursor wrapping or
	 * window shrinking. In that case, the scroll must be moved to
	 * make it visible.
	 */
	if (fm.nfiles > HEIGHT) {
		SCROLL = MAX(MIN(SCROLL, ESEL), ESEL - HEIGHT + 1);
		SCROLL = MIN(MAX(SCROLL, 0), fm.nfiles - HEIGHT);
	} else
		SCROLL = 0;
	marking = !strcmp(CWD, fm.marks.dirpath);
	for (i = 0, j = SCROLL; i < HEIGHT && j < fm.nfiles; i++, j++) {
		ishidden = ENAME(j)[0] == '.';
		if (j == ESEL)
			wattr_on(fm.window, A_REVERSE, NULL);
		if (ISLINK(j))
			wcolor_set(fm.window, RVC_LINK, NULL);
		else if (ishidden)
			wcolor_set(fm.window, RVC_HIDDEN, NULL);
		else if (S_ISREG(EMODE(j))) {
			if (EMODE(j) & (S_IXUSR | S_IXGRP | S_IXOTH))
				wcolor_set(fm.window, RVC_EXEC, NULL);
			else
				wcolor_set(fm.window, RVC_REG, NULL);
		} else if (S_ISDIR(EMODE(j)))
			wcolor_set(fm.window, RVC_DIR, NULL);
		else if (S_ISCHR(EMODE(j)))
			wcolor_set(fm.window, RVC_CHR, NULL);
		else if (S_ISBLK(EMODE(j)))
			wcolor_set(fm.window, RVC_BLK, NULL);
		else if (S_ISFIFO(EMODE(j)))
			wcolor_set(fm.window, RVC_FIFO, NULL);
		else if (S_ISSOCK(EMODE(j)))
			wcolor_set(fm.window, RVC_SOCK, NULL);
		if (S_ISDIR(EMODE(j))) {
			mbstowcs(WBUF, ENAME(j), PATH_MAX);
			if (ISLINK(j))
				wcscat(WBUF, L"/");
		} else {
			const char *suffix, *suffixes = "BKMGTPEZY";
			off_t human_size = ESIZE(j) * 10;
			int length = mbstowcs(WBUF, ENAME(j), PATH_MAX);
			int namecols = wcswidth(WBUF, length);
			for (suffix = suffixes; human_size >= 10240; suffix++)
				human_size = (human_size + 512) / 1024;
			if (*suffix == 'B')
				swprintf(WBUF + length, PATH_MAX - length,
				    L"%*d %c",
				    (int)(COLS - namecols - 6),
				    (int)human_size / 10, *suffix);
			else
				swprintf(WBUF + length, PATH_MAX - length,
				    L"%*d.%d %c",
				    (int)(COLS - namecols - 8),
				    (int)human_size / 10,
				    (int)human_size % 10, *suffix);
		}
		mvwhline(fm.window, i + 1, 1, ' ', COLS - 2);
		mvwaddnwstr(fm.window, i + 1, 2, WBUF, COLS - 4);
		if (marking && MARKED(j)) {
			wcolor_set(fm.window, RVC_MARKS, NULL);
			mvwaddch(fm.window, i + 1, 1, RVS_MARK);
		} else
			mvwaddch(fm.window, i + 1, 1, ' ');
		if (j == ESEL)
			wattr_off(fm.window, A_REVERSE, NULL);
	}
	for (; i < HEIGHT; i++)
		mvwhline(fm.window, i + 1, 1, ' ', COLS - 2);
	if (fm.nfiles > HEIGHT) {
		int center, height;
		center = (SCROLL + HEIGHT / 2) * HEIGHT / fm.nfiles;
		height = (HEIGHT - 1) * HEIGHT / fm.nfiles;
		if (!height)
			height = 1;
		wcolor_set(fm.window, RVC_SCROLLBAR, NULL);
		mvwvline(fm.window, center - height/2 + 1, COLS - 1,
		    RVS_SCROLLBAR, height);
	}
	BUF1[0] = FLAGS & SHOW_FILES ? 'F' : ' ';
	BUF1[1] = FLAGS & SHOW_DIRS ? 'D' : ' ';
	BUF1[2] = FLAGS & SHOW_HIDDEN ? 'H' : ' ';
	if (!fm.nfiles)
		strcpy(BUF2, "0/0");
	else
		snprintf(BUF2, BUFLEN, "%d/%d", ESEL + 1, fm.nfiles);
	snprintf(BUF1 + 3, BUFLEN - 3, "%12s", BUF2);
	color_set(RVC_STATUS, NULL);
	mvaddstr(LINES - 1, STATUSPOS, BUF1);
	wrefresh(fm.window);
}

/* Show a message on the status bar. */
static void __attribute__((format(printf, 2, 3)))
message(enum color c, const char *fmt, ...)
{
	int len, pos;
	va_list args;

	va_start(args, fmt);
	vsnprintf(BUF1, MIN(BUFLEN, STATUSPOS), fmt, args);
	va_end(args);
	len = strlen(BUF1);
	pos = (STATUSPOS - len) / 2;
	attr_on(A_BOLD, NULL);
	color_set(c, NULL);
	mvaddstr(LINES - 1, pos, BUF1);
	color_set(DEFAULT, NULL);
	attr_off(A_BOLD, NULL);
}

/* Clear message area, leaving only status info. */
static void
clear_message()
{
	mvhline(LINES - 1, 0, ' ', STATUSPOS);
}

/* Comparison used to sort listing entries. */
static int
rowcmp(const void *a, const void *b)
{
	int isdir1, isdir2, cmpdir;
	const struct row *r1 = a;
	const struct row *r2 = b;
	isdir1 = S_ISDIR(r1->mode);
	isdir2 = S_ISDIR(r2->mode);
	cmpdir = isdir2 - isdir1;
	return cmpdir ? cmpdir : strcoll(r1->name, r2->name);
}

/* Get all entries in current working directory. */
static int
ls(struct row **rowsp, uint8_t flags)
{
	DIR *dp;
	struct dirent *ep;
	struct stat statbuf;
	struct row *rows;
	int i, n;

	if (!(dp = opendir(".")))
		return -1;
	n = -2; /* We don't want the entries "." and "..". */
	while (readdir(dp))
		n++;
	if (n == 0) {
		closedir(dp);
		return 0;
	}
	rewinddir(dp);
	rows = xmalloc(n * sizeof(*rows));
	i = 0;
	while ((ep = readdir(dp))) {
		if (!strcmp(ep->d_name, ".") || !strcmp(ep->d_name, ".."))
			continue;
		if (!(flags & SHOW_HIDDEN) && ep->d_name[0] == '.')
			continue;
		lstat(ep->d_name, &statbuf);
		rows[i].islink = S_ISLNK(statbuf.st_mode);
		stat(ep->d_name, &statbuf);
		if (S_ISDIR(statbuf.st_mode)) {
			if (flags & SHOW_DIRS) {
				rows[i].name = xmalloc(strlen(ep->d_name) + 2);
				strcpy(rows[i].name, ep->d_name);
				if (!rows[i].islink)
					strcat(rows[i].name, "/");
				rows[i].mode = statbuf.st_mode;
				i++;
			}
		} else if (flags & SHOW_FILES) {
			rows[i].name = xmalloc(strlen(ep->d_name) + 1);
			strcpy(rows[i].name, ep->d_name);
			rows[i].size = statbuf.st_size;
			rows[i].mode = statbuf.st_mode;
			i++;
		}
	}
	n = i; /* Ignore unused space in array caused by filters. */
	qsort(rows, n, sizeof(*rows), rowcmp);
	closedir(dp);
	*rowsp = rows;
	return n;
}

static void
free_rows(struct row **rowsp, int nfiles)
{
	int i;

	for (i = 0; i < nfiles; i++)
		free((*rowsp)[i].name);
	free(*rowsp);
	*rowsp = NULL;
}

/* Change working directory to the path in CWD. */
static void
cd(int reset)
{
	int i, j;

	message(CYAN, "Loading \"%s\"...", CWD);
	refresh();
	if (chdir(CWD) == -1) {
		getcwd(CWD, PATH_MAX - 1);
		if (CWD[strlen(CWD) - 1] != '/')
			strcat(CWD, "/");
		goto done;
	}
	if (reset)
		ESEL = SCROLL = 0;
	if (fm.nfiles)
		free_rows(&fm.rows, fm.nfiles);
	fm.nfiles = ls(&fm.rows, FLAGS);
	if (!strcmp(CWD, fm.marks.dirpath)) {
		for (i = 0; i < fm.nfiles; i++) {
			for (j = 0; j < fm.marks.bulk; j++)
				if (fm.marks.entries[j] &&
				    !strcmp(fm.marks.entries[j], ENAME(i)))
					break;
			MARKED(i) = j < fm.marks.bulk;
		}
	} else
		for (i = 0; i < fm.nfiles; i++)
			MARKED(i) = 0;
done:
	clear_message();
	update_view();
}

/* Select a target entry, if it is present. */
static void
try_to_sel(const char *target)
{
	ESEL = 0;
	if (!ISDIR(target))
		while ((ESEL + 1) < fm.nfiles && S_ISDIR(EMODE(ESEL)))
			ESEL++;
	while ((ESEL + 1) < fm.nfiles && strcoll(ENAME(ESEL), target) < 0)
		ESEL++;
}

/* Reload CWD, but try to keep selection. */
static void
reload()
{
	if (fm.nfiles) {
		strcpy(INPUT, ENAME(ESEL));
		cd(0);
		try_to_sel(INPUT);
		update_view();
	} else
		cd(1);
}

static off_t
count_dir(const char *path)
{
	DIR *dp;
	struct dirent *ep;
	struct stat statbuf;
	char subpath[PATH_MAX];
	off_t total;

	if (!(dp = opendir(path)))
		return 0;
	total = 0;
	while ((ep = readdir(dp))) {
		if (!strcmp(ep->d_name, ".") || !strcmp(ep->d_name, ".."))
			continue;
		snprintf(subpath, PATH_MAX, "%s%s", path, ep->d_name);
		lstat(subpath, &statbuf);
		if (S_ISDIR(statbuf.st_mode)) {
			strcat(subpath, "/");
			total += count_dir(subpath);
		} else
			total += statbuf.st_size;
	}
	closedir(dp);
	return total;
}

static off_t
count_marked()
{
	int i;
	char *entry;
	off_t total;
	struct stat statbuf;

	total = 0;
	chdir(fm.marks.dirpath);
	for (i = 0; i < fm.marks.bulk; i++) {
		entry = fm.marks.entries[i];
		if (entry) {
			if (ISDIR(entry)) {
				total += count_dir(entry);
			} else {
				lstat(entry, &statbuf);
				total += statbuf.st_size;
			}
		}
	}
	chdir(CWD);
	return total;
}

/*
 * Recursively process a source directory using CWD as destination
 * root.  For each node (i.e. directory), do the following:
 *
 * 1. call pre(destination);
 * 2. call proc() on every child leaf (i.e. files);
 * 3. recurse into every child node;
 * 4. call pos(source).
 *
 * E.g. to move directory /src/ (and all its contents) inside /dst/:
 * strcpy(CWD, "/dst/");
 * process_dir(adddir, movfile, deldir, "/src/");
 */
static int
process_dir(PROCESS pre, PROCESS proc, PROCESS pos, const char *path)
{
	int ret;
	DIR *dp;
	struct dirent *ep;
	struct stat statbuf;
	char subpath[PATH_MAX];

	ret = 0;
	if (pre) {
		char dstpath[PATH_MAX];
		strcpy(dstpath, CWD);
		strcat(dstpath, path + strlen(fm . marks . dirpath));
		ret |= pre(dstpath);
	}
	if (!(dp = opendir(path)))
		return -1;
	while ((ep = readdir(dp))) {
		if (!strcmp(ep->d_name, ".") || !strcmp(ep->d_name, ".."))
			continue;
		snprintf(subpath, PATH_MAX, "%s%s", path, ep->d_name);
		lstat(subpath, &statbuf);
		if (S_ISDIR(statbuf.st_mode)) {
			strcat(subpath, "/");
			ret |= process_dir(pre, proc, pos, subpath);
		} else
			ret |= proc(subpath);
	}
	closedir(dp);
	if (pos)
		ret |= pos(path);
	return ret;
}

/*
 * Process all marked entries using CWD as destination root.  All
 * marked entries that are directories will be recursively processed.
 * See process_dir() for details on the parameters.
 */
static void
process_marked(PROCESS pre, PROCESS proc, PROCESS pos, const char *msg_doing,
    const char *msg_done)
{
	int i, ret;
	char *entry;
	char path[PATH_MAX];

	clear_message();
	message(CYAN, "%s...", msg_doing);
	refresh();
	fm.prog = (struct prog){0, count_marked(), msg_doing};
	for (i = 0; i < fm.marks.bulk; i++) {
		entry = fm.marks.entries[i];
		if (entry) {
			ret = 0;
			snprintf(path, PATH_MAX, "%s%s", fm.marks.dirpath,
			    entry);
			if (ISDIR(entry)) {
				if (!strncmp(path, CWD, strlen(path)))
					ret = -1;
				else
					ret = process_dir(pre, proc, pos, path);
			} else
				ret = proc(path);
			if (!ret) {
				del_mark(&fm.marks, entry);
				reload();
			}
		}
	}
	fm.prog.total = 0;
	reload();
	if (!fm.marks.nentries)
		message(GREEN, "%s all marked entries.", msg_done);
	else
		message(RED, "Some errors occured while %s.", msg_doing);
	RV_ALERT();
}

static void
update_progress(off_t delta)
{
	int percent;

	if (!fm.prog.total)
		return;
	fm.prog.partial += delta;
	percent = (int)(fm.prog.partial * 100 / fm.prog.total);
	message(CYAN, "%s...%d%%", fm.prog.msg, percent);
	refresh();
}

/* Wrappers for file operations. */
static int
delfile(const char *path)
{
	int ret;
	struct stat st;

	ret = lstat(path, &st);
	if (ret < 0)
		return ret;
	update_progress(st.st_size);
	return unlink(path);
}

static PROCESS deldir = rmdir;
static int
addfile(const char *path)
{
	/* Using creat(2) because mknod(2) doesn't seem to be portable. */
	int ret;

	ret = creat(path, 0644);
	if (ret < 0)
		return ret;
	return close(ret);
}

static int
cpyfile(const char *srcpath)
{
	int src, dst, ret;
	size_t size;
	struct stat st;
	char buf[BUFSIZ];
	char dstpath[PATH_MAX];

	strcpy(dstpath, CWD);
	strcat(dstpath, srcpath + strlen(fm.marks.dirpath));
	ret = lstat(srcpath, &st);
	if (ret < 0)
		return ret;
	if (S_ISLNK(st.st_mode)) {
		ret = readlink(srcpath, BUF1, BUFLEN - 1);
		if (ret < 0)
			return ret;
		BUF1[ret] = '\0';
		ret = symlink(BUF1, dstpath);
	} else {
		ret = src = open(srcpath, O_RDONLY);
		if (ret < 0)
			return ret;
		ret = dst = creat(dstpath, st.st_mode);
		if (ret < 0)
			return ret;
		while ((size = read(src, buf, BUFSIZ)) > 0) {
			write(dst, buf, size);
			update_progress(size);
			sync_signals();
		}
		close(src);
		close(dst);
		ret = 0;
	}
	return ret;
}

static int
adddir(const char *path)
{
	int ret;
	struct stat st;

	ret = stat(CWD, &st);
	if (ret < 0)
		return ret;
	return mkdir(path, st.st_mode);
}

static int
movfile(const char *srcpath)
{
	int ret;
	struct stat st;
	char dstpath[PATH_MAX];

	strcpy(dstpath, CWD);
	strcat(dstpath, srcpath + strlen(fm.marks.dirpath));
	ret = rename(srcpath, dstpath);
	if (ret == 0) {
		ret = lstat(dstpath, &st);
		if (ret < 0)
			return ret;
		update_progress(st.st_size);
	} else if (errno == EXDEV) {
		ret = cpyfile(srcpath);
		if (ret < 0)
			return ret;
		ret = unlink(srcpath);
	}
	return ret;
}

static void
start_line_edit(const char *init_input)
{
	curs_set(TRUE);
	strncpy(INPUT, init_input, BUFLEN);
	fm.edit.left = mbstowcs(fm.edit.buffer, init_input, BUFLEN);
	fm.edit.right = BUFLEN - 1;
	fm.edit.buffer[BUFLEN] = L'\0';
	fm.edit_scroll = 0;
}

/* Read input and change editing state accordingly. */
static enum editstate
get_line_edit()
{
	wchar_t eraser, killer, wch;
	int ret, length;

	ret = fm_get_wch((wint_t *)&wch);
	erasewchar(&eraser);
	killwchar(&killer);
	if (ret == KEY_CODE_YES) {
		if (wch == KEY_ENTER) {
			curs_set(FALSE);
			return CONFIRM;
		} else if (wch == KEY_LEFT) {
			if (EDIT_CAN_LEFT(fm.edit))
				EDIT_LEFT(fm.edit);
		} else if (wch == KEY_RIGHT) {
			if (EDIT_CAN_RIGHT(fm.edit))
				EDIT_RIGHT(fm.edit);
		} else if (wch == KEY_UP) {
			while (EDIT_CAN_LEFT(fm.edit))
				EDIT_LEFT(fm.edit);
		} else if (wch == KEY_DOWN) {
			while (EDIT_CAN_RIGHT(fm.edit))
				EDIT_RIGHT(fm.edit);
		} else if (wch == KEY_BACKSPACE) {
			if (EDIT_CAN_LEFT(fm.edit))
				EDIT_BACKSPACE(fm.edit);
		} else if (wch == KEY_DC) {
			if (EDIT_CAN_RIGHT(fm.edit))
				EDIT_DELETE(fm.edit);
		}
	} else {
		if (wch == L'\r' || wch == L'\n') {
			curs_set(FALSE);
			return CONFIRM;
		} else if (wch == L'\t') {
			curs_set(FALSE);
			return CANCEL;
		} else if (wch == eraser) {
			if (EDIT_CAN_LEFT(fm.edit))
				EDIT_BACKSPACE(fm.edit);
		} else if (wch == killer) {
			EDIT_CLEAR(fm.edit);
			clear_message();
		} else if (iswprint(wch)) {
			if (!EDIT_FULL(fm.edit))
				EDIT_INSERT(fm.edit, wch);
		}
	}
	/* Encode edit contents in INPUT. */
	fm.edit.buffer[fm.edit.left] = L'\0';
	length = wcstombs(INPUT, fm.edit.buffer, BUFLEN);
	wcstombs(&INPUT[length], &fm.edit.buffer[fm.edit.right + 1],
	    BUFLEN - length);
	return CONTINUE;
}

/* Update line input on the screen. */
static void
update_input(const char *prompt, enum color c)
{
	int plen, ilen, maxlen;

	plen = strlen(prompt);
	ilen = mbstowcs(NULL, INPUT, 0);
	maxlen = STATUSPOS - plen - 2;
	if (ilen - fm.edit_scroll < maxlen)
		fm.edit_scroll = MAX(ilen - maxlen, 0);
	else if (fm.edit.left > fm.edit_scroll + maxlen - 1)
		fm.edit_scroll = fm.edit.left - maxlen;
	else if (fm.edit.left < fm.edit_scroll)
		fm.edit_scroll = MAX(fm.edit.left - maxlen, 0);
	color_set(RVC_PROMPT, NULL);
	mvaddstr(LINES - 1, 0, prompt);
	color_set(c, NULL);
	mbstowcs(WBUF, INPUT, COLS);
	mvaddnwstr(LINES - 1, plen, &WBUF[fm.edit_scroll], maxlen);
	mvaddch(LINES - 1, plen + MIN(ilen - fm.edit_scroll, maxlen + 1),
	    ' ');
	color_set(DEFAULT, NULL);
	if (fm.edit_scroll)
		mvaddch(LINES - 1, plen - 1, '<');
	if (ilen > fm.edit_scroll + maxlen)
		mvaddch(LINES - 1, plen + maxlen, '>');
	move(LINES - 1, plen + fm.edit.left - fm.edit_scroll);
}

static void
cmd_down(void)
{
	if (fm.nfiles)
		ESEL = MIN(ESEL + 1, fm.nfiles - 1);
}

static void
cmd_up(void)
{
	if (fm.nfiles)
		ESEL = MAX(ESEL - 1, 0);
}

static void
cmd_scroll_down(void)
{
	if (!fm.nfiles)
		return;
	ESEL = MIN(ESEL + HEIGHT, fm.nfiles - 1);
	if (fm.nfiles > HEIGHT)
		SCROLL = MIN(SCROLL + HEIGHT, fm.nfiles - HEIGHT);
}

static void
cmd_scroll_up(void)
{
	if (!fm.nfiles)
		return;
	ESEL = MAX(ESEL - HEIGHT, 0);
	SCROLL = MAX(SCROLL - HEIGHT, 0);
}

static void
cmd_man(void)
{
	spawn("man", "fm", NULL);
}

static void
loop(void)
{
	int meta, ch, c;
	struct binding {
		int ch;
#define K_META 1
#define K_CTRL 2
		int chflags;
		void (*fn)(void);
#define X_UPDV 1
#define X_QUIT 2
		int flags;
	} bindings[] = {
		{'?',		0,	cmd_man,		0},
		{'J',		0,	cmd_scroll_down,	X_UPDV},
		{'K',		0,	cmd_scroll_up,		X_UPDV},
		{'V',		K_CTRL,	cmd_scroll_down,	X_UPDV},
		{'g',		K_CTRL,	NULL,			X_UPDV},
		{'j',		0,	cmd_down,		X_UPDV},
		{'k',		0,	cmd_up,			X_UPDV},
		{'n',		0,	cmd_down,		X_UPDV},
		{'n',		K_CTRL,	cmd_down,		X_UPDV},
		{'p',		0,	cmd_up,			X_UPDV},
		{'p',		K_CTRL,	cmd_up,			X_UPDV},
		{'q',		0,	NULL,			X_QUIT},
		{'v',		K_META,	cmd_scroll_up,		X_UPDV},
		{KEY_NPAGE,	0,	cmd_scroll_down,	X_UPDV},
		{KEY_PPAGE,	0,	cmd_scroll_up,		X_UPDV},
		{KEY_RESIZE,	0,	NULL,			X_UPDV},
		{KEY_RESIZE,	K_META,	NULL,			X_UPDV},
	}, *b;
	size_t i;

	for (;;) {
	again:
		meta = 0;
		ch = fm_getch();
		if (ch == '\e') {
			meta = 1;
			if ((ch = fm_getch()) == '\e') {
				meta = 0;
				ch = '\e';
			}
		}

		clear_message();

		for (i = 0; i < nitems(bindings); ++i) {
			b = &bindings[i];
			c = b->ch;
			if (b->chflags & K_CTRL)
				c = CTRL(c);
			if ((!meta && b->chflags & K_META) || ch != c)
				continue;

			if (b->flags & X_QUIT)
				return;
			if (b->fn != NULL)
				b->fn();
			if (b->flags & X_UPDV)
				update_view();

			goto again;
		}

		message(RED, "%s%s is undefined",
		    meta ? "M-": "", keyname(ch));
		refresh();
	}
}

int
main(int argc, char *argv[])
{
	int i, ch;
	char *program;
	char *entry;
	const char *key;
	const char *clip_path;
	DIR *d;
	enum editstate edit_stat;
	FILE *save_cwd_file = NULL;
	FILE *save_marks_file = NULL;
	FILE *clip_file;

	while ((ch = getopt_long(argc, argv, "d:hm:v", opts, NULL)) != -1) {
		switch (ch) {
		case 'd':
			if ((save_cwd_file = fopen(optarg, "w")) == NULL)
				err(1, "open %s", optarg);
			break;
		case 'h':
                        printf(""
			    "Usage: fm [-hv] [-d file] [-m file] [dirs...]\n"
			    "Browse current directory or the ones specified.\n"
			    "\n"
			    "See fm(1) for more information.\n"
			    "fm homepage <https://github.com/omar-polo/fm>\n");
			return 0;
		case 'm':
			if ((save_marks_file = fopen(optarg, "a")) == NULL)
				err(1, "open %s", optarg);
			break;
		case 'v':
			printf("version: fm %s\n", RV_VERSION);
			return 0;
		}
	}

	get_user_programs();
	init_term();
	fm.nfiles = 0;
	for (i = 0; i < 10; i++) {
		fm.tabs[i].esel = fm.tabs[i].scroll = 0;
		fm.tabs[i].flags = RV_FLAGS;
	}
	strcpy(fm.tabs[0].cwd, getenv("HOME"));
	for (i = 1; i < argc && i < 10; i++) {
		if ((d = opendir(argv[i]))) {
			realpath(argv[i], fm.tabs[i].cwd);
			closedir(d);
		} else
			strcpy(fm.tabs[i].cwd, fm.tabs[0].cwd);
	}
	getcwd(fm.tabs[i].cwd, PATH_MAX);
	for (i++; i < 10; i++)
		strcpy(fm.tabs[i].cwd, fm.tabs[i - 1].cwd);
	for (i = 0; i < 10; i++)
		if (fm.tabs[i].cwd[strlen(fm.tabs[i].cwd) - 1] != '/')
			strcat(fm.tabs[i].cwd, "/");
	fm.tab = 1;
	fm.window = subwin(stdscr, LINES - 2, COLS, 1, 0);
	init_marks(&fm.marks);
	cd(1);
	strcpy(CLIPBOARD, CWD);
	if (fm.nfiles > 0)
		strcat(CLIPBOARD, ENAME(ESEL));

	loop();

	if (fm.nfiles)
		free_rows(&fm.rows, fm.nfiles);
	delwin(fm.window);
	if (save_cwd_file != NULL) {
		fputs(CWD, save_cwd_file);
		fclose(save_cwd_file);
	}
	if (save_marks_file != NULL) {
		for (i = 0; i < fm.marks.bulk; i++) {
			entry = fm.marks.entries[i];
			if (entry)
				fprintf(save_marks_file, "%s%s\n",
				    fm.marks.dirpath, entry);
		}
		fclose(save_marks_file);
	}
	free_marks(&fm.marks);
	return 0;
}
