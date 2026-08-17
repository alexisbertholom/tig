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
#include "tig/display.h"
#include "tig/io.h"
#include "tig/map.h"
#include "tig/options.h"
#include "tig/string.h"
#include "tig/util.h"
#include "tig/bdiff.h"
#include "tig/csearch.h"

/*
 * Content search.
 *
 * What a commit carries is looked for the three ways a diff shows it: -G
 * reports the commits whose patch adds or removes a matching line, --grep
 * those whose message matches, and --name-only names the files each commit
 * edits, for the pattern to be tried on.  Given at once the first two would
 * narrow each other down, so all three run one after the other and what they
 * report is gathered in the same map.
 *
 * Only IDs and paths come down the pipe, which is little enough to read on the
 * side, while the views the markers belong to are being used.  The regular
 * expression is handed to git as it was typed, and compiled here the way the
 * view search compiles it, so what marks a commit is what the search finds
 * once its diff is open.
 */

#define CSEARCH_RECORD	'\001'

struct csearch_commit {
	char id[SIZEOF_REV];
};

DEFINE_STRING_MAP(csearch_commits, struct csearch_commit *, id, 128)

enum csearch_kind {
	CSEARCH_PATCH,		/* Commits whose patch matches. */
	CSEARCH_MESSAGE,	/* Commits whose message matches. */
	CSEARCH_PATHS,		/* Commits editing a file whose name matches. */
};

/* The scans making up a search, in the order they are run; the last three are
 * only for the other side of a base diff. */
static const struct csearch_scan {
	enum csearch_kind kind;
	bool old_side;
} csearch_scans[] = {
	{ CSEARCH_PATCH,	false },
	{ CSEARCH_MESSAGE,	false },
	{ CSEARCH_PATHS,	false },
	{ CSEARCH_PATCH,	true },
	{ CSEARCH_MESSAGE,	true },
	{ CSEARCH_PATHS,	true },
};

#define CSEARCH_OWN_SCANS 3
#define CSEARCH_IDLE (-1)

static struct {
	/* First, so that nothing is running before a search is: the rest of
	 * the state starts zeroed, and zero is a scan. */
	int scan;		/* Index into csearch_scans, or CSEARCH_IDLE. */
	char pattern[SIZEOF_STR];
	bool active;
	bool ignore_case;
	regex_t regex;		/* The pattern, to try on the paths. */
	bool has_regex;
	char reading[SIZEOF_REV];	/* Commit whose paths are arriving. */
	bool read_matched;	/* One of them matched already. */
	struct io io;
	size_t matched;
} csearch = { CSEARCH_IDLE };

bool
csearch_is_active(void)
{
	return csearch.active;
}

const char *
csearch_pattern(void)
{
	return csearch.pattern;
}

bool
csearch_matched(const char *id)
{
	return csearch.active && !!string_map_get(&csearch_commits, id);
}

int
csearch_fd(void)
{
	return csearch.scan == CSEARCH_IDLE ? -1 : csearch.io.pipe;
}

static bool
csearch_free_commit(void *data, void *value)
{
	free(value);
	return true;
}

static void
csearch_forget(void)
{
	string_map_foreach(&csearch_commits, csearch_free_commit, NULL);
	string_map_clear(&csearch_commits);
	csearch.matched = 0;
}

void
csearch_stop(void)
{
	if (csearch.scan == CSEARCH_IDLE)
		return;

	io_kill(&csearch.io);
	io_done(&csearch.io);
	csearch.scan = CSEARCH_IDLE;
}

static bool
csearch_add(const char *id)
{
	struct csearch_commit *commit;

	if (!*id || string_map_get(&csearch_commits, id))
		return false;

	commit = calloc(1, sizeof(*commit));
	if (!commit)
		return false;

	string_copy_rev(commit->id, id);
	if (!string_map_put(&csearch_commits, commit->id, commit)) {
		free(commit);
		return false;
	}

	csearch.matched++;
	return true;
}

