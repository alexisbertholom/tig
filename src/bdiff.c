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
#include "tig/map.h"
#include "tig/parse.h"
#include "tig/repo.h"
#include "tig/util.h"
#include "tig/bdiff.h"

/*
 * Base diff.
 *
 * git-range-diff(1) does the heavy lifting of matching up the commits of the
 * two ranges, but it is not precise enough on its own: it is blind to the
 * author date, it reports positions that shift whenever a commit is added or
 * removed, and it gives up on pairing commits whose patch changed too much.
 * The pairing is therefore refined here before the commits are classified.
 */

#define BDIFF_RECORD	'\001'

struct bdiff_side {
	struct bdiff_commit **commit;
	size_t commits;
};

static struct {
	bool active;
	char rev[SIZEOF_REV];		/* Revision to compare against. */
	char base[SIZEOF_REV];		/* Commit HEAD is compared from. */
	char old_base[SIZEOF_REV];	/* Commit the revision is compared from. */
	char head[SIZEOF_REV];		/* HEAD when the history was classified. */
	const char *rev_spec;		/* What was asked for, to classify again. */
	const char *base_spec;
	const char *onto_spec;
	struct bdiff_side new_side;	/* Commits of HEAD. */
	struct bdiff_side old_side;	/* Commits of the other revision. */
} bdiff;

/* Commits injected into the main view, keyed by the commit they are
 * displayed in front of. */
struct bdiff_anchor {
	const char *id;
	struct bdiff_commit *injected;
};

DEFINE_STRING_MAP(bdiff_commits, struct bdiff_commit *, id, 128)
DEFINE_STRING_MAP(bdiff_anchors, struct bdiff_anchor *, id, 32)

DEFINE_ALLOCATOR(realloc_commits, struct bdiff_commit *, 64)

bool
bdiff_is_active(void)
{
	return bdiff.active;
}

const char *
bdiff_rev(void)
{
	return bdiff.rev;
}

const char *
bdiff_rev_spec(void)
{
	return bdiff.rev_spec;
}

const char *
bdiff_base_spec(void)
{
	return bdiff.base_spec;
}

const char *
bdiff_onto_spec(void)
{
	return bdiff.onto_spec;
}

const char *
bdiff_base(void)
{
	return bdiff.base;
}

const char *
bdiff_state_label(enum bdiff_state state)
{
	switch (state) {
	case BDIFF_ADD:		return "[add]";
	case BDIFF_DEL:		return "[del]";
	case BDIFF_MOVE:	return "[move]";
	case BDIFF_FROM:	return "[from]";
	case BDIFF_CHANGE:	return "[change]";
	case BDIFF_SAME:	return "[same]";
	case BDIFF_MOVE_CHANGE:	return "[move-change]";
	case BDIFF_CTX:		return "[ctx]";
	case BDIFF_MOVE_CTX:	return "[move-ctx]";
	default:		return NULL;
	}
}

enum line_type
bdiff_state_line_type(enum bdiff_state state)
{
	switch (state) {
	case BDIFF_ADD:		return LINE_BDIFF_ADD;
	case BDIFF_DEL:		return LINE_BDIFF_DEL;
	case BDIFF_MOVE:	return LINE_BDIFF_MOVE;
	case BDIFF_FROM:	return LINE_BDIFF_FROM;
	case BDIFF_CHANGE:	return LINE_BDIFF_CHANGE;
	case BDIFF_SAME:	return LINE_BDIFF_SAME;
	case BDIFF_MOVE_CHANGE:	return LINE_BDIFF_MOVE_CHANGE;
	case BDIFF_CTX:		return LINE_BDIFF_CTX;
	case BDIFF_MOVE_CTX:	return LINE_BDIFF_MOVE_CTX;
	default:		return LINE_DEFAULT;
	}
}

bool
bdiff_state_shows_new(enum bdiff_state state)
{
	return state == BDIFF_ADD || state == BDIFF_MOVE || state == BDIFF_SAME ||
	       state == BDIFF_CHANGE || state == BDIFF_MOVE_CHANGE ||
	       state == BDIFF_CTX || state == BDIFF_MOVE_CTX;
}

