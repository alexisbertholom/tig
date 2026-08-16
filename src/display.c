/* Copyright (c) 2006-2026 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/tig.h"
#include "tig/argv.h"
#include "tig/io.h"
#include "tig/repo.h"
#include "tig/options.h"
#include "tig/view.h"
#include "tig/draw.h"
#include "tig/display.h"
#include "tig/watch.h"
#include "tig/search.h"
#include "tig/csearch.h"

#ifdef HAVE_READLINE
#include <readline/readline.h>
#endif /* HAVE_READLINE */

#define MAX_KEYS 2000

static void set_terminal_modes(void);

struct view *display[2];
unsigned int current_view;

static WINDOW *display_win[2];
static WINDOW *display_title[2];
static WINDOW *display_sep;

struct display_tty {
	FILE *file;
	int fd;
	struct termios *attr;
	pid_t opgrp;
};
static struct display_tty opt_tty = { NULL, -1, NULL, -1 };

static struct io script_io = { -1 };

bool
is_script_executing(void)
{
	return script_io.pipe != -1;
}

enum status_code
open_script(const char *path)
{
	if (is_script_executing())
		return error("Scripts cannot be run from scripts");

	char buf[SIZEOF_STR];

	if (!path_expand(buf, sizeof(buf), path))
		return error("Failed to expand path: %s", path);

	return io_open(&script_io, "%s", buf)
		? SUCCESS : error("Failed to open %s", buf);
}

bool
open_external_viewer(const char *argv[], const char *dir, bool silent, bool confirm, bool echo, bool quick, bool do_refresh, const char *notice)
{
	bool ok;

	if (echo) {
		struct io io;
		char buf[SIZEOF_STR] = "";

		ok = io_exec(&io, IO_RD, dir, NULL, argv, IO_RD_WITH_STDERR) && io_read_buf(&io, buf, sizeof(buf), true);
		if (*buf)
			report("%s", buf);

	} else if (silent || is_script_executing()) {
		ok = io_run_bg(argv, dir);

	} else {
		/* Neutralize the ncurses SIGTSTP handler; a suspended
		 * editor signals the whole process group, and on resume
		 * the handler would repaint tig's screen and reset tty
		 * modes while the editor still owns the terminal. */
		void (*tstp_handler)(int) = signal(SIGTSTP, SIG_DFL);

		signal(SIGINT, SIG_IGN);
		clear();
		refresh();
		endwin();                  /* restore original tty modes */
		tcsetattr(opt_tty.fd, TCSAFLUSH, opt_tty.attr);
		ok = io_run_fg(argv, dir, opt_tty.fd);
		if (confirm || !ok) {
			if (!ok && *notice)
				fprintf(stderr, "%s", notice);

			if (!ok || !quick) {
				fprintf(stderr, "Press Enter to continue");
				getc(opt_tty.file);
			}
		}
		fseek(opt_tty.file, 0, SEEK_END);
		tcsetattr(opt_tty.fd, TCSAFLUSH, opt_tty.attr);
		set_terminal_modes();
		signal(SIGINT, SIG_DFL);
		signal(SIGTSTP, tstp_handler);
	}

	if (watch_update(WATCH_EVENT_AFTER_COMMAND) && do_refresh) {
		struct view *view;
		int i;

		foreach_displayed_view (view, i) {
			if (watch_dirty(&view->watch))
				refresh_view(view);
		}
	}
	redraw_display(true);
	return ok;
}

static void
shell_escape(char *dst, size_t dstlen, const char *src)
{
	size_t pos = 0;

	dst[pos++] = '\'';			/* opening quote */
	for (; *src && pos < dstlen - 2; ++src) {
		if (*src == '\'') {
			if (pos + 4 >= dstlen)
				break;
			dst[pos++] = '\'';	/* close current quote */
			dst[pos++] = '\\'; dst[pos++] = '\'';
						/* insert escaped quote */
			dst[pos++] = '\'';	/* reopen quote */
		} else {
			dst[pos++] = *src;
		}
	}
	dst[pos++] = '\'';			/* closing quote */
	dst[pos] = '\0';
}