/*
 * Matching without regard to case where the view search would, so that a
 * commit is marked exactly when the search finds something in it.  git is
 * told, and the pattern the paths are tried against is compiled to match.
 */
static enum status_code
csearch_compile(void)
{
	int flags = REG_EXTENDED;
	int err;

	csearch.ignore_case = opt_ignore_case == IGNORE_CASE_YES ||
			      (opt_ignore_case == IGNORE_CASE_SMART_CASE &&
			       !utf8_string_contains_uppercase(csearch.pattern));
	if (csearch.ignore_case)
		flags |= REG_ICASE;

	if (csearch.has_regex) {
		regfree(&csearch.regex);
		csearch.has_regex = false;
	}

	err = regcomp(&csearch.regex, csearch.pattern, flags);
	if (err) {
		char buf[SIZEOF_STR] = "unknown error";

		regerror(err, &csearch.regex, buf, sizeof(buf));
		regfree(&csearch.regex);
		return error("Search failed: %s", buf);
	}

	csearch.has_regex = true;
	return SUCCESS;
}

static const char *
csearch_ignore_case_arg(void)
{
	return csearch.ignore_case ? "--regexp-ignore-case" : "";
}

/*
 * Only the revisions and the paths the main view lists are looked at: a
 * marker is about a commit which is on display, and a search of the whole
 * history would be a scan the view gives no way to read.
 */
static enum status_code
csearch_run(enum csearch_kind kind, const char *pattern_arg, const char *range)
{
	bool by_path = kind == CSEARCH_PATHS;
	/* --extended-regexp is what -G already is, and what the view search
	 * is; it is --grep which needs telling. */
	const char *head_argv[] = {
		"git", "log", "--no-color", "--extended-regexp",
			by_path ? "--format=%x01%H" : "--format=%H",
			/* A rename told apart hides one of the two names the
			 * diff shows; both are wanted. */
			by_path ? "--name-only" : "", by_path ? "--no-renames" : "",
			csearch_ignore_case_arg(), NULL
	};
	const char *tail_argv[] = { range, "--", "%(fileargs)", NULL };
	const char **argv = NULL;
	const char **tail = NULL;
	int flags = 0;
	bool ok = false;
	int i;

	if (opt_file_filter)
		flags |= argv_flag_file_filter;
	if (opt_rev_filter)
		flags |= argv_flag_rev_filter;

	/* Where the pattern goes the formatting must not: a %% or a %(...) in
	 * it is the user's, and git is to be given it as it was typed. */
	if (argv_append_array(&argv, head_argv) &&
	    argv_append(&argv, pattern_arg) &&
	    argv_format(&argv_env, &tail, tail_argv, flags)) {
		for (i = 0; tail[i]; i++)
			/* An argument left empty by a filter which is off would
			 * be read as a revision of its own. */
			if (*tail[i] && !argv_append(&argv, tail[i]))
				break;
		ok = !tail[i];
	}

	if (ok)
		ok = io_run(&csearch.io, IO_RD, NULL, NULL, argv);

	argv_free(argv);
	free(argv);
	argv_free(tail);
	free(tail);

	if (!ok) {
		csearch.scan = CSEARCH_IDLE;
		return error("Failed to search the commits for '%s'", csearch.pattern);
	}

	return SUCCESS;
}