bool
bdiff_state_shows_old(enum bdiff_state state)
{
	return state == BDIFF_DEL || state == BDIFF_FROM ||
	       state == BDIFF_CHANGE || state == BDIFF_MOVE_CHANGE ||
	       state == BDIFF_CTX || state == BDIFF_MOVE_CTX;
}

bool
bdiff_state_moved(enum bdiff_state state)
{
	return state == BDIFF_MOVE || state == BDIFF_MOVE_CHANGE ||
	       state == BDIFF_MOVE_CTX;
}

bool
bdiff_state_shows_range(enum bdiff_state state)
{
	return state == BDIFF_CHANGE || state == BDIFF_MOVE_CHANGE ||
	       state == BDIFF_CTX || state == BDIFF_MOVE_CTX;
}

void
bdiff_sides(const struct bdiff_commit *commit, const char **new_id, const char **old_id)
{
	*new_id = *old_id = NULL;
	if (!commit)
		return;
	if (commit->old_side) {
		*old_id = commit->id;
		*new_id = commit->peer;
	} else {
		*new_id = commit->id;
		*old_id = commit->peer;
	}
}

static const char *
bdiff_next_parent(const char *parents, char id[SIZEOF_REV])
{
	size_t len;

	while (*parents == ' ')
		parents++;
	if (!*parents)
		return NULL;

	len = strcspn(parents, " ");
	string_ncopy_do(id, SIZEOF_REV, parents, len);

	return parents + len;
}

/*
 * Rewriting the history changes the ID of every parent, which says nothing on
 * its own; the parents only differ when they no longer are the same commits.
 */
bool
bdiff_parents_differ(const struct bdiff_commit *new_commit, const struct bdiff_commit *old_commit)
{
	const char *new_parents = new_commit->parents;
	const char *old_parents = old_commit->parents;

	while (true) {
		char new_id[SIZEOF_REV] = "";
		char old_id[SIZEOF_REV] = "";
		const struct bdiff_commit *parent;

		new_parents = bdiff_next_parent(new_parents, new_id);
		old_parents = bdiff_next_parent(old_parents, old_id);

		if (!new_parents || !old_parents)
			return !new_parents != !old_parents;

		parent = bdiff_lookup(old_id);
		if (parent && parent->peer)
			string_copy_rev(old_id, parent->peer);

		if (strcmp(new_id, old_id))
			return true;
	}
}

const struct bdiff_commit *
bdiff_lookup(const char *id)
{
	if (!bdiff.active || !id || !*id)
		return NULL;
	return string_map_get(&bdiff_commits, id);
}

struct bdiff_commit *
bdiff_injected_at(const char *id)
{
	struct bdiff_anchor *anchor;

	if (!bdiff.active || !id || !*id)
		return NULL;

	anchor = string_map_get(&bdiff_anchors, id);
	return anchor ? anchor->injected : NULL;
}

/*
 * Loading.
 */

static const char *
bdiff_strdup(const char *text)
{
	char *copy = strdup(text ? text : "");

	if (!copy)
		die("Out of memory");
	return copy;
}

static uint64_t
bdiff_hash(uint64_t hash, const char *text, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		hash ^= (unsigned char) text[i];
		hash *= 0x100000001b3ULL;
	}

	return hash;
}

static struct bdiff_commit *
bdiff_add_commit(struct bdiff_side *side, bool old_side, const char *id, const char *parents)
{
	struct bdiff_commit *commit = calloc(1, sizeof(*commit));
	struct bdiff_commit **slot;

	if (!commit || !realloc_commits(&side->commit, side->commits, 1))
		die("Out of memory");

	commit->id = bdiff_strdup(id);
	commit->parents = bdiff_strdup(parents);
	commit->old_side = old_side;
	commit->merge = !!strchr(commit->parents, ' ');
	commit->state = BDIFF_NONE;
	commit->subject = "";

	side->commit[side->commits++] = commit;

	slot = (struct bdiff_commit **) string_map_put_to(&bdiff_commits, commit->id);
	if (slot && !*slot)
		*slot = commit;

	return commit;
}