#define EDITOR_LINENO_MSG \
	"*** Your editor reported an error while opening the file.\n" \
	"*** This is probably because it doesn't support the line\n" \
	"*** number argument added automatically. The line number\n" \
	"*** has been disabled for now. You can permanently disable\n" \
	"*** it by adding the following line to ~/.tigrc\n" \
	"***	set editor-line-number = no\n"

void
open_editor(const char *file, unsigned int lineno)
{
	char escaped_file[SIZEOF_STR];
	char editor_cmd[SIZEOF_STR];
	const char *editor_argv[] = { "sh", "-c", editor_cmd, NULL };
	const char *editor;

	editor = getenv("TIG_EDITOR");
	if (!editor)
		editor = getenv("GIT_EDITOR");
	if (!editor && *opt_editor)
		editor = opt_editor;
	if (!editor)
		editor = getenv("VISUAL");
	if (!editor)
		editor = getenv("EDITOR");
	if (!editor)
		editor = "vi";

	shell_escape(escaped_file, sizeof(escaped_file), file);

	if (lineno && opt_editor_line_number)
		string_format(editor_cmd, "%s +%u %s",
			editor,
			lineno,
			escaped_file);
	else
		string_format(editor_cmd, "%s %s",
			editor,
			escaped_file);

	if (!open_external_viewer(editor_argv, repo.cdup, false, false, false, false, true, EDITOR_LINENO_MSG))
		opt_editor_line_number = false;
}


static void
apply_horizontal_split(struct view *base, struct view *view)
{
	view->width   = base->width;
	view->height  = apply_step(opt_split_view_height, base->height);
	view->height  = MAX(view->height, MIN_VIEW_HEIGHT);
	view->height  = MIN(view->height, base->height - MIN_VIEW_HEIGHT);
	base->height -= view->height;
}

int
apply_vertical_split(int base_width)
{
	int width  = apply_step(opt_split_view_width, base_width);

	width = MAX(width, MIN_VIEW_WIDTH);
	width = MIN(width, base_width - MIN_VIEW_WIDTH);

	return width;
}

bool
vertical_split_is_enabled(enum vertical_split vsplit, int height, int width)
{
	if (vsplit == VERTICAL_SPLIT_AUTO)
		return width > 160 || width * VSPLIT_SCALE > (height - 1) * 2;
	return vsplit == VERTICAL_SPLIT_VERTICAL;
}

static void
redraw_display_separator(bool clear)
{
	if (display_sep) {
		int lineno = 0;

		if (clear)
			wclear(display_sep);
		wbkgdset(display_sep, get_line_attr(NULL, LINE_TITLE_BLUR));

		switch (opt_line_graphics) {
		case GRAPHIC_ASCII:
			while (mvwaddch(display_sep, lineno++, 0, '|') == OK);
			break;
		case GRAPHIC_DEFAULT:
			while (mvwaddch(display_sep, lineno++, 0, ACS_VLINE) == OK);
			break;
		case GRAPHIC_UTF_8:
			while (mvwaddstr(display_sep, lineno++, 0, "│") == OK);
			break;
		}

		wnoutrefresh(display_sep);
	}
}

static void create_or_move_display_separator(int height, int x)
{
	if (!display_sep) {
		display_sep = newwin(height, 1, 0, x);
		if (!display_sep)
			die("Failed to create separator window");

	} else {
		wresize(display_sep, height, 1);
		mvwin(display_sep, 0, x);
	}
}

static void remove_display_separator(void)
{
	if (display_sep) {
		delwin(display_sep);
		display_sep = NULL;
	}
}

