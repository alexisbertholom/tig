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
 * Which commits carry the pattern is left to git: -G reports the ones whose
 * patch adds or removes a line matching it, --grep the ones whose message
 * matches.  Given at once the two would narrow each other down, so they are
 * run one after the other and what they report is gathered in the same map.
 *
 * Only commit IDs come down the pipe, which is little enough to read on the
 * side, while the views the markers belong to are being used.  The regular
 * expression is handed over as it was typed: git matches -G and, told to,
 * --grep the way the view search does, so what marks a commit here is what
 * the search finds once its diff is open.
 */

struct csearch_commit {
	char id[SIZEOF_REV];
};

DEFINE_STRING_MAP(csearch_commits, struct csearch_commit *, id, 128)

enum csearch_pass {
	CSEARCH_IDLE,		/* Nothing is running. */
	CSEARCH_PATCH,		/* Looking for the pattern in the patches. */
	CSEARCH_MESSAGE,	/* Looking for it in the messages. */
	CSEARCH_OLD_PATCH,	/* The same, over the other side of a base diff. */
	CSEARCH_OLD_MESSAGE,
};

static struct {
	char pattern[SIZEOF_STR];
	bool active;
	enum csearch_pass pass;
	struct io io;
	size_t matched;
} csearch;

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
	return csearch.pass == CSEARCH_IDLE ? -1 : csearch.io.pipe;
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
	if (csearch.pass == CSEARCH_IDLE)
		return;

	io_kill(&csearch.io);
	io_done(&csearch.io);
	csearch.pass = CSEARCH_IDLE;
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

/* Matching without regard to case where the view search would, so that a
 * commit is marked exactly when the search finds something in it. */
static const char *
csearch_ignore_case_arg(void)
{
	if (opt_ignore_case == IGNORE_CASE_YES ||
	    (opt_ignore_case == IGNORE_CASE_SMART_CASE &&
	     !utf8_string_contains_uppercase(csearch.pattern)))
		return "--regexp-ignore-case";

	return "";
}

/*
 * Only the revisions and the paths the main view lists are looked at: a
 * marker is about a commit which is on display, and a search of the whole
 * history would be a scan the view gives no way to read.
 */
static enum status_code
csearch_run(const char *pattern_arg, const char *range)
{
	/* --extended-regexp is what -G already is, and what the view search
	 * is; it is --grep which needs telling. */
	const char *head_argv[] = {
		"git", "log", "--no-color", "--format=%H", "--extended-regexp",
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
		csearch.pass = CSEARCH_IDLE;
		return error("Failed to search the commits for '%s'", csearch.pattern);
	}

	return SUCCESS;
}

static enum status_code
csearch_next_pass(void)
{
	char pattern_arg[SIZEOF_STR];
	char old_range[SIZEOF_STR];
	const char *range = "%(revargs)";
	bool in_message;

	switch (csearch.pass) {
	case CSEARCH_IDLE:
		csearch.pass = CSEARCH_PATCH;
		break;

	case CSEARCH_PATCH:
		csearch.pass = CSEARCH_MESSAGE;
		break;

	case CSEARCH_MESSAGE:
		/* The commits a base diff injects are on the other revision,
		 * which the listed ones leave out. */
		if (!bdiff_is_active()) {
			csearch.pass = CSEARCH_IDLE;
			return SUCCESS;
		}
		csearch.pass = CSEARCH_OLD_PATCH;
		break;

	case CSEARCH_OLD_PATCH:
		csearch.pass = CSEARCH_OLD_MESSAGE;
		break;

	default:
		csearch.pass = CSEARCH_IDLE;
		return SUCCESS;
	}

	in_message = csearch.pass == CSEARCH_MESSAGE ||
		     csearch.pass == CSEARCH_OLD_MESSAGE;

	if (!string_format(pattern_arg, in_message ? "--grep=%s" : "-G%s", csearch.pattern)) {
		csearch.pass = CSEARCH_IDLE;
		return error("The pattern is too long to search for");
	}

	if (csearch.pass == CSEARCH_OLD_PATCH || csearch.pass == CSEARCH_OLD_MESSAGE) {
		if (!string_format(old_range, "%s..%s", bdiff_old_base(), bdiff_rev())) {
			csearch.pass = CSEARCH_IDLE;
			return error("Failed to name the range of %s", bdiff_rev());
		}
		range = old_range;
	}

	return csearch_run(pattern_arg, range);
}

enum status_code
csearch_start(const char *pattern)
{
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

	return csearch_next_pass();
}

bool
csearch_update(void)
{
	bool allow_read = true;
	bool changed = false;
	struct buffer buf;

	if (csearch.pass == CSEARCH_IDLE || !io_can_read(&csearch.io, false))
		return false;

	/* One helping of what has arrived, as the view loading does: a scan
	 * which stops mid-line must not hold the keyboard behind it. */
	for (; io_get_buffered(&csearch.io, &buf, '\n', allow_read); allow_read = false)
		if (csearch_add(buf.data))
			changed = true;

	if (io_error(&csearch.io)) {
		report("Failed to search the commits: %s", io_strerror(&csearch.io));
		csearch_stop();
		return true;
	}

	if (io_eof(&csearch.io)) {
		enum status_code code;

		io_done(&csearch.io);
		code = csearch_next_pass();

		if (code != SUCCESS)
			report("%s", get_status_message(code));
		else if (csearch.pass == CSEARCH_IDLE)
			report("%zu commit%s matching '%s'", csearch.matched,
			       csearch.matched == 1 ? "" : "s", csearch.pattern);
		changed = true;
	}

	return changed;
}

/* vim: set ts=8 sw=8 noexpandtab: */