/*
 * Collect the metadata range-diff does not report: the author date, the
 * parents and the full commit message.  The output is read as a sequence of
 * records introduced by a marker line, so that multi-line messages are
 * handled without a terminator that could occur in the message itself.
 */
static enum status_code
bdiff_harvest(struct bdiff_side *side, bool old_side, const char *base, const char *tip)
{
	char range[SIZEOF_STR];
	const char *log_argv[] = {
		"git", "log", encoding_arg, "--no-color", "--reverse", "--date=raw",
			"--format=" "%x01%H %P%n%an <%ae> %ad%n%s%n%B", range, "--", NULL
	};
	struct bdiff_commit *commit = NULL;
	uint64_t hash = 0;
	int blank_lines = 0;
	int field = 0;
	struct buffer buf;
	struct io io;

	if (!string_format(range, "%s..%s", base, tip))
		return error("Failed to name the range of %s", tip);

	if (!io_run(&io, IO_RD, NULL, NULL, log_argv))
		return error("Failed to read the commits of %s", tip);

	while (io_get(&io, &buf, '\n', true)) {
		char *line = buf.data;

		if (*line == BDIFF_RECORD) {
			char *parents;

			if (commit)
				commit->message_hash = hash;

			line++;
			parents = strchr(line, ' ');
			if (parents)
				*parents++ = 0;
			else
				parents = "";

			commit = bdiff_add_commit(side, old_side, line, parents);
			hash = 0xcbf29ce484222325ULL;
			blank_lines = 0;
			field = 0;
			continue;
		}

		if (!commit)
			continue;

		switch (field++) {
		case 0:
			parse_author_line(line, &commit->author, &commit->author_time);
			break;

		case 1:
			commit->subject = bdiff_strdup(line);
			/* Fall through; the subject is part of the message. */
		default: {
			size_t len = strlen(line);

			/* Rewriting a commit reflows the whitespace at the end
			 * of its message, so leave it out of the hash: it says
			 * nothing about the message being the same one. */
			while (len > 0 && isspace((unsigned char) line[len - 1]))
				len--;

			if (!len) {
				blank_lines++;
				break;
			}

			for (; blank_lines > 0; blank_lines--)
				hash = bdiff_hash(hash, "\n", 1);

			hash = bdiff_hash(hash, line, len);
			hash = bdiff_hash(hash, "\n", 1);
			break;
		}
		}
	}

	if (commit)
		commit->message_hash = hash;

	if (io_error(&io)) {
		io_done(&io);
		return error("Failed to read the commits of %s", tip);
	}

	io_done(&io);

	return SUCCESS;
}

static struct bdiff_commit *
bdiff_find(struct bdiff_side *side, const char *id)
{
	struct bdiff_commit *commit = string_map_get(&bdiff_commits, id);
	size_t i;

	if (commit && commit->old_side == (side == &bdiff.old_side))
		return commit;

	/* Fall back to a prefix match; git may abbreviate the IDs further
	 * than requested when the hash algorithm is not SHA-1. */
	for (i = 0; i < side->commits; i++)
		if (!strncmp(side->commit[i]->id, id, strlen(id)))
			return side->commit[i];

	return NULL;
}

static char *
bdiff_skip_spaces(char *text)
{
	while (isspace((unsigned char) *text))
		text++;
	return text;
}

static char *
bdiff_parse_position(char *text, int *pos)
{
	text = bdiff_skip_spaces(text);
	*pos = 0;

	if (isdigit((unsigned char) *text)) {
		*pos = atoi(text);
		while (isdigit((unsigned char) *text))
			text++;
	} else if (*text == '-') {
		text++;
	} else {
		return NULL;
	}

	if (*text != ':')
		return NULL;

	return text + 1;
}

static char *
bdiff_parse_id(char *text, char **id)
{
	text = bdiff_skip_spaces(text);
	*id = text;

	while (*text && !isspace((unsigned char) *text))
		text++;
	if (*text)
		*text++ = 0;

	if (**id == '-')
		*id = NULL;

	return text;
}