void
resize_display(void)
{
	int x, y, i;
	int height, width;
	struct view *base = display[0];
	struct view *view = display[1] ? display[1] : display[0];
	bool vsplit;

	/* Setup window dimensions */

	getmaxyx(stdscr, height, width);

	/* Make room for the status window. */
	base->height = height - 1;
	base->width = width;

	vsplit = vertical_split_is_enabled(opt_vertical_split, height, width);

	if (view != base) {
		if (vsplit) {
			view->height = base->height;
			view->width = apply_vertical_split(base->width);
			base->width -= view->width;

			/* Make room for the separator bar. */
			view->width -= 1;

			create_or_move_display_separator(base->height, base->width);
			redraw_display_separator(false);
		} else {
			remove_display_separator();
			apply_horizontal_split(base, view);
		}

		/* Make room for the title bar. */
		view->height -= 1;

	} else {
		remove_display_separator();
	}

	/* Make room for the title bar. */
	base->height -= 1;

	x = y = 0;

	foreach_displayed_view (view, i) {
		if (!display_win[i]) {
			display_win[i] = newwin(view->height, view->width, y, x);
			if (!display_win[i])
				die("Failed to create %s view", view->name);

			scrollok(display_win[i], false);

			display_title[i] = newwin(1, view->width, y + view->height, x);
			if (!display_title[i])
				die("Failed to create title window");

		} else {
			wresize(display_win[i], view->height, view->width);
			mvwin(display_win[i], y, x);
			wresize(display_title[i], 1, view->width);
			mvwin(display_title[i], y + view->height, x);
		}

		view->win = display_win[i];
		view->title = display_title[i];

		if (vsplit)
			x += view->width + 1;
		else
			y += view->height + 1;
	}

	redraw_display_separator(false);
}

void
redraw_display(bool clear)
{
	struct view *view;
	int i;

	foreach_displayed_view (view, i) {
		if (clear)
			wclear(view->win);
		redraw_view(view);
		update_view_title(view);
	}

	redraw_display_separator(clear);
}

static bool
save_window_line(FILE *file, WINDOW *win, int y, char *buf, size_t bufsize)
{
	int read = mvwinnstr(win, y, 0, buf, bufsize);
	const char *out = read == ERR ? "" : string_trim_end(buf);

	return read == ERR ? false : fprintf(file, "%s\n", out) == strlen(out) + 1;
}

static bool
save_window_vline(FILE *file, WINDOW *left, WINDOW *right, int y, char *buf, size_t bufsize)
{
	int read1 = mvwinnstr(left, y, 0, buf, bufsize);
	int read2 = read1 == ERR ? ERR : mvwinnstr(right, y, 0, buf + read1 + 1, bufsize - read1 - 1);

	if (read2 == ERR)
		return false;
	buf[read1] = '|';
	buf = string_trim_end(buf);

	return fprintf(file, "%s\n", string_trim_end(buf)) == strlen(buf) + 1;
}

bool
save_display(const char *path)
{
	int i, width;
	size_t linelen;
	char *line;
	FILE *file = fopen(path, "w");
	bool ok = true;
	struct view *view = display[0];

	if (!file)
		return false;

	getmaxyx(stdscr, i, width);
	linelen = width * 4;
	line = malloc(linelen + 1);
	if (!line) {
		fclose(file);
		return false;
	}

	if (view->width < width && display[1]) {
		struct view *left = display[0],
			    *right = display[1];

		for (i = 0; ok && i < left->height; i++)
			ok = save_window_vline(file, left->win, right->win, i, line, linelen);
		if (ok)
			ok = save_window_vline(file, left->title, right->title, 0, line, linelen);
	} else {
		int j;

		foreach_displayed_view (view, j) {
			for (i = 0; ok && i < view->height; i++)
				ok = save_window_line(file, view->win, i, line, linelen);
			if (ok)
				ok = save_window_line(file, view->title, 0, line, linelen);
		}
	}

	free(line);
	fclose(file);
	return ok;
}

/*
 * Dump view data to file.
 *
 * FIXME: Add support for more line state and column data.
 */