static enum status_code
csearch_next_scan(void)
{
	char pattern_arg[SIZEOF_STR] = "";
	char old_range[SIZEOF_STR];
	const char *range = "%(revargs)";
	const struct csearch_scan *scan;

	csearch.scan++;
	/* The commits a base diff injects are on the other revision, which the
	 * listed ones leave out; without one there is nothing over there. */
	if (csearch.scan >= (bdiff_is_active() ? (int) ARRAY_SIZE(csearch_scans)
						: CSEARCH_OWN_SCANS)) {
		csearch.scan = CSEARCH_IDLE;
		return SUCCESS;
	}

	scan = &csearch_scans[csearch.scan];
	csearch.reading[0] = 0;
	csearch.read_matched = false;

	/* Nothing to give git for the paths: it has no way to match a name
	 * against a regular expression, so it names them and they are tried
	 * here. */
	if (scan->kind != CSEARCH_PATHS &&
	    !string_format(pattern_arg, scan->kind == CSEARCH_MESSAGE
					? "--grep=%s" : "-G%s", csearch.pattern)) {
		csearch.scan = CSEARCH_IDLE;
		return error("The pattern is too long to search for");
	}

	if (scan->old_side) {
		if (!string_format(old_range, "%s..%s", bdiff_old_base(), bdiff_rev())) {
			csearch.scan = CSEARCH_IDLE;
			return error("Failed to name the range of %s", bdiff_rev());
		}
		range = old_range;
	}

	return csearch_run(scan->kind, pattern_arg, range);
}

enum status_code
csearch_start(const char *pattern)
{
	enum status_code code;

	csearch_stop();
	csearch_forget();

	if (!pattern || !*pattern) {
		bool was_active = csearch.active;

		csearch.active = false;
		csearch.pattern[0] = 0;
		return was_active ? success("Dropped the content search")
				  : success("No content search to drop");
	}

	string_ncopy(csearch.pattern, pattern, strlen(pattern));
	csearch.active = true;

	code = csearch_compile();
	if (code != SUCCESS) {
		csearch.active = false;
		csearch.pattern[0] = 0;
		return code;
	}

	return csearch_next_scan();
}

/*
 * A commit carries the pattern or it does not, and neither its ID nor what it
 * holds can change afterwards: what was found stays true, and only what has
 * turned up since the search is missing.  Keeping the markers over the scan
 * spares the view a round of them coming and going for nothing.
 */
enum status_code
csearch_refresh(void)
{
	enum status_code code;

	if (!csearch.active)
		return SUCCESS;

	csearch_stop();

	/* Compiled again: ignoring case is an option, and it may have been
	 * toggled since. */
	code = csearch_compile();
	if (code != SUCCESS)
		return code;

	return csearch_next_scan();
}

/*
 * A commit and the names of the files it edits: the ID arrives on a record
 * line of its own, the names follow it one per line, and the first of them to
 * match is enough to mark the commit and to leave the rest unread.
 */
static bool
csearch_read_path(const char *line)
{
	if (*line == CSEARCH_RECORD) {
		string_copy_rev(csearch.reading, line + 1);
		csearch.read_matched = false;
		return false;
	}

	if (!*line || csearch.read_matched || !*csearch.reading)
		return false;

	if (regexec(&csearch.regex, line, 0, NULL, 0))
		return false;

	csearch.read_matched = true;
	return csearch_add(csearch.reading);
}

bool
csearch_update(void)
{
	bool by_path;
	bool allow_read = true;
	bool changed = false;
	struct buffer buf;

	if (csearch.scan == CSEARCH_IDLE || !io_can_read(&csearch.io, false))
		return false;

	by_path = csearch_scans[csearch.scan].kind == CSEARCH_PATHS;

	/* One helping of what has arrived, as the view loading does: a scan
	 * which stops mid-line must not hold the keyboard behind it. */
	for (; io_get_buffered(&csearch.io, &buf, '\n', allow_read); allow_read = false)
		if (by_path ? csearch_read_path(buf.data) : csearch_add(buf.data))
			changed = true;

	if (io_error(&csearch.io)) {
		report("Failed to search the commits: %s", io_strerror(&csearch.io));
		csearch_stop();
		return true;
	}

	if (io_eof(&csearch.io)) {
		enum status_code code;

		io_done(&csearch.io);
		code = csearch_next_scan();

		if (code != SUCCESS)
			report("%s", get_status_message(code));
		else if (csearch.scan == CSEARCH_IDLE)
			report("%zu commit%s matching '%s'", csearch.matched,
			       csearch.matched == 1 ? "" : "s", csearch.pattern);
		changed = true;
	}

	return changed;
}

/* vim: set ts=8 sw=8 noexpandtab: */