/*
 * Parse one "1:  <id> = 2:  <id> title" line of the range-diff summary.  A
 * side which does not take part in the pair is reported as "-:  ------".
 */
static struct bdiff_commit *
bdiff_parse_pairing(char *line)
{
	struct bdiff_commit *old_commit = NULL;
	struct bdiff_commit *new_commit = NULL;
	char *old_id, *new_id;
	int old_pos, new_pos;
	char sign;

	line = bdiff_parse_position(line, &old_pos);
	if (!line)
		return NULL;
	line = bdiff_parse_id(line, &old_id);

	line = bdiff_skip_spaces(line);
	if (!*line)
		return NULL;
	sign = *line++;

	line = bdiff_parse_position(line, &new_pos);
	if (!line)
		return NULL;
	bdiff_parse_id(line, &new_id);

	if (old_id)
		old_commit = bdiff_find(&bdiff.old_side, old_id);
	if (new_id)
		new_commit = bdiff_find(&bdiff.new_side, new_id);

	if (old_commit) {
		old_commit->pos = old_pos;
		old_commit->patch_differs = sign != '=';
	}
	if (new_commit) {
		new_commit->pos = new_pos;
		new_commit->patch_differs = sign != '=';
		new_commit->range_diffed = sign == '!';
	}

	if (old_commit && new_commit) {
		old_commit->peer = new_commit->id;
		old_commit->peer_pos = new_pos;
		new_commit->peer = old_commit->id;
		new_commit->peer_pos = old_pos;
	}

	return new_commit;
}

/*
 * A line of the diff between the two patches: the outer marker tells which of
 * them has the line, the inner one what that patch does with it.  A line only
 * one patch has, doing anything else than holding context, is the patch
 * itself differing rather than what surrounds it.
 */
static bool
bdiff_pairing_line_changes_patch(const char *line)
{
	if (strlen(line) <= BDIFF_RANGE_INDENT + 1)
		return false;

	if (line[BDIFF_RANGE_INDENT] != '-' && line[BDIFF_RANGE_INDENT] != '+')
		return false;

	return line[BDIFF_RANGE_INDENT + 1] == '-' ||
	       line[BDIFF_RANGE_INDENT + 1] == '+';
}

static bool
bdiff_count_merges(const char *base, const char *tip)
{
	char range[SIZEOF_STR];
	char buf[SIZEOF_STR] = "";
	const char *argv[] = { "git", "rev-list", "--count", "--merges", range, "--", NULL };

	if (!string_format(range, "%s..%s", base, tip))
		return false;

	if (!io_run_buf(argv, buf, sizeof(buf), NULL, true))
		return false;

	return atoi(buf) > 0;
}

static enum status_code
bdiff_pair_commits(bool with_merges)
{
	char old_range[SIZEOF_STR], new_range[SIZEOF_STR];
	const char *argv[10];
	struct bdiff_commit *pair = NULL;
	struct buffer buf;
	struct io io;
	int argc = 0;

	if (!string_format(old_range, "%s..%s", bdiff.old_base, bdiff.rev) ||
	    !string_format(new_range, "%s..%s", bdiff.base, repo.head_id))
		return error("Failed to name the compared ranges");

	argv[argc++] = "git";
	argv[argc++] = "range-diff";
	argv[argc++] = "--no-color";
	argv[argc++] = "--abbrev=40";
	if (with_merges)
		argv[argc++] = "--diff-merges=remerge";
	argv[argc++] = old_range;
	argv[argc++] = new_range;
	argv[argc] = NULL;

	if (!io_run(&io, IO_RD, NULL, NULL, argv))
		return error("Failed to run git range-diff");

	while (io_get(&io, &buf, '\n', true)) {
		char *line = buf.data;

		/* The diff between the two patches of a pair is indented under
		 * the line listing it.  That line is not always in the first
		 * column: the positions it gives are aligned on the widest of
		 * them, so the shorter ones are padded. */
		if (strncmp(line, "    ", BDIFF_RANGE_INDENT))
			pair = bdiff_parse_pairing(line);
		else if (pair && bdiff_pairing_line_changes_patch(line))
			pair->patch_body_differs = true;
	}

	io_done(&io);

	if (io.status && with_merges)
		return error("git range-diff failed; comparing histories containing merge "
			     "commits needs a git version supporting '--diff-merges'");
	if (io.status)
		return error("git range-diff failed with status %d", io.status);

	return SUCCESS;
}