bool
save_view(struct view *view, const char *path)
{
	struct view_column_data column_data = {0};
	FILE *file = fopen(path, "w");
	size_t i;

	if (!file)
		return false;

	fprintf(file, "View: %s\n", view->name);
	if (view->prev && view->prev != view)
		fprintf(file, "Prev: %s\n", view->prev->name);
	if (view->parent)
		fprintf(file, "Parent: %s\n", view->parent->name);
	fprintf(file, "Ref: %s\n", view->ref);
	fprintf(file, "Dimensions: height=%d width=%d\n", view->height, view->width);
	fprintf(file, "Position: offset=%lu column=%lu lineno=%lu\n",
		view->pos.offset,
		view->pos.col,
		view->pos.lineno);

	for (i = 0; i < view->lines; i++) {
		struct line *line = &view->line[i];

		fprintf(file, "line[%3zu] type=%s selected=%u\n",
			i,
			enum_name(get_line_type_name(line->type)),
			line->selected);

		if (view->columns &&
		    view->ops->get_column_data(view, line, &column_data) &&
		    column_data.box) {
			const struct box *box = column_data.box;
			size_t j;
			size_t offset;

			fprintf(file, "line[%3zu] cells=%zu text=",
				i, box->cells);

			for (j = 0, offset = 0; j < box->cells; j++) {
				const struct box_cell *cell = &box->cell[j];

				fprintf(file, "[%.*s]", (int) cell->length, box->text + offset);
				offset += cell->length;
			}

			fprintf(file, "\n");
		}
	}

	fclose(file);
	return true;
}

/*
 * Status management
 */

/* Whether or not the curses interface has been initialized. */
static bool cursed = false;

/* The status window is used for polling keystrokes. */
WINDOW *status_win;

/* Reading from the prompt? */
static bool input_mode = false;

static bool status_empty = false;

/* Update status and title window. */
static bool
update_status_window(struct view *view, const char *context, const char *msg, va_list args)
{
	if (input_mode)
		return false;

	if (!status_empty || *msg) {
		wmove(status_win, 0, 0);
		if (*msg) {
			vw_printw(status_win, msg, args);
			status_empty = false;
		} else {
			status_empty = true;
		}
		wclrtoeol(status_win);

		if (context && *context) {
			size_t contextlen = strlen(context);
			int x, y, width, ___;

			getyx(status_win, y, x);
			getmaxyx(status_win, ___, width);
			(void) ___;
			if (contextlen < width - x) {
				mvwprintw(status_win, 0, width - contextlen, "%s", context);
				wmove(status_win, y, x);
			}
		}

		return true;
	}

	return false;
}

void
update_status(const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	update_status_window(display[current_view], "", msg, args);
	va_end(args);
}

void
update_status_with_context(const char *context, const char *msg, ...)
{
	va_list args;

	va_start(args, msg);
	update_status_window(display[current_view], context, msg, args);
	va_end(args);
}

void
report(const char *msg, ...)
{
	struct view *view = display[current_view];
	va_list args;

	if (!view) {
		char buf[SIZEOF_STR];
		int retval;

		FORMAT_BUFFER(buf, sizeof(buf), msg, retval, true);
		die("%s", buf);
	}

	va_start(args, msg);
	if (update_status_window(view, "", msg, args))
		wnoutrefresh(status_win);
	va_end(args);

	update_view_title(view);
}

void
report_clear(void)
{
	struct view *view = display[current_view];

	if (!view)
		return;

	if (!input_mode && !status_empty) {
		werase(status_win);
		doupdate();
	}
	status_empty = true;
	update_view_title(view);
}

static void
done_display(void)
{
	if (cursed) {
		if (status_win) {
			werase(status_win);
			doupdate();
		}
		curs_set(1);
		endwin();
	}
	cursed = false;

	if (opt_tty.attr) {
		tcsetattr(opt_tty.fd, TCSAFLUSH, opt_tty.attr);
		free(opt_tty.attr);
		opt_tty.attr = NULL;
	}
	if (opt_tty.opgrp != -1) {
		signal(SIGTTOU, SIG_IGN);
		tcsetpgrp(opt_tty.fd, opt_tty.opgrp);
		signal(SIGTTOU, SIG_DFL);
	}
}

static void
set_terminal_modes(void)
{
	nonl();		/* Disable conversion and detect newlines from input. */
	raw();		/* Take input chars one at a time, no wait for \n */
	noecho();	/* Don't echo input */
	curs_set(0);
	leaveok(stdscr, false);
}

void
init_tty(void)
{
	/* open */
	opt_tty.file = fopen("/dev/tty", "r+");
	if (!opt_tty.file)
		die("Failed to open tty for input");
	opt_tty.fd = fileno(opt_tty.file);
#ifdef HAVE_READLINE
	rl_instream = opt_tty.file;
#endif /* HAVE_READLINE */

	/* attributes */
	opt_tty.attr = calloc(1, sizeof(struct termios));
	if (!opt_tty.attr)
		die("Failed allocation for tty attributes");
	tcgetattr(opt_tty.fd, opt_tty.attr);

	if (opt_pgrp) {
		/* process-group leader */
		setpgid(getpid(), getpid());
		opt_tty.opgrp = tcgetpgrp(opt_tty.fd);
		signal(SIGTTOU, SIG_IGN);
		tcsetpgrp(opt_tty.fd, getpid());
		signal(SIGTTOU, SIG_DFL);
	}

	die_callback = done_display;
}

static void init_winch_pipe(void);

void
init_display(void)
{
	bool no_display = !!getenv("TIG_NO_DISPLAY");
	int x, y;
	int code;

	if (!opt_tty.file)
		die("Can't initialize display without tty");

	if (atexit(done_display))
		die("Failed to register done_display");

	/* Initialize the curses library */
	if (!no_display && isatty(STDIN_FILENO)) {
		/* Needed for ncurses 5.4 compatibility. */
		cursed = !!initscr();
	} else {
		/* Leave stdin and stdout alone when acting as a pager. */
		FILE *out_tty;

		out_tty = no_display ? fopen("/dev/null", "w+") : opt_tty.file;
		if (!out_tty)
			die("Failed to open tty for output");
		cursed = !!newterm(NULL, out_tty, opt_tty.file);
	}

	if (!cursed)
		die("Failed to initialize curses");

	set_terminal_modes();
	init_colors();
	/* After curses, so that its own handler is the one chained to. */
	init_winch_pipe();

	getmaxyx(stdscr, y, x);
	status_win = newwin(1, x, y - 1, 0);
	if (!status_win)
		die("Failed to create status window");

	/* Enable keyboard mapping */
	keypad(status_win, true);
	wbkgdset(status_win, get_line_attr(NULL, LINE_STATUS));
	enable_mouse(opt_mouse);

#ifdef NCURSES_VERSION
	/* Disable extended keys so that esc-codes will be received
	 * instead of extended key values (> KEY_MAX).
	 * Then these keys can be mapped in .tigrc etc. */
	for (code = KEY_MAX; code < MAX_KEYS; code++) {
		keyok(code, false);
	}
#endif

#if defined(NCURSES_VERSION_PATCH) && (NCURSES_VERSION_PATCH >= 20080119)
	set_tabsize(opt_tab_size);
#else
	TABSIZE = opt_tab_size;
#endif
}

static bool
read_script(struct key *key)
{
	static struct buffer input_buffer;
	static const char *line = "";
	enum status_code code;

	while (!line || !*line) {
		if (input_buffer.data && *input_buffer.data == ':') {
			line = "<Enter>";
			memset(&input_buffer, 0, sizeof(input_buffer));

		} else if (!io_get(&script_io, &input_buffer, '\n', true)) {
			io_done(&script_io);
			return false;
		} else if (input_buffer.data[strspn(input_buffer.data, " \t")] == '#') {
			continue;
		} else {
			line = input_buffer.data;
		}
	}

	code = get_key_value(&line, key);
	if (code != SUCCESS)
		die("Error reading script: %s", get_status_message(code));
	return true;
}