/*
 * range-diff gives up on commits whose patch changed too much to be
 * recognised.  Pair the leftovers on what a rewrite keeps: the author date
 * and the commit message, first as a whole and then down to its subject,
 * which two unrelated commits will not share.
 */
static bool
bdiff_rescue_matches(struct bdiff_commit *new_commit, struct bdiff_commit *old_commit,
		     bool subject_only)
{
	/* The author date is kept by a rewrite, but says little on its own:
	 * commits written in one go share it. */
	if (old_commit->author_time.sec != new_commit->author_time.sec)
		return false;

	/* Rewriting a commit often reworks the body of its message, to
	 * describe what the commit became; the subject is what stays. */
	if (subject_only)
		return *new_commit->subject &&
		       !strcmp(old_commit->subject, new_commit->subject);

	return old_commit->message_hash == new_commit->message_hash;
}

/*
 * range-diff pairs commits by how alike their patches are, which two commits
 * doing a similar small thing to the same file can be enough for.  A commit
 * whose message and author date a rewrite left untouched has a stronger claim
 * than that: hand it over when the pair range-diff settled on agrees on
 * neither, and the commit it claims is waiting on nothing else.
 */
static void
bdiff_repair_pairs(void)
{
	size_t i, j;

	for (i = 0; i < bdiff.old_side.commits; i++) {
		struct bdiff_commit *old_commit = bdiff.old_side.commit[i];
		struct bdiff_commit *paired;

		if (!old_commit->peer)
			continue;

		paired = string_map_get(&bdiff_commits, old_commit->peer);
		if (!paired || bdiff_rescue_matches(paired, old_commit, false))
			continue;

		for (j = 0; j < bdiff.new_side.commits; j++) {
			struct bdiff_commit *new_commit = bdiff.new_side.commit[j];

			if (new_commit->peer ||
			    !bdiff_rescue_matches(new_commit, old_commit, false))
				continue;

			paired->peer = NULL;
			paired->peer_pos = 0;
			paired->range_diffed = false;
			paired->patch_body_differs = false;

			old_commit->peer = new_commit->id;
			old_commit->peer_pos = new_commit->pos;
			new_commit->peer = old_commit->id;
			new_commit->peer_pos = old_commit->pos;

			/* range-diff never compared these two, so nothing says
			 * their patches are the same. */
			old_commit->patch_differs = true;
			new_commit->patch_differs = true;
			new_commit->range_diffed = false;
			break;
		}
	}
}

static void
bdiff_rescue_pairs(void)
{
	size_t i, j;
	int pass;

	/* The whole message tells two commits apart better than its subject
	 * alone, so pair on it first: a looser match must not take a commit
	 * an exact one was waiting for. */
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < bdiff.new_side.commits; i++) {
			struct bdiff_commit *new_commit = bdiff.new_side.commit[i];

			if (new_commit->peer)
				continue;

			for (j = 0; j < bdiff.old_side.commits; j++) {
				struct bdiff_commit *old_commit = bdiff.old_side.commit[j];

				if (old_commit->peer ||
				    !bdiff_rescue_matches(new_commit, old_commit, pass > 0))
					continue;

				old_commit->peer = new_commit->id;
				old_commit->peer_pos = new_commit->pos;
				old_commit->patch_differs = true;
				new_commit->peer = old_commit->id;
				new_commit->peer_pos = old_commit->pos;
				new_commit->patch_differs = true;
				break;
			}
		}
	}
}

static int
bdiff_compare_pos(const void *l, const void *r)
{
	const struct bdiff_commit *lhs = *(struct bdiff_commit * const *) l;
	const struct bdiff_commit *rhs = *(struct bdiff_commit * const *) r;

	return lhs->pos - rhs->pos;
}