int
get_input_char(void)
{
	if (is_script_executing()) {
		static struct key key;
		static int bytes_pos;

		if (!key.modifiers.multibytes || bytes_pos >= strlen(key.data.bytes)) {
			if (!read_script(&key))
				return 0;
			bytes_pos = 0;
		}

		if (!key.modifiers.multibytes) {
			if (key.data.value < 128)
				return key.data.value;
			die("Only ASCII control characters can be used in prompts: %d", key.data.value);
		}

		return key.data.bytes[bytes_pos++];
	}

	return getc(opt_tty.file);
}

/*
 * Waiting for something to happen.
 *
 * The loop below sleeps until one of the descriptors it depends on has
 * something to say: the terminal, the pipe of every view still loading, and a
 * pipe of its own which the window-change handler writes to.  Two things
 * follow from that, and both are load-bearing.
 *
 * A descriptor left out of that set is not serviced late, it is not serviced
 * at all until something else happens to wake the loop -- a view would stop
 * part way through loading and stay there until a key was pressed.  Anything
 * which opens a descriptor this loop depends on belongs in input_wait().  The
 * cap on the sleep is what keeps such an oversight recoverable: half a second
 * of lateness rather than a stall.
 *
 * And there are now two reasons to wake, a descriptor and a deadline, so every
 * new one has to be put in the right camp.  A window change produces no bytes,
 * which is what the self-pipe is for; a periodic refresh produces no
 * descriptor, which is what the deadline is for.  Neither works as the other.
 */

/* Look in on a loading view at least this often even when its child is saying
 * nothing: it bounds the cost of a descriptor missing from the set above, and
 * keeps the "loading 5s" counter of a quiet view moving.  A loading view
 * therefore costs two wake-ups a second; an idle tig costs none. */
#define INPUT_LOADING_CAP_MS	500

/* A pipe holds 64KB and a read takes BUFSIZ, so this many drains everything
 * which can physically be waiting.  Anything beyond it arrived while we were
 * working, and the descriptor will still be ready when we look again. */
#define INPUT_DRAIN_READS	8

static int winch_pipe[2] = { -1, -1 };
static struct sigaction winch_chain;

static void
winch_handler(int signal)
{
	int saved_errno = errno;

	/* curses installed its handler first and resizing depends on it having
	 t run, so it keeps its turn; this one only wakes the loop. */
	if (!(winch_chain.sa_flags & SA_SIGINFO) &&
	    winch_chain.sa_handler != SIG_DFL &&
	    winch_chain.sa_handler != SIG_IGN &&
	    winch_chain.sa_handler != NULL)
		winch_chain.sa_handler(signal);

	if (winch_pipe[1] != -1) {
		ssize_t written = write(winch_pipe[1], "", 1);

		(void) written;
	}

	errno = saved_errno;
}

static void
init_winch_pipe(void)
{
	struct sigaction action;
	int i;

	if (pipe(winch_pipe) == -1) {
		winch_pipe[0] = winch_pipe[1] = -1;
		return;
	}

	for (i = 0; i < 2; i++) {
		int flags = fcntl(winch_pipe[i], F_GETFL);

		fcntl(winch_pipe[i], F_SETFD, FD_CLOEXEC);
		if (flags != -1)
			fcntl(winch_pipe[i], F_SETFL, flags | O_NONBLOCK);
	}

	memset(&action, 0, sizeof(action));
	action.sa_handler = winch_handler;
	sigemptyset(&action.sa_mask);
	sigaction(SIGWINCH, &action, &winch_chain);
}

/*
 * Sleep until the terminal, a loading view or a window change has something,
 * or until the deadline falls due.  Says whether the terminal was the one:
 * bytes went to curses, and if no key comes of them it is part way through an
 * escape sequence.
 */