/*
 * A commit has moved when it no longer keeps the same relative order as the
 * other commits that survived: comparing positions directly would report
 * every commit following an insertion or a deletion.  The commits which did
 * keep their order are the longest increasing subsequence of the positions
 * they had on the other side.
 */
static bool *
bdiff_find_kept_order(struct bdiff_commit **pairs, size_t pairs_len)
{
	bool *kept = calloc(pairs_len ? pairs_len : 1, sizeof(bool));
	size_t *tail = calloc(pairs_len ? pairs_len : 1, sizeof(size_t));
	size_t *prev = calloc(pairs_len ? pairs_len : 1, sizeof(size_t));
	size_t i, len = 0;

	if (!kept || !tail || !prev)
		die("Out of memory");

	for (i = 0; i < pairs_len; i++) {
		size_t lo = 0, hi = len;

		/* Find the first tail whose position is >= this one. */
		while (lo < hi) {
			size_t mid = lo + (hi - lo) / 2;

			if (pairs[tail[mid]]->peer_pos < pairs[i]->peer_pos)
				lo = mid + 1;
			else
				hi = mid;
		}

		prev[i] = lo > 0 ? tail[lo - 1] : pairs_len;
		tail[lo] = i;
		if (lo == len)
			len++;
	}

	if (len > 0) {
		size_t at = tail[len - 1];

		while (at != pairs_len) {
			kept[at] = true;
			at = prev[at];
		}
	}

	free(tail);
	free(prev);

	return kept;
}

static size_t
bdiff_displacement(struct bdiff_commit *commit)
{
	return commit->pos > commit->peer_pos ? commit->pos - commit->peer_pos
					      : commit->peer_pos - commit->pos;
}

/*
 * Which commits kept their order can be read in more than one way when two of
 * them were swapped: both readings leave as many commits in place, and the
 * one to report as moved is the one which ended up furthest from where it
 * was, rather than whichever the search happened to drop.
 */
static void
bdiff_prefer_least_moved(struct bdiff_commit **pairs, size_t pairs_len, bool *kept)
{
	size_t i, j;

	for (i = 0; i < pairs_len; i++) {
		size_t conflicts = 0, conflict = 0;

		if (kept[i])
			continue;

		/* The commits which kept their order read in increasing
		 * position; this one can take the place of the only one it
		 * reads out of order with, if there is just the one. */
		for (j = 0; j < pairs_len && conflicts < 2; j++) {
			if (!kept[j])
				continue;
			if (j < i ? pairs[j]->peer_pos > pairs[i]->peer_pos
				  : pairs[j]->peer_pos < pairs[i]->peer_pos) {
				conflict = j;
				conflicts++;
			}
		}

		if (conflicts == 1 &&
		    bdiff_displacement(pairs[i]) < bdiff_displacement(pairs[conflict])) {
			kept[i] = true;
			kept[conflict] = false;
		}
	}
}

static bool
bdiff_metadata_differs(struct bdiff_commit *new_commit, struct bdiff_commit *old_commit)
{
	if (new_commit->patch_differs || old_commit->patch_differs)
		return true;
	if (new_commit->author_time.sec != old_commit->author_time.sec)
		return true;
	if (ident_compare(new_commit->author, old_commit->author))
		return true;
	return false;
}

/*
 * A commit whose neighbours were rewritten around it is written again itself,
 * against the lines they left behind: what it does to the file is the same,
 * only the context it does it in has moved.  range-diff reports it as a
 * commit that differs, which says more than there is to say about it.
 */
static bool
bdiff_context_only(struct bdiff_commit *new_commit, struct bdiff_commit *old_commit)
{
	/* Commits paired on their message have no diff between their patches
	 * to tell what differs; they were paired because it differs a lot. */
	if (!new_commit->range_diffed || new_commit->patch_body_differs)
		return false;

	return new_commit->message_hash == old_commit->message_hash &&
	       new_commit->author_time.sec == old_commit->author_time.sec &&
	       !ident_compare(new_commit->author, old_commit->author);
}

static void
bdiff_classify(void)
{
	struct bdiff_commit **pairs = NULL;
	size_t pairs_len = 0;
	bool *kept;
	size_t i;

	for (i = 0; i < bdiff.new_side.commits; i++)
		if (bdiff.new_side.commit[i]->peer) {
			if (!realloc_commits(&pairs, pairs_len, 1))
				die("Out of memory");
			pairs[pairs_len++] = bdiff.new_side.commit[i];
		}

	qsort(pairs, pairs_len, sizeof(*pairs), bdiff_compare_pos);
	kept = bdiff_find_kept_order(pairs, pairs_len);
	bdiff_prefer_least_moved(pairs, pairs_len, kept);

	for (i = 0; i < pairs_len; i++) {
		struct bdiff_commit *new_commit = pairs[i];
		struct bdiff_commit *old_commit = string_map_get(&bdiff_commits, new_commit->peer);
		bool changed = old_commit && bdiff_metadata_differs(new_commit, old_commit);
		bool context = changed && bdiff_context_only(new_commit, old_commit);

		if (kept[i]) {
			new_commit->state = !changed ? BDIFF_SAME
					  : context ? BDIFF_CTX : BDIFF_CHANGE;
			if (old_commit)
				old_commit->state = new_commit->state;
		} else {
			new_commit->state = !changed ? BDIFF_MOVE
					  : context ? BDIFF_MOVE_CTX : BDIFF_MOVE_CHANGE;
			if (old_commit)
				old_commit->state = BDIFF_FROM;
		}
	}

	for (i = 0; i < bdiff.new_side.commits; i++)
		if (!bdiff.new_side.commit[i]->peer)
			bdiff.new_side.commit[i]->state = BDIFF_ADD;

	for (i = 0; i < bdiff.old_side.commits; i++)
		if (!bdiff.old_side.commit[i]->peer)
			bdiff.old_side.commit[i]->state = BDIFF_DEL;

	free(kept);
	free(pairs);
}

/*
 * Commits which are gone from HEAD have no line of their own to be drawn on,
 * so they are injected into the main view right after the nearest commit that
 * did survive, keeping the order they had.  The commit they report as their
 * parent is the next one displayed, which keeps the graph well formed.
 */
static void
bdiff_plan_injections(void)
{
	const char *anchor = bdiff.base;
	size_t i;

	for (i = 0; i < bdiff.old_side.commits; i++) {
		struct bdiff_commit *commit = bdiff.old_side.commit[i];
		struct bdiff_anchor **slot;

		if (commit->state != BDIFF_DEL && commit->state != BDIFF_FROM) {
			if (commit->peer)
				anchor = commit->peer;
			continue;
		}

		slot = (struct bdiff_anchor **) string_map_put_to(&bdiff_anchors, anchor);
		if (!slot)
			die("Out of memory");

		if (!*slot) {
			*slot = calloc(1, sizeof(**slot));
			if (!*slot)
				die("Out of memory");
			(*slot)->id = anchor;
		}

		/* Injected commits are chained in display order, that is the
		 * reverse of the order they are walked in here. */
		commit->graph_parent = (*slot)->injected ? (*slot)->injected->id : anchor;
		commit->next_injected = (*slot)->injected;
		(*slot)->injected = commit;
	}
}

static enum status_code
bdiff_resolve_commit(const char *rev, char id[SIZEOF_REV])
{
	if (!repo_rev_exists(repo_expand_rev(rev), id))
		return error("Not a valid commit: %s", rev);

	return SUCCESS;
}

static enum status_code
bdiff_merge_base(const char *rev1, const char *rev2, char id[SIZEOF_REV])
{
	const char *merge_base_argv[] = { "git", "merge-base", rev1, rev2, NULL };
	char buf[SIZEOF_STR] = "";

	if (!io_run_buf(merge_base_argv, buf, sizeof(buf), NULL, false) || !*buf)
		return error("%s and %s have no common ancestor", rev1, rev2);
	string_copy_rev(id, buf);

	return SUCCESS;
}