/* How long curses gives the rest of an escape sequence to turn up. */
static int
input_escape_delay(void)
{
#ifdef NCURSES_VERSION
	return ESCDELAY > 0 ? ESCDELAY : 1000;
#else
	return 1000;
#endif
}

static bool
input_wait(int timeout_ms)
{
	struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
	struct view *view;
	fd_set fds;
	int maxfd;
	int i;

	FD_ZERO(&fds);
	FD_SET(opt_tty.fd, &fds);
	maxfd = opt_tty.fd;

	if (winch_pipe[0] != -1) {
		FD_SET(winch_pipe[0], &fds);
		maxfd = MAX(maxfd, winch_pipe[0]);
	}

	foreach_view (view, i) {
		if (!view->pipe || view->pipe->pipe == -1)
			continue;
		FD_SET(view->pipe->pipe, &fds);
		maxfd = MAX(maxfd, view->pipe->pipe);
	}

	/* A content search runs beside the views, with nothing else to wake
	 * the loop for what it finds. */
	if (csearch_fd() != -1) {
		FD_SET(csearch_fd(), &fds);
		maxfd = MAX(maxfd, csearch_fd());
	}

	/* An interrupted wait is a wait which ended: come round, look at
	 * everything again, and sleep afresh if there is still nothing. */
	if (select(maxfd + 1, &fds, NULL, NULL, timeout_ms < 0 ? NULL : &tv) < 0)
		return false;

	if (winch_pipe[0] != -1 && FD_ISSET(winch_pipe[0], &fds)) {
		char discard[64];

		while (read(winch_pipe[0], discard, sizeof(discard)) > 0)
			;
	}

	return !!FD_ISSET(opt_tty.fd, &fds);
}

static bool
update_views(void)
{
	struct view *view;
	int i;
	bool is_loading = false;

	foreach_view (view, i) {
		int reads = INPUT_DRAIN_READS;

		/* Take everything which has arrived for this view, not just
		 * the first helping of it, so that how much gets through does
		 * not ride on how often the loop comes round. */
		do {
			update_view(view);
		} while (--reads > 0 && view->pipe &&
			 io_can_read(view->pipe, false));

		if (view->pipe ||
		    (view_is_displayed(view) && view->watch.changed))
			is_loading = true;
	}

	/* Newly marked commits are scattered over the whole view, and the
	 * lines they are on say nothing of having changed: draw them all
	 * again, and let the view search find its matches anew. */
	if (csearch_update()) {
		foreach_displayed_view (view, i) {
			reset_search(view);
			redraw_view(view);
		}
	}

	if (csearch_fd() != -1)
		is_loading = true;

	return is_loading;
}