static enum status_code
bdiff_resolve(const char *rev, const char *base, const char *onto)
{
	enum status_code code = bdiff_resolve_commit(rev, bdiff.rev);

	if (code != SUCCESS)
		return code;

	/* The commit the two sides are compared from is theirs to pick: the
	 * best common ancestor is not always the one the history was written
	 * against. */
	if (base) {
		code = bdiff_resolve_commit(base, bdiff.base);
		string_copy_rev(bdiff.old_base, bdiff.base);
		return code;
	}

	/* A rebase moves a branch onto a newer state of the branch it is
	 * meant to be merged into, so the two sides no longer start at the
	 * same commit: measure each of them from where it left that branch,
	 * or everything the rebase pulled in shows up as added. */
	if (onto) {
		char onto_id[SIZEOF_REV];

		code = bdiff_resolve_commit(onto, onto_id);
		if (code == SUCCESS)
			code = bdiff_merge_base(onto_id, repo.head_id, bdiff.base);
		if (code == SUCCESS)
			code = bdiff_merge_base(onto_id, bdiff.rev, bdiff.old_base);
		return code;
	}

	code = bdiff_merge_base("HEAD", bdiff.rev, bdiff.base);
	string_copy_rev(bdiff.old_base, bdiff.base);

	return code;
}

/*
 * Forget the previous classification.  The commits themselves are kept: a
 * view being loaded may still hold a pointer to one of them, and the history
 * only gets classified again when it changed, so at most a handful of them
 * pile up.
 */
static void
bdiff_reset(void)
{
	string_map_clear(&bdiff_commits);
	string_map_clear(&bdiff_anchors);

	bdiff.new_side.commit = NULL;
	bdiff.new_side.commits = 0;
	bdiff.old_side.commit = NULL;
	bdiff.old_side.commits = 0;
}

static const char *
bdiff_keep(const char *spec)
{
	return spec && *spec ? bdiff_strdup(spec) : NULL;
}

enum status_code
bdiff_load(const char *rev, const char *base, const char *onto)
{
	enum status_code code;
	bool merges;

	if (!repo.head_id[0])
		return error("There is no HEAD to compare against");

	/* Comparing a rewritten branch with the version it was pushed as is
	 * the common case, so the upstream is what is compared against when
	 * no revision is given. */
	if (!rev || !*rev) {
		rev = repo_upstream_rev();

		if (!rev)
			return error("%s has no upstream branch; please name the revision to compare with",
				     repo.head[0] ? repo.head : "HEAD");
	}

	code = bdiff_resolve(rev, base, onto);
	if (code != SUCCESS)
		return code;

	bdiff_reset();

	/* Remember what was asked for, to classify the history again once it
	 * has changed under us. */
	bdiff.rev_spec = bdiff_keep(rev);
	bdiff.base_spec = bdiff_keep(base);
	bdiff.onto_spec = bdiff_keep(onto);
	string_copy_rev(bdiff.head, repo.head_id);
	bdiff.active = true;

	code = bdiff_harvest(&bdiff.old_side, true, bdiff.old_base, bdiff.rev);
	if (code == SUCCESS)
		code = bdiff_harvest(&bdiff.new_side, false, bdiff.base, repo.head_id);
	if (code != SUCCESS)
		return code;

	merges = bdiff_count_merges(bdiff.old_base, bdiff.rev) ||
		 bdiff_count_merges(bdiff.base, repo.head_id);

	/* git-range-diff refuses an empty range; with only one side left
	 * there is nothing to pair anyway. */
	if (bdiff.old_side.commits && bdiff.new_side.commits) {
		code = bdiff_pair_commits(merges);
		if (code != SUCCESS)
			return code;
		bdiff_repair_pairs();
		bdiff_rescue_pairs();
	}

	bdiff_classify();
	bdiff_plan_injections();

	return SUCCESS;
}

/*
 * Classify the history again when it changed, so that the markers keep
 * describing the commits they are drawn on.
 */
enum status_code
bdiff_refresh(void)
{
	if (!bdiff.active || !strncmp(bdiff.head, repo.head_id, SIZEOF_REV - 1))
		return SUCCESS;

	return bdiff_load(bdiff.rev_spec, bdiff.base_spec, bdiff.onto_spec);
}