int
get_input(int prompt_position, struct key *key)
{
	struct view *view;
	int i, key_value, cursor_y, cursor_x;
	bool escape_pending = false;

	if (prompt_position > 0)
		input_mode = true;

	memset(key, 0, sizeof(*key));

	while (true) {
		int delay = -1;
		bool loading;

		if (opt_refresh_mode != REFRESH_MODE_MANUAL) {
			bool refs_refreshed = false;

			if (opt_refresh_mode == REFRESH_MODE_PERIODIC)
				delay = watch_periodic(opt_refresh_interval);

			foreach_displayed_view (view, i) {
				if (view_can_refresh(view) &&
					watch_dirty(&view->watch)) {
					if (!refs_refreshed) {
						load_refs(true);
						refs_refreshed = true;
					}
					refresh_view(view);
				}
			}
		}

		loading = update_views();
		if (loading && (delay < 0 || delay > INPUT_LOADING_CAP_MS))
			delay = INPUT_LOADING_CAP_MS;

		/* Update the cursor position. */
		if (prompt_position) {
			getbegyx(status_win, cursor_y, cursor_x);
			cursor_x = prompt_position;
		} else {
			view = display[current_view];
			getbegyx(view->win, cursor_y, cursor_x);
			cursor_x += view->width - 1;
			cursor_y += view->pos.lineno - view->pos.offset;
		}
		set_cursor_pos(cursor_y, cursor_x);

		if (is_script_executing()) {
			/* Wait for the current command to complete. */
			if (loading || !read_script(key))
				continue;
			return key->modifiers.multibytes ? OK : key->data.value;

		} else {
			/* Refresh, accept single keystroke of input */
			doupdate();

			if (escape_pending) {
				/* Bytes have gone to curses.  If they were the
				 * start of a sequence, only its own timer can
				 * decide where the sequence ends -- and it
				 * only runs that timer while it is the one
				 * waiting, so it does the waiting here.  A key
				 * already made of them comes back at once. */
				wtimeout(status_win, input_escape_delay());
				key_value = wgetch(status_win);
				escape_pending = false;
				if (key_value == ERR)
					continue;

			} else {
				/* curses reads the terminal in helpings, so a
				 * key can be waiting in its queue with nothing
				 * left on the descriptor to wake us for.  Ask
				 * it first, and only sleep once it has none. */
				nodelay(status_win, true);
				key_value = wgetch(status_win);
				nodelay(status_win, false);

				if (key_value == ERR) {
					escape_pending = input_wait(delay);
					continue;
				}
			}
		}

		/* wgetch() with nodelay() enabled returns ERR when
		 * there's no input. */
		if (key_value == ERR) {

		} else if (key_value == KEY_RESIZE) {
			int height, width;

			getmaxyx(stdscr, height, width);

			wresize(status_win, 1, width);
			mvwin(status_win, height - 1, 0);
			wnoutrefresh(status_win);
			resize_display();
			redraw_display(true);

		} else if (key_value == KEY_CTL('z')) {
			raise(SIGTSTP);

		} else {
			int pos, key_length;

			input_mode = false;
			if (key_value == erasechar())
				key_value = KEY_BACKSPACE;

			/*
			 * Ctrl-<key> values are represented using a 0x1F
			 * bitmask on the key value. To 'unmap' we assume that:
			 *
			 * - Ctrl-Z is handled separately for job control.
			 * - Ctrl-m is the same as Return/Enter.
			 * - Ctrl-i is the same as Tab.
			 * - Ctrl-[ is the same as Esc.
			 *
			 * For all other key values in the range the Ctrl flag
			 * is set and the key value is updated to the proper
			 * ASCII value.
			 */
			if (KEY_CTL('@') <= key_value && key_value <= KEY_CTL('_') &&
			    key_value != KEY_RETURN && key_value != KEY_TAB && key_value != KEY_ESC) {
				key->modifiers.control = 1;
				key_value = key_value | 0x40;
			}

			if ((key_value >= KEY_MIN && key_value < KEY_MAX) || key_value <= 0x1F) {
				key->data.value = key_value;
				return key->data.value;
			}

			key->modifiers.multibytes = 1;
			key->data.bytes[0] = key_value;

			key_length = utf8_char_length(key->data.bytes);
			/* The rest of the character is on its way, but say how
			 * long to wait rather than inheriting whichever mode
			 * the wait above left behind: a sequence cut short
			 * would otherwise hold this loop, and every view
			 * loading behind it, for good. */
			wtimeout(status_win, input_escape_delay());
			for (pos = 1; pos < key_length && pos < sizeof(key->data.bytes) - 1; pos++) {
				key->data.bytes[pos] = wgetch(status_win);
			}

			return OK;
		}
	}
}

void
enable_mouse(bool enable)
{
#ifdef NCURSES_MOUSE_VERSION
	static bool enabled = false;

	if (enable != enabled) {
		mmask_t mask = enable ? ALL_MOUSE_EVENTS : 0;

		if (mousemask(mask, NULL))
			mouseinterval(0);
		enabled = enable;
	}
#endif
}

/* vim: set ts=8 sw=8 noexpandtab: */
