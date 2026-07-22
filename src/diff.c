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

#include "tig/argv.h"
#include "tig/refdb.h"
#include "tig/repo.h"
#include "tig/options.h"
#include "tig/display.h"
#include "tig/parse.h"
#include "tig/pager.h"
#include "tig/diff.h"
#include "tig/draw.h"
#include "tig/apps.h"

static bool diff_highlight_is_internal(void);

/* When grouping the diffstat, ask git for the full, untruncated paths (its
 * default budget elides them to ".../"): tig then re-fits them to the view
 * width itself.  Raising --stat-name-width alone has no effect, so --stat-width
 * is raised too, and the +/- graph is capped to keep the tail short. */
const char *diff_stat_width_arg(void)
{
	return opt_diff_stat_group ? "--stat-width=32767" : "";
}
const char *diff_stat_name_width_arg(void)
{
	return opt_diff_stat_group ? "--stat-name-width=32767" : "";
}
const char *diff_stat_graph_width_arg(void)
{
	return opt_diff_stat_group ? "--stat-graph-width=20" : "";
}
static void diff_refine_free(struct diff_refine **rp);
static void diff_statgrp_free(struct diff_stat_group **gp);
static void diff_stat_rows_free(struct diff_stat_rows **rp);

static enum status_code
diff_open(struct view *view, enum open_flags flags)
{
	const char *diff_argv[] = {
		"git", "show", encoding_arg, "--pretty=fuller", "--root",
			"--patch-with-stat", diff_stat_width_arg(),
			diff_stat_name_width_arg(), diff_stat_graph_width_arg(),
			use_mailmap_arg(),
			show_notes_arg(), diff_context_arg(), ignore_space_arg(),
			DIFF_ARGS, "%(cmdlineargs)", "--no-color", word_diff_arg(),
			"%(commit)", "--", "%(fileargs)", NULL
	};
	enum status_code code;

	diff_save_line(view, view->private, flags);

	code = begin_update(view, NULL, diff_argv, flags | OPEN_WITH_STDERR);
	if (code != SUCCESS)
		return code;

	return diff_init_highlight(view, view->private);
}

enum status_code
diff_init_highlight(struct view *view, struct diff_state *state)
{
	state->highlight = false;
	state->native_refine = false;
	/* Discard any buffer left over from a previous, aborted load, and the
	 * entry map of the content being replaced. */
	diff_refine_free(&state->refine);
	diff_statgrp_free(&state->stat_group);
	diff_stat_rows_free(&state->stat_rows);

	if (opt_word_diff)
		return SUCCESS;

	/* "internal" enables the built-in word refinement, so that no external
	 * diff-highlight program (and no output cleanup) is required. */
	if (diff_highlight_is_internal()) {
		state->native_refine = true;
		return SUCCESS;
	}

	if (!opt_diff_highlight || !*opt_diff_highlight)
		return SUCCESS;

	struct app_external *app = app_diff_highlight_load(opt_diff_highlight);
	struct io io;

	/* XXX This empty string keeps valgrind happy while preserving earlier
	 * behavior of test diff/diff-highlight-test:diff-highlight-misconfigured.
	 * Simpler would be to return error when user misconfigured, though we
	 * don't want tig to fail when diff-highlight isn't present.  io_exec
	 * below does not return error when app->argv[0] is empty or null as the
	 * conditional might suggest. */
	if (!*app->argv)
		app->argv[0] = "";

	if (!io_exec(&io, IO_RP, view->dir, app->env, app->argv, view->io.pipe))
		return error("Failed to run %s", opt_diff_highlight);

	state->view_io = view->io;
	view->io = io;
	state->highlight = true;

	return SUCCESS;
}

bool
diff_done_highlight(struct diff_state *state)
{
	if (!state->highlight)
		return true;
	io_kill(&state->view_io);
	return io_done(&state->view_io);
}

struct diff_stat_context {
	const char *text;
	enum line_type type;
	bool skip;
	size_t cells;
	const char **cell_text;
	struct box_cell cell[8192];
};

static bool
diff_common_add_cell(struct diff_stat_context *context, size_t length, bool allow_empty)
{
	if (!allow_empty && (length == 0))
		return true;
	if (context->cells > ARRAY_SIZE(context->cell) - 1) {
		report("Too many diff cells, truncating");
		return false;
	}
	if (context->skip && !argv_appendn(&context->cell_text, context->text, length))
		return false;
	context->cell[context->cells].length = length;
	context->cell[context->cells].type = context->type;
	context->cells++;
	return true;
}

static struct line *
diff_common_add_line(struct view *view, const char *text, enum line_type type, struct diff_stat_context *context)
{
	char *cell_text = context->cell_text ? argv_to_string_alloc(context->cell_text, "") : NULL;
	const char *line_text = cell_text ? cell_text : text;
	struct line *line = add_line_text_at(view, view->lines, line_text, type, context->cells);
	struct box *box;

	free(cell_text);
	argv_free(context->cell_text);
	free(context->cell_text);

	if (!line)
		return NULL;

	box = line->data;
	if (context->cells)
		memcpy(box->cell, context->cell, sizeof(struct box_cell) * context->cells);
	box->cells = context->cells;
	return line;
}

static bool
diff_common_add_cell_until(struct diff_stat_context *context, const char *s, enum line_type next_type)
{
	const char *sep = strstr(context->text, s);

	if (sep == NULL)
		return false;

	if (!diff_common_add_cell(context, sep - context->text, false))
		return false;

	context->text = sep + (context->skip ? strlen(s) : 0);
	context->type = next_type;

	return true;
}

static bool
diff_common_read_diff_stat_part(struct diff_stat_context *context, char c, enum line_type next_type)
{
	const char *sep = c == '|' ? strrchr(context->text, c) : strchr(context->text, c);

	if (sep == NULL)
		return false;

	diff_common_add_cell(context, sep - context->text, false);
	context->text = sep;
	context->type = next_type;

	return true;
}

static struct line *
diff_common_read_diff_stat(struct view *view, const char *text)
{
	struct diff_stat_context context = { text, LINE_DIFF_STAT };

	diff_common_read_diff_stat_part(&context, '|', LINE_DEFAULT);
	if (diff_common_read_diff_stat_part(&context, 'B', LINE_DEFAULT)) {
		/* Handle binary diffstat: Bin <deleted> -> <added> bytes */
		diff_common_read_diff_stat_part(&context, ' ', LINE_DIFF_DEL);
		diff_common_read_diff_stat_part(&context, '-', LINE_DEFAULT);
		diff_common_read_diff_stat_part(&context, ' ', LINE_DIFF_ADD);
		diff_common_read_diff_stat_part(&context, 'b', LINE_DEFAULT);

	} else {
		diff_common_read_diff_stat_part(&context, '+', LINE_DIFF_ADD);
		diff_common_read_diff_stat_part(&context, '-', LINE_DIFF_DEL);
	}
	diff_common_add_cell(&context, strlen(context.text), false);

	return diff_common_add_line(view, text, LINE_DIFF_STAT, &context);
}

/* Detect a diff stat line:
 *
 *	added                    |   40 +++++++++++
 *	remove                   |  124 --------------------------
 *	updated                  |   14 +----
 *	rename.from => rename.to |    0
 *	.../truncated file name  |   11 ++---
 *	binary add               |  Bin 0 -> 1234 bytes
 *	binary update            |  Bin 1234 -> 2345 bytes
 *	binary copy              |  Bin
 *	unmerged                 | Unmerged
 */
static bool
diff_stat_is_entry(const char *text)
{
	const char *data = text + strspn(text, " ");
	size_t len = strlen(data);
	const char *pipe = strchr(data, '|');

	/* Ensure that '|' is present and the file name part contains
	 * non-space characters. */
	if (!pipe || pipe == data)
		return false;

	return (data[len - 1] == '-' || data[len - 1] == '+') ||
	       strstr(pipe, " 0") || strstr(pipe, "Bin") || strstr(pipe, "Unmerged") ||
	       (data[len - 1] == '0' && (strstr(data, "=>") || !prefixcmp(data, "...")));
}

struct line *
diff_common_add_diff_stat(struct view *view, const char *text, size_t offset)
{
	if (!diff_stat_is_entry(text + offset))
		return NULL;
	return diff_common_read_diff_stat(view, text);
}

static bool
diff_common_read_diff_wdiff_group(struct diff_stat_context *context)
{
	const char *sep_add = strstr(context->text, "{+");
	const char *sep_del = strstr(context->text, "[-");
	const char *sep;
	enum line_type next_type;
	const char *end_delimiter;
	const char *end_sep;
	size_t len;

	if (sep_add == NULL && sep_del == NULL)
		return false;

	if (sep_del == NULL || (sep_add != NULL && sep_add < sep_del)) {
		sep = sep_add;
		next_type = LINE_DIFF_ADD;
		end_delimiter = "+}";
	} else {
		sep = sep_del;
		next_type = LINE_DIFF_DEL;
		end_delimiter = "-]";
	}

	diff_common_add_cell(context, sep - context->text, false);

	context->type = next_type;
	context->text = sep;

	// workaround for a single }/] change
	end_sep = strstr(context->text + sizeof("{+") - 1, end_delimiter);

	if (end_sep == NULL) {
		// diff is not terminated
		len = strlen(context->text);
	} else {
		// separators are included in the add/del highlight
		len = end_sep - context->text + sizeof("+}") - 1;
	}

	diff_common_add_cell(context, len, false);

	if (end_sep == NULL) {
		context->text += len;
	} else {
		context->text = end_sep + sizeof("+}") - 1;
	}
	context->type = LINE_DEFAULT;

	return true;
}

static bool
diff_common_read_diff_wdiff(struct view *view, const char *text)
{
	struct diff_stat_context context = { text, LINE_DEFAULT };

	/* Detect remaining part of a word diff line:
	 *
	 *	added {+new +} text
	 *	removed[- something -] from the file
	 *	replaced [-some-]{+same+} text
	 *	there could be [-one-] diff part{+s+} in the {+any +} line
	 */
	while (diff_common_read_diff_wdiff_group(&context))
		;

	diff_common_add_cell(&context, strlen(context.text), true);
	return diff_common_add_line(view, text, LINE_DEFAULT, &context);
}

static bool
diff_common_highlight(struct view *view, const char *text, enum line_type type)
{
	struct diff_stat_context context = { text, type, true };
	enum line_type hi_type = type == LINE_DIFF_ADD
				 ? LINE_DIFF_ADD_HIGHLIGHT : LINE_DIFF_DEL_HIGHLIGHT;
	const char *codes[] = { "\x1b[7m", "\x1b[27m" };
	const enum line_type types[] = { hi_type, type };
	int i;

	for (i = 0; diff_common_add_cell_until(&context, codes[i], types[i]); i = (i + 1) % 2)
		;

	diff_common_add_cell(&context, strlen(context.text), true);
	return diff_common_add_line(view, text, type, &context);
}

/*
 * Native intra-line diff refinement.
 *
 * Reimplements the word-level highlighting otherwise delegated to an external
 * "diff-highlight" program (e.g. diffr): each maximal block of removed (-) and
 * added (+) lines is buffered, a longest common subsequence of their words is
 * computed, and the words that are *not* shared are emitted as
 * LINE_DIFF_*_HIGHLIGHT cells.  No external process, and thus no output
 * sanitizing, is needed.
 *
 * Enabled with "set diff-highlight = internal".
 */

/* Skip refinement when the token product would be too large, to bound the cost
 * of the O(n*m) longest common subsequence on pathological blocks. */
#define REFINE_MAX_PRODUCT 500000

struct refine_line {
	char *text;			/* owned copy of the stored diff line */
	enum line_type type;
	bool added;
	unsigned int prefix;		/* marker + indent, never highlighted */
	int tok_first, tok_count;	/* range into the removed/added tokens */
};

struct refine_token {
	int line;			/* index into refine->line[] */
	unsigned int lo, hi;		/* byte range within that line's text */
	bool shared;			/* part of the common subsequence */
};

struct diff_refine {
	struct refine_line *line;
	size_t lines, lines_alloc;
	struct refine_token *removed, *added;
	size_t nremoved, removed_alloc;
	size_t nadded, added_alloc;
};

static bool
diff_highlight_is_internal(void)
{
	return opt_diff_highlight && !strcmp(opt_diff_highlight, "internal");
}

/* diffr's classification: a word is a run of alphanumerics/'_' (and, kept
 * whole, any UTF-8 continuation), spaces are runs of ' '/'\t', and every other
 * byte is its own token. */
static int
refine_char_kind(unsigned char c)
{
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	    (c >= '0' && c <= '9') || c == '_' || c >= 0x80)
		return 1;
	if (c == ' ' || c == '\t')
		return 2;
	return 0;
}

static unsigned int
refine_prefix_len(const char *s)
{
	unsigned int i = 0;

	if (s[0] == '+' || s[0] == '-' || s[0] == ' ')
		i = 1;
	while (s[i] == ' ' || s[i] == '\t')
		i++;
	return i;
}

static bool
refine_token_push(struct refine_token **arr, size_t *n, size_t *alloc,
		  int line, size_t lo, size_t hi)
{
	if (*n == *alloc) {
		size_t na = *alloc ? *alloc * 2 : 256;
		struct refine_token *p = realloc(*arr, na * sizeof(*p));

		if (!p)
			return false;
		*arr = p;
		*alloc = na;
	}
	(*arr)[*n].line = line;
	(*arr)[*n].lo = lo;
	(*arr)[*n].hi = hi;
	(*arr)[*n].shared = false;
	(*n)++;
	return true;
}

static bool
refine_tokenize(struct diff_refine *r, int line_idx)
{
	struct refine_line *ln = &r->line[line_idx];
	const char *s = ln->text;
	size_t len = strlen(s);
	size_t i = ln->prefix;
	struct refine_token **arr = ln->added ? &r->added : &r->removed;
	size_t *n = ln->added ? &r->nadded : &r->nremoved;
	size_t *alloc = ln->added ? &r->added_alloc : &r->removed_alloc;

	ln->tok_first = (int) *n;
	while (i < len) {
		int kind = refine_char_kind((unsigned char) s[i]);
		size_t lo = i;

		i++;
		if (kind != 0)
			while (i < len &&
			       refine_char_kind((unsigned char) s[i]) == kind)
				i++;
		if (!refine_token_push(arr, n, alloc, line_idx, lo, i))
			return false;
	}
	ln->tok_count = (int) *n - ln->tok_first;
	return true;
}

static bool
refine_push_line(struct diff_refine *r, const char *text, enum line_type type,
		 bool added)
{
	struct refine_line *ln;
	char *copy = strdup(text);

	if (!copy)
		return false;
	if (r->lines == r->lines_alloc) {
		size_t na = r->lines_alloc ? r->lines_alloc * 2 : 32;
		struct refine_line *p = realloc(r->line, na * sizeof(*p));

		if (!p) {
			free(copy);
			return false;
		}
		r->line = p;
		r->lines_alloc = na;
	}
	ln = &r->line[r->lines++];
	ln->text = copy;
	ln->type = type;
	ln->added = added;
	ln->prefix = refine_prefix_len(copy);
	ln->tok_first = ln->tok_count = 0;
	return refine_tokenize(r, (int) (r->lines - 1));
}

static bool
refine_token_eq(struct diff_refine *r, struct refine_token *a,
		struct refine_token *b)
{
	size_t la = a->hi - a->lo;

	if (la != b->hi - b->lo)
		return false;
	return memcmp(r->line[a->line].text + a->lo,
		      r->line[b->line].text + b->lo, la) == 0;
}

/* Mark the longest common subsequence of removed/added tokens as shared, via a
 * standard suffix-table dynamic program.  Returns false only on allocation
 * failure, in which case the block is emitted without refinement. */
static bool
refine_lcs(struct diff_refine *r)
{
	size_t n = r->nremoved, m = r->nadded, stride = m + 1;
	size_t i, j;
	int *l = calloc((n + 1) * stride, sizeof(int));

	if (!l)
		return false;

	for (i = n; i-- > 0; ) {
		for (j = m; j-- > 0; ) {
			if (refine_token_eq(r, &r->removed[i], &r->added[j])) {
				l[i * stride + j] = l[(i + 1) * stride + j + 1] + 1;
			} else {
				int down = l[(i + 1) * stride + j];
				int right = l[i * stride + j + 1];

				l[i * stride + j] = down >= right ? down : right;
			}
		}
	}

	for (i = 0, j = 0; i < n && j < m; ) {
		if (refine_token_eq(r, &r->removed[i], &r->added[j])) {
			r->removed[i].shared = r->added[j].shared = true;
			i++;
			j++;
		} else if (l[(i + 1) * stride + j] >= l[i * stride + j + 1]) {
			i++;
		} else {
			j++;
		}
	}

	free(l);
	return true;
}

static bool
refine_emit_plain(struct view *view, struct refine_line *ln)
{
	return pager_common_read(view, ln->text, ln->type, NULL);
}

static bool
refine_emit_line(struct view *view, struct diff_refine *r,
		 struct refine_line *ln)
{
	struct diff_stat_context ctx = { ln->text, ln->type };
	enum line_type hi = (ln->type == LINE_DIFF_ADD || ln->type == LINE_DIFF_ADD2)
			    ? LINE_DIFF_ADD_HIGHLIGHT : LINE_DIFF_DEL_HIGHLIGHT;
	struct refine_token *toks = ln->added ? r->added : r->removed;
	bool ok = true;
	int t;

	if (ln->prefix)
		ok = diff_common_add_cell(&ctx, ln->prefix, false);

	for (t = 0; ok && t < ln->tok_count; ) {
		struct refine_token *tok = &toks[ln->tok_first + t];
		bool shared = tok->shared;
		size_t end = tok->hi;
		int t2 = t + 1;

		while (t2 < ln->tok_count &&
		       toks[ln->tok_first + t2].shared == shared) {
			end = toks[ln->tok_first + t2].hi;
			t2++;
		}
		ctx.type = shared ? ln->type : hi;
		ok = diff_common_add_cell(&ctx, end - tok->lo, false);
		t = t2;
	}

	/* Fall back to a plain line if the cell budget was exhausted. */
	if (!ok)
		return refine_emit_plain(view, ln);

	return diff_common_add_line(view, ln->text, ln->type, &ctx) != NULL;
}

static void
diff_refine_reset(struct diff_refine *r)
{
	size_t i;

	for (i = 0; i < r->lines; i++)
		free(r->line[i].text);
	r->lines = r->nremoved = r->nadded = 0;
}

static void
diff_refine_free(struct diff_refine **rp)
{
	struct diff_refine *r = *rp;

	if (!r)
		return;
	diff_refine_reset(r);
	free(r->line);
	free(r->removed);
	free(r->added);
	free(r);
	*rp = NULL;
}

static bool
diff_refine_flush(struct view *view, struct diff_state *state)
{
	struct diff_refine *r = state->refine;
	bool refine, ok = true;
	size_t k;

	if (!r || r->lines == 0)
		return true;

	/* Highlight as long as there is content.  When one side is empty (a pure
	 * insertion or deletion) every token is new, so the whole run is
	 * highlighted, as diffr does.  The common subsequence is only needed when
	 * both sides exist, and is skipped above the product guard to bound the
	 * cost of the O(n*m) dynamic program on pathological blocks. */
	refine = r->nremoved > 0 || r->nadded > 0;
	if (r->nremoved > 0 && r->nadded > 0) {
		if ((unsigned long long) r->nremoved * r->nadded > REFINE_MAX_PRODUCT)
			refine = false;
		else if (!refine_lcs(r))
			refine = false;
	}

	for (k = 0; ok && k < r->lines; k++)
		ok = refine ? refine_emit_line(view, r, &r->line[k])
			    : refine_emit_plain(view, &r->line[k]);

	diff_refine_reset(r);
	return ok;
}

static bool
diff_refine_is_content(struct diff_state *state, enum line_type type)
{
	if (!state->native_refine || state->combined_diff ||
	    !state->reading_diff_chunk)
		return false;
	return type == LINE_DIFF_ADD || type == LINE_DIFF_DEL;
}

static bool
diff_refine_push(struct diff_state *state, const char *data, enum line_type type)
{
	if (!state->refine) {
		state->refine = calloc(1, sizeof(*state->refine));
		if (!state->refine)
			return false;
	}
	return refine_push_line(state->refine, data, type, type == LINE_DIFF_ADD);
}

/*
 * Diffstat entry map.
 *
 * A diffstat entry is resolved to its file diff by position: the Nth entry
 * stands for the Nth file of the diff.  That only holds while the entries are
 * listed in git's order, which grouping the paths is free to give up.  So
 * remember the path each entry stands for as it is emitted, and look the file
 * diff up by name instead of by rank.  Entries whose path git did not spell out
 * in full -- renames and paths it truncated -- are left out of the map and keep
 * being resolved by position.
 */

struct diff_stat_row {
	unsigned long lineno;		/* the view line holding the entry */
	const char *path;		/* interned, never freed; NULL for a directory */
	unsigned int depth;		/* how deep in the tree the row sits */
};

struct diff_stat_rows {
	struct diff_stat_row *row;
	size_t rows, rows_alloc;
};

static void
diff_stat_rows_free(struct diff_stat_rows **rp)
{
	struct diff_stat_rows *r = *rp;

	if (!r)
		return;
	free(r->row);
	free(r);
	*rp = NULL;
}

/* Entries are pushed as they are emitted, keeping `row` sorted by line. */
static bool
diff_stat_row_push(struct diff_state *state, unsigned long lineno,
		   const char *path, unsigned int depth)
{
	struct diff_stat_rows *r = state->stat_rows;
	struct diff_stat_row *row;

	if (!r) {
		r = state->stat_rows = calloc(1, sizeof(*r));
		if (!r)
			return false;
	}
	if (r->rows == r->rows_alloc) {
		size_t na = r->rows_alloc ? r->rows_alloc * 2 : 16;
		struct diff_stat_row *p = realloc(r->row, na * sizeof(*p));

		if (!p)
			return false;
		r->row = p;
		r->rows_alloc = na;
	}

	row = &r->row[r->rows++];
	row->lineno = lineno;
	row->path = path;
	row->depth = depth;
	return true;
}

/* The map entry of a diffstat row, or NULL when it has none.  Every view
 * reading diff content -- diff, stage and pager -- heads its private data with
 * a struct diff_state, so the map is reachable from all of them. */
static const struct diff_stat_row *
diff_stat_row_find(struct view *view, struct line *line)
{
	struct diff_state *state = view->private;
	struct diff_stat_rows *r = state ? state->stat_rows : NULL;
	unsigned long lineno = line - view->line;
	size_t lo = 0, hi;

	if (!r)
		return NULL;

	for (hi = r->rows; lo < hi; ) {
		size_t mid = lo + (hi - lo) / 2;

		if (r->row[mid].lineno == lineno)
			return &r->row[mid];
		if (r->row[mid].lineno < lineno)
			lo = mid + 1;
		else
			hi = mid;
	}
	return NULL;
}

/* Does `line` sit below the directory row `header` in the tree? */
bool
diff_stat_row_under(struct view *view, struct line *header, struct line *line)
{
	const struct diff_stat_row *h = diff_stat_row_find(view, header);
	const struct diff_stat_row *r = diff_stat_row_find(view, line);

	return h && r && r->depth > h->depth;
}

/*
 * Diffstat path tree.
 *
 * The paths in a diffstat are mostly made of directories shared with the file
 * above, which pushes the interesting part (the file name and the graph) far
 * to the right.  With "set diff-stat-group = yes", the whole stat block is
 * buffered and laid out as a tree, so that a directory is spelled out once:
 *
 *	 src/
 *	 ├── components/
 *	 │   ├── App.tsx        | 2 +
 *	 │   └── AppTopBar.tsx  | 8 ++
 *	 └── store/reducers.ts  | 4 +-
 *
 * A directory holding a single entry is folded into it ("store/reducers.ts"):
 * one row less to read, and one level less of indent for everything below.
 * The fold stops where the row would no longer fit the view, so a narrow view
 * trades width for depth.  Renames keep the "{old => new}" form git prints,
 * which counts as one component, so a directory moved wholesale is one node.
 *
 * The directory rows use a line type of their own, skipped when navigating,
 * and every row is mapped to the path it stands for, so that jumping to a file
 * diff does not depend on the entries keeping git's order.
 */

/* Columns a level of the tree is indented by, glyph included. */
#define STATGRP_STEP 4

enum statgrp_glyph {
	STATGRP_BRANCH,		/* an entry, with more to follow */
	STATGRP_LAST,		/* the last entry of its directory */
	STATGRP_PIPE,		/* lead-in past a directory with more to come */
	STATGRP_BLANK,		/* lead-in past the last directory */
	STATGRP_GLYPHS
};

static const char *statgrp_utf8[STATGRP_GLYPHS] = { "├── ", "└── ", "│   ", "    " };
static const char *statgrp_ascii[STATGRP_GLYPHS] = { "|-- ", "`-- ", "|   ", "    " };

struct statgrp_file {
	char *text;		/* original stat line */
	char *path;		/* file name (before '|'), trimmed */
	size_t pathlen;
	const char *rest;	/* "| <graph>" tail, points into text */
};

struct statgrp_node {
	char *name;			/* component, directories keep their '/' */
	size_t namelen;
	int fidx;			/* the entry it stands for, or -1 */
	struct statgrp_node **kid;
	size_t kids, kids_alloc;
};

struct diff_stat_group {
	struct statgrp_file *file;
	size_t files, files_alloc;
};

static bool
diff_statgrp_push(struct diff_state *state, const char *text)
{
	struct diff_stat_group *g = state->stat_group;
	struct statgrp_file *f;
	char *copy, *pipe, *start, *end;

	if (!g) {
		g = state->stat_group = calloc(1, sizeof(*g));
		if (!g)
			return false;
	}
	if (g->files == g->files_alloc) {
		size_t na = g->files_alloc ? g->files_alloc * 2 : 16;
		struct statgrp_file *p = realloc(g->file, na * sizeof(*p));

		if (!p)
			return false;
		g->file = p;
		g->files_alloc = na;
	}

	copy = strdup(text);
	if (!copy)
		return false;
	pipe = strrchr(copy, '|');
	if (!pipe) {			/* guarded by diff_stat_is_entry, defensive */
		free(copy);
		return false;
	}

	f = &g->file[g->files++];
	memset(f, 0, sizeof(*f));
	f->text = copy;
	f->rest = pipe;

	start = copy + strspn(copy, " ");
	end = pipe;
	while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
		end--;
	f->pathlen = end - start;
	f->path = malloc(f->pathlen + 1);
	if (!f->path)
		return false;
	memcpy(f->path, start, f->pathlen);
	f->path[f->pathlen] = 0;
	return true;
}

/* Did git spell the path out, or did it truncate it ("...")?  A rename is
 * spelled out, but as both of its paths at once. */
static bool
statgrp_full_path(const struct statgrp_file *f)
{
	return prefixcmp(f->path, "...") != 0;
}

/*
 * The path a rename entry ends up at.  git renders a rename as
 * "pfx{old => new}sfx", eliding what the two paths share, or as "old => new"
 * when they share nothing; either way the new path is the one the file diff
 * is headed by.  Returns NULL when the entry is not a rename.
 */
static char *
statgrp_renamed_path(const char *path)
{
	const char *arrow = strstr(path, " => ");
	const char *open = strchr(path, '{');
	const char *close = open ? strchr(open, '}') : NULL;
	size_t pfx, new, sfx;
	char *out;

	if (!arrow)
		return NULL;
	if (!close || arrow < open || arrow > close)
		return strdup(arrow + STRING_SIZE(" => "));

	pfx = open - path;
	arrow += STRING_SIZE(" => ");
	new = close - arrow;
	sfx = strlen(close + 1);

	out = malloc(pfx + new + sfx + 1);
	if (!out)
		return NULL;
	memcpy(out, path, pfx);
	memcpy(out + pfx, arrow, new);
	memcpy(out + pfx + new, close + 1, sfx + 1);
	return out;
}

/* The leading path component of `path`, its '/' included.  A rename stands as
 * one component of its own, even when the paths it holds have slashes. */
static size_t
statgrp_component(const char *path)
{
	size_t i;

	for (i = 0; path[i]; i++) {
		if (path[i] == '{') {
			const char *close = strchr(path + i, '}');

			if (!close)
				break;		/* unbalanced, take the rest */
			i = close - path;
			continue;
		}
		if (path[i] == '/')
			return i + 1;
	}
	return strlen(path);
}

static struct statgrp_node *
statgrp_kid(struct statgrp_node *node, const char *name, size_t namelen, int fidx)
{
	struct statgrp_node *n;
	size_t i;

	/* Directories are shared, entries are not: two files never merge. */
	if (fidx < 0)
		for (i = 0; i < node->kids; i++)
			if (node->kid[i]->fidx < 0 &&
			    node->kid[i]->namelen == namelen &&
			    !memcmp(node->kid[i]->name, name, namelen))
				return node->kid[i];

	if (node->kids == node->kids_alloc) {
		size_t na = node->kids_alloc ? node->kids_alloc * 2 : 4;
		struct statgrp_node **p = realloc(node->kid, na * sizeof(*p));

		if (!p)
			return NULL;
		node->kid = p;
		node->kids_alloc = na;
	}
	n = calloc(1, sizeof(*n));
	if (!n)
		return NULL;
	n->name = malloc(namelen + 1);
	if (!n->name) {
		free(n);
		return NULL;
	}
	memcpy(n->name, name, namelen);
	n->name[namelen] = 0;
	n->namelen = namelen;
	n->fidx = fidx;
	node->kid[node->kids++] = n;
	return n;
}

static bool
statgrp_insert(struct statgrp_node *root, const char *path, int fidx)
{
	struct statgrp_node *node = root;

	while (*path) {
		size_t len = statgrp_component(path);
		bool leaf = path[len - 1] != '/';

		node = statgrp_kid(node, path, len, leaf ? fidx : -1);
		if (!node)
			return false;
		path += len;
	}
	return true;
}

/*
 * Fold a directory holding a single entry into that entry, as long as the row
 * it makes fits: `names` is what a file row has left for its name, the graph
 * column taken out, and `width` all a directory row can use.
 */
static void
statgrp_fold(struct statgrp_node *node, size_t indent, size_t names, size_t width)
{
	size_t i;

	while (node->fidx < 0 && node->kids == 1) {
		struct statgrp_node *kid = node->kid[0];
		size_t len = node->namelen + kid->namelen;
		char *name;

		if (indent + len > (kid->fidx >= 0 ? names : width))
			break;

		name = realloc(node->name, len + 1);
		if (!name)
			break;
		memcpy(name + node->namelen, kid->name, kid->namelen + 1);
		node->name = name;
		node->namelen = len;
		node->fidx = kid->fidx;

		free(node->kid);
		node->kid = kid->kid;
		node->kids = kid->kids;
		node->kids_alloc = kid->kids_alloc;
		free(kid->name);
		free(kid);
	}

	for (i = 0; i < node->kids; i++)
		statgrp_fold(node->kid[i], indent + STATGRP_STEP, names, width);
}

/* The columns the widest file row takes up, indent and name. */
static size_t
statgrp_name_width(struct statgrp_node *node, size_t depth)
{
	size_t i, max = node->fidx >= 0 ? depth * STATGRP_STEP + node->namelen : 0;

	for (i = 0; i < node->kids; i++) {
		size_t w = statgrp_name_width(node->kid[i], depth + 1);

		if (w > max)
			max = w;
	}
	return max;
}

struct statgrp_draw {
	struct view *view;
	struct diff_state *state;
	struct statgrp_file *file;
	const char **glyph;
	char *pfx;			/* lead-in of the level being drawn */
	size_t pfxlen, pfxcap;
	char *line;
	size_t linecap;
	size_t names;			/* width of the name column */
};

static bool
statgrp_pfx_push(struct statgrp_draw *d, const char *seg)
{
	size_t len = strlen(seg);

	if (d->pfxlen + len + 1 > d->pfxcap) {
		size_t na = (d->pfxlen + len + 1) * 2;
		char *p = realloc(d->pfx, na);

		if (!p)
			return false;
		d->pfx = p;
		d->pfxcap = na;
	}
	memcpy(d->pfx + d->pfxlen, seg, len + 1);
	d->pfxlen += len;
	return true;
}

static bool
statgrp_draw_node(struct statgrp_draw *d, struct statgrp_node *node,
		  size_t depth, bool last)
{
	const char *glyph = depth ? d->glyph[last ? STATGRP_LAST : STATGRP_BRANCH] : "";
	size_t cols = depth * STATGRP_STEP + node->namelen;
	size_t saved = d->pfxlen;
	struct line *row;
	const char *path = NULL;
	char *renamed = NULL;
	size_t i;

	if (node->fidx >= 0) {
		struct statgrp_file *f = &d->file[node->fidx];

		snprintf(d->line, d->linecap, " %s%s%s%*s %s",
			 d->pfx, glyph, node->name,
			 (int) (d->names > cols ? d->names - cols : 0), "", f->rest);
		row = diff_common_read_diff_stat(d->view, d->line);

		if (statgrp_full_path(f)) {
			renamed = statgrp_renamed_path(f->path);
			path = get_path(renamed ? renamed : f->path);
			free(renamed);
		}
	} else {
		snprintf(d->line, d->linecap, " %s%s%s", d->pfx, glyph, node->name);
		row = add_line_text(d->view, d->line, LINE_DIFF_STAT_HEADER);
	}

	if (!row || !diff_stat_row_push(d->state, row - d->view->line, path, depth))
		return false;

	if (depth && !statgrp_pfx_push(d, d->glyph[last ? STATGRP_BLANK : STATGRP_PIPE]))
		return false;
	for (i = 0; i < node->kids; i++)
		if (!statgrp_draw_node(d, node->kid[i], depth + 1, i + 1 == node->kids))
			return false;
	d->pfx[d->pfxlen = saved] = 0;

	return true;
}

static void
statgrp_free_node(struct statgrp_node *node)
{
	size_t i;

	if (!node)
		return;
	for (i = 0; i < node->kids; i++)
		statgrp_free_node(node->kid[i]);
	free(node->kid);
	free(node->name);
	free(node);
}

static void
diff_statgrp_free(struct diff_stat_group **gp)
{
	struct diff_stat_group *g = *gp;
	size_t i;

	if (!g)
		return;
	for (i = 0; i < g->files; i++) {
		free(g->file[i].text);
		free(g->file[i].path);
	}
	free(g->file);
	free(g);
	*gp = NULL;
}

/* The tree is drawn with characters of the line itself, where the curses
 * graphics of "line-graphics = default" have no place: fall back on UTF-8 when
 * the locale allows it, as "auto" does, and on ASCII otherwise. */
static const char **
statgrp_glyphs(void)
{
	if (opt_line_graphics == GRAPHIC_ASCII)
		return statgrp_ascii;
	if (opt_line_graphics == GRAPHIC_UTF_8 || locale_is_utf8())
		return statgrp_utf8;
	return statgrp_ascii;
}

static bool
diff_statgrp_flush(struct view *view, struct diff_state *state)
{
	struct diff_stat_group *g = state->stat_group;
	struct statgrp_node *root = NULL;
	struct statgrp_draw draw = { view, state, NULL, statgrp_glyphs() };
	size_t i, maxrest = 0, maxpath = 0, width, names;
	bool ok = false;

	if (!g || g->files == 0) {
		diff_statgrp_free(&state->stat_group);
		return true;
	}

	root = calloc(1, sizeof(*root));
	if (!root)
		goto out;
	root->fidx = -1;
	root->name = strdup("");
	if (!root->name)
		goto out;

	for (i = 0; i < g->files; i++) {
		struct statgrp_file *f = &g->file[i];

		if (strlen(f->rest) > maxrest)
			maxrest = strlen(f->rest);
		if (f->pathlen > maxpath)
			maxpath = f->pathlen;
		if (!statgrp_insert(root, f->path, (int) i))
			goto out;
	}

	/* What a file row has left for its name once the graph column is in. */
	width = view->width > 0 ? (size_t) view->width : 80;
	names = width > maxrest + 2 ? width - maxrest - 2 : 1;

	/* The entries git listed at the root head a tree of their own, drawn
	 * from the first column, so each of them starts the walk over. */
	for (i = 0; i < root->kids; i++) {
		size_t w;

		statgrp_fold(root->kid[i], 0, names, width);
		w = statgrp_name_width(root->kid[i], 0);
		if (w > draw.names)
			draw.names = w;
	}
	if (draw.names > names)
		draw.names = names;

	/* A row is the lead-in (at most 6 bytes a level, and a level takes at
	 * least two characters of the path), the name, the padding to the name
	 * column, and the graph. */
	draw.file = g->file;
	draw.linecap = draw.names + maxpath * 6 + maxrest + 32;
	draw.line = malloc(draw.linecap);
	draw.pfx = calloc(1, 1);	/* the empty lead-in of the top level */
	draw.pfxcap = 1;
	if (!draw.line || !draw.pfx)
		goto out;

	/* The tree is walked in the order the entries came in, so the rows stay
	 * in git's order but for a rename reaching across a directory. */
	for (i = 0; i < root->kids; i++)
		if (!statgrp_draw_node(&draw, root->kid[i], 0, i + 1 == root->kids))
			goto out;
	ok = true;

out:
	free(draw.line);
	free(draw.pfx);
	statgrp_free_node(root);
	diff_statgrp_free(&state->stat_group);
	return ok;
}

bool
diff_common_read(struct view *view, const char *data, struct diff_state *state)
{
	enum line_type type = get_line_type(data);

	/* ADD2 and DEL2 are only valid in combined diff hunks */
	if (!state->combined_diff && (type == LINE_DIFF_ADD2 || type == LINE_DIFF_DEL2))
		type = LINE_DEFAULT;

	/* DEL_FILE, ADD_FILE and START are only valid outside diff chunks */
	if (state->reading_diff_chunk) {
		if (type == LINE_DIFF_DEL_FILE || type == LINE_DIFF_START)
			type = LINE_DIFF_DEL;
		else if (type == LINE_DIFF_ADD_FILE)
			type = LINE_DIFF_ADD;
	}

	/* Emit any buffered refinement block when leaving the run of +/- lines. */
	if (state->native_refine && !diff_refine_is_content(state, type) &&
	    !diff_refine_flush(view, state))
		return false;

	if (!view->lines && type != LINE_COMMIT)
		state->reading_diff_stat = true;

	/* combined diffs lack LINE_DIFF_START and we don't know
	 * if this is a combined diff until we see a "@@@" */
	if (!state->after_diff && data[0] == ' ' && data[1] != ' ')
		state->reading_diff_stat = true;

	if (state->reading_diff_stat) {
		if (opt_diff_stat_group && diff_stat_is_entry(data)) {
			if (!diff_statgrp_push(state, data))
				return false;
			return true;
		}
		if (diff_common_add_diff_stat(view, data, 0))
			return true;
		state->reading_diff_stat = false;
		/* The buffered stat block ends here: group and emit it. */
		if (state->stat_group && !diff_statgrp_flush(view, state))
			return false;

	} else if (type == LINE_DIFF_START) {
		state->reading_diff_stat = true;
	}

	if (!state->after_commit_title && !prefixcmp(data, "    ")) {
		struct line *line = add_line_text(view, data, LINE_DEFAULT);

		if (line)
			line->commit_title = 1;
		state->after_commit_title = true;
		return line != NULL;
	}

	if (type == LINE_DIFF_HEADER) {
		state->after_diff = true;
		state->reading_diff_chunk = false;

	} else if (type == LINE_DIFF_CHUNK) {
		const unsigned int len = chunk_header_marker_length(data);
		const char *context = strstr(data + len, "@@");
		struct line *line =
			context ? add_line_text_at(view, view->lines, data, LINE_DIFF_CHUNK, len)
				: NULL;
		struct box *box;

		if (!line)
			return false;

		box = line->data;
		box->cell[0].length = (context + len) - data;
		box->cell[1].length = strlen(context + len);
		box->cell[box->cells++].type = LINE_DIFF_STAT;
		state->combined_diff = (len > 2);
		state->parents = len - 1;
		state->reading_diff_chunk = true;
		return true;

	} else if (type == LINE_COMMIT) {
		state->reading_diff_chunk = false;

	}

	if (opt_word_diff && state->reading_diff_chunk &&
	    /* combined diff format is not using word diff */
	    !state->combined_diff)
		return diff_common_read_diff_wdiff(view, data);

	if (!opt_diff_indicator && state->reading_diff_chunk &&
	    !state->stage)
		data += state->parents;

	if (diff_refine_is_content(state, type))
		return diff_refine_push(state, data, type);

	if (state->highlight && strchr(data, 0x1b))
		return diff_common_highlight(view, data, type);

	return pager_common_read(view, data, type, NULL);
}

static bool
diff_find_stat_entry(struct view *view, struct line *line, enum line_type type)
{
	struct line *marker = find_next_line_by_type(view, line, type);

	return marker &&
		line == find_prev_line_by_type(view, marker, LINE_DIFF_HEADER);
}

/* The file diff of `path`, among the headers a diffstat entry can stand for. */
static struct line *
diff_find_header_by_pathname(struct view *view, const char *path)
{
	struct line *line;

	for (line = view->line; view_has_line(view, line); line++) {
		const char *file;

		line = find_next_line_by_type(view, line, LINE_DIFF_HEADER);
		if (!line)
			break;

		if (!diff_find_stat_entry(view, line, LINE_DIFF_INDEX)
		    && !diff_find_stat_entry(view, line, LINE_DIFF_OLDMODE)
		    && !diff_find_stat_entry(view, line, LINE_DIFF_SIMILARITY))
			continue;

		file = diff_get_pathname(view, line, false);
		if (file && !strcmp(file, path))
			return line;
	}

	return NULL;
}

static struct line *
diff_find_header_from_stat(struct view *view, struct line *line)
{
	if (line->type == LINE_DIFF_STAT) {
		const struct diff_stat_row *row = diff_stat_row_find(view, line);
		int file_number = 0;

		if (row && row->path) {
			struct line *header = diff_find_header_by_pathname(view, row->path);

			if (header)
				return header;
			/* An unmapped or unmatched entry falls back on its
			 * position, which the entries in git's order keep. */
		}

		/* Count the stat entries above, skipping the group headers and the
		 * blank separators so that grouping does not shift the numbering. */
		while (view_has_line(view, line) &&
		       (line->type == LINE_DIFF_STAT ||
			line->type == LINE_DIFF_STAT_HEADER)) {
			if (line->type == LINE_DIFF_STAT)
				file_number++;
			line--;
		}

		for (line = view->line; view_has_line(view, line); line++) {
			line = find_next_line_by_type(view, line, LINE_DIFF_HEADER);
			if (!line)
				break;

			if (diff_find_stat_entry(view, line, LINE_DIFF_INDEX)
			    || diff_find_stat_entry(view, line, LINE_DIFF_OLDMODE)
			    || diff_find_stat_entry(view, line, LINE_DIFF_SIMILARITY)) {
				if (file_number == 1) {
					break;
				}
				file_number--;
			}
		}

		return line;
	}

	return NULL;
}

/* Resolve a diffstat entry (LINE_DIFF_STAT) to the file it stands for. */
const char *
diff_stat_pathname(struct view *view, struct line *line, bool old)
{
	struct line *header = diff_find_header_from_stat(view, line);

	return header ? diff_get_pathname(view, header, old) : NULL;
}

enum request
diff_common_enter(struct view *view, enum request request, struct line *line)
{
	if (line->type == LINE_DIFF_STAT) {
		line = diff_find_header_from_stat(view, line);
		if (!line) {
			report("Failed to find file diff");
			return REQ_NONE;
		}

		select_view_line(view, line - view->line);
		report_clear();
		return REQ_NONE;

	} else {
		return pager_request(view, request, line);
	}
}

void
diff_save_line(struct view *view, struct diff_state *state, enum open_flags flags)
{
	if (flags & OPEN_RELOAD) {
		struct line *line = &view->line[view->pos.lineno];
		const char *file = view_has_line(view, line) ? diff_get_pathname(view, line, false) : NULL;

		if (file) {
			state->file = get_path(file);
			state->lineno = diff_get_lineno(view, line, false);
			state->pos = view->pos;
		}
	}
}

void
diff_restore_line(struct view *view, struct diff_state *state)
{
	struct line *line = &view->line[view->lines - 1];

	if (!state->file)
		return;

	while ((line = find_prev_line_by_type(view, line, LINE_DIFF_HEADER))) {
		const char *file = diff_get_pathname(view, line, false);

		if (file && !strcmp(file, state->file))
			break;
		line--;
	}

	state->file = NULL;

	if (!line)
		return;

	while ((line = find_next_line_by_type(view, line, LINE_DIFF_CHUNK))) {
		unsigned int lineno = diff_get_lineno(view, line, false);

		for (line++; view_has_line(view, line) && line->type != LINE_DIFF_CHUNK; line++) {
			if (lineno == state->lineno) {
				unsigned long lineno = line - view->line;
				unsigned long offset = lineno - (state->pos.lineno - state->pos.offset);

				goto_view_line(view, offset, lineno);
				redraw_view(view);
				return;
			}
			if (line->type != LINE_DIFF_DEL &&
			    line->type != LINE_DIFF_DEL2)
				lineno++;
		}
	}
}

static bool
diff_read_describe(struct view *view, struct buffer *buffer, struct diff_state *state)
{
	struct line *line = find_next_line_by_type(view, view->line, LINE_PP_REFS);

	if (line && buffer) {
		const char *ref = string_trim(buffer->data);
		const char *sep = !strcmp("Refs: ", box_text(line)) ? "" : ", ";

		if (*ref && !append_line_format(view, line, "%s%s", sep, ref))
			return false;
	}

	return true;
}

static bool
diff_read(struct view *view, struct buffer *buf, bool force_stop)
{
	struct diff_state *state = view->private;

	if (state->adding_describe_ref)
		return diff_read_describe(view, buf, state);

	if (!buf) {
		if (state->native_refine) {
			if (!diff_refine_flush(view, state))
				return false;
			diff_refine_free(&state->refine);
		}
		/* Flush a stat block that was not terminated by a summary line. */
		if (state->stat_group && !diff_statgrp_flush(view, state))
			return false;

		if (!diff_done_highlight(state)) {
			if (!force_stop)
				report("Failed to run the diff-highlight program: %s", opt_diff_highlight);
			return false;
		}

		/* Fall back to retry if no diff will be shown. */
		if (view->lines == 0 && opt_file_args) {
			int pos = argv_size(view->argv)
				- argv_size(opt_file_args) - 1;

			if (pos > 0 && !strcmp(view->argv[pos], "--")) {
				for (; view->argv[pos]; pos++) {
					free((void *) view->argv[pos]);
					view->argv[pos] = NULL;
				}

				if (view->pipe)
					io_done(view->pipe);
				if (view_exec(view, 0))
					return false;
			}
		}

		if (view->env->blame_lineno) {
			state->file = get_path(view->env->file);
			state->lineno = view->env->blame_lineno;
			state->pos.offset = 0;
			state->pos.lineno = view->lines - 1;

			view->env->blame_lineno = 0;
		}

		diff_restore_line(view, state);

		if (!state->adding_describe_ref && !ref_list_contains_tag(view->vid)) {
			const char *describe_argv[] = { "git", "describe", "--tags", view->vid, NULL };
			enum status_code code = begin_update(view, NULL, describe_argv, OPEN_EXTRA);

			if (code != SUCCESS) {
				report("Failed to load describe data: %s", get_status_message(code));
				return true;
			}

			state->adding_describe_ref = true;
			return false;
		}

		return true;
	}

	return diff_common_read(view, buf->data, state);
}

static bool
diff_blame_line(const char *ref, const char *file, unsigned long lineno,
		struct blame_header *header, struct blame_commit *commit)
{
	char author[SIZEOF_STR] = "";
	char committer[SIZEOF_STR] = "";
	char line_arg[SIZEOF_STR];
	const char *blame_argv[] = {
		"git", "blame", encoding_arg, "-p", line_arg, ref, "--", file, NULL
	};
	struct io io;
	bool ok = false;
	struct buffer buf;

	if (!string_format(line_arg, "-L%lu,+1", lineno))
		return false;

	if (!io_run(&io, IO_RD, repo.exec_dir, NULL, blame_argv))
		return false;

	while (io_get(&io, &buf, '\n', true)) {
		if (header) {
			if (!parse_blame_header(header, buf.data))
				break;
			header = NULL;

		} else if (parse_blame_info(commit, author, committer, buf.data)) {
			ok = commit->filename != NULL;
			break;
		}
	}

	if (io_error(&io))
		ok = false;

	io_done(&io);
	return ok;
}

unsigned int
diff_get_lineno(struct view *view, struct line *line, bool old)
{
	const struct line *header, *chunk;
	unsigned int lineno;
	struct chunk_header chunk_header;

	/* Verify that we are after a diff header and one of its chunks */
	header = find_prev_line_by_type(view, line, LINE_DIFF_HEADER);
	chunk = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK);
	if (!header || !chunk || chunk < header)
		return 0;

	/*
	 * In a chunk header, the number after the '+' sign is the number of its
	 * following line, in the new version of the file. We increment this
	 * number for each non-deletion line, until the given line position.
	 */
	if (!parse_chunk_header(&chunk_header, box_text(chunk)))
		return 0;

	lineno = old ? chunk_header.old.position : chunk_header.new.position;

	for (chunk++; chunk < line; chunk++)
		if (old ? chunk->type != LINE_DIFF_ADD && chunk->type != LINE_DIFF_ADD2
			: chunk->type != LINE_DIFF_DEL && chunk->type != LINE_DIFF_DEL2)
			lineno++;

	return lineno;
}

static enum request
diff_trace_origin(struct view *view, enum request request, struct line *line)
{
	struct line *commit_line = find_prev_line_by_type(view, line, LINE_COMMIT);
	char id[SIZEOF_REV];
	struct line *diff = find_prev_line_by_type(view, line, LINE_DIFF_HEADER);
	struct line *chunk = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK);
	const char *chunk_data;
	int chunk_marker = line->type == LINE_DIFF_DEL ? '-' : '+';
	unsigned long lineno = 0;
	const char *file = NULL;
	char ref[SIZEOF_REF];
	struct blame_header header;
	struct blame_commit commit;

	if (!diff || !chunk || chunk == line || diff < commit_line) {
		report("The line to trace must be inside a diff chunk");
		return REQ_NONE;
	}

	file = diff_get_pathname(view, line, line->type == LINE_DIFF_DEL);

	if (!file) {
		report("Failed to read the file name");
		return REQ_NONE;
	}

	chunk_data = box_text(chunk);

	if (!parse_chunk_lineno(&lineno, chunk_data, chunk_marker)) {
		report("Failed to read the line number");
		return REQ_NONE;
	}

	if (lineno == 0) {
		report("This is the origin of the line");
		return REQ_NONE;
	}

	for (chunk += 1; chunk < line; chunk++) {
		if (chunk->type == LINE_DIFF_ADD) {
			lineno += chunk_marker == '+';
		} else if (chunk->type == LINE_DIFF_DEL) {
			lineno += chunk_marker == '-';
		} else {
			lineno++;
		}
	}

	if (commit_line)
		string_copy_rev_from_commit_line(id, box_text(commit_line));
	else
		string_copy(id, view->vid);

	if (chunk_marker == '-')
		string_format(ref, "%s^", id);
	else
		string_copy(ref, id);

	if (!diff_blame_line(ref, file, lineno, &header, &commit)) {
		report("Failed to read blame data");
		return REQ_NONE;
	}

	string_ncopy(view->env->file, commit.filename, strlen(commit.filename));
	string_copy_rev(request == REQ_VIEW_BLAME ? view->env->ref : view->env->commit, header.id);
	view->env->goto_lineno = header.orig_lineno - 1;

	return request;
}

const char *
diff_get_pathname(struct view *view, struct line *line, bool old)
{
	struct line *header;
	struct line *file;

	header = find_prev_line_in_commit_by_type(view, line, LINE_DIFF_HEADER);
	if (!header)
		return NULL;

	if (!old) {
		const char *dst;
		const char *prefixes[] = { "diff --cc ", "diff --combined " };
		int i;

		for (i = 0; i < ARRAY_SIZE(prefixes); i++) {
			dst = strstr(box_text(header), prefixes[i]);
			if (dst)
				return dst + strlen(prefixes[i]);
		}
	}

	file = find_next_line_in_diff_by_type(view, header + 1, old ? LINE_DIFF_DEL_FILE : LINE_DIFF_ADD_FILE);
	if (file) {
		const char *name;

		name = box_text(file);
		if (old ? !prefixcmp(name, "--- ") : !prefixcmp(name, "+++ "))
			name += STRING_SIZE("+++ ");

		if (opt_diff_noprefix)
			return name;

		/* Handle mnemonic prefixes, such as "b/" and "w/". */
		if (!prefixcmp(name, "a/") || !prefixcmp(name, "b/") || !prefixcmp(name, "i/") || !prefixcmp(name, "w/"))
			name += STRING_SIZE("a/");
		return name;
	}

	file = find_next_line_in_diff_by_type(view, header + 1, old ? LINE_DIFF_RENAME_FROM : LINE_DIFF_RENAME_TO);
	if (file) {
		const char *name;

		name = box_text(file);
		name += old ? STRING_SIZE("rename from ") : STRING_SIZE("rename to ");
		return name;
	}

	return NULL;
}

enum request
diff_common_edit(struct view *view, enum request request, struct line *line)
{
	const char *file;
	char path[SIZEOF_STR];
	unsigned int lineno;

	if (line->type == LINE_DIFF_STAT) {
		file = view->env->file;
		lineno = view->env->lineno;
	} else {
		file = diff_get_pathname(view, line, false);
		lineno = diff_get_lineno(view, line, false);
	}

	if (!file || !*file) {
		report("Nothing to edit");
		return REQ_NONE;
	}

	if (!string_concat_path(path, repo.cdup, file) || access(path, R_OK)) {
		report("Failed to open file: %s", file);
		return REQ_NONE;
	}

	open_editor(file, lineno);
	return REQ_NONE;
}

static enum request
diff_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_VIEW_BLAME:
	case REQ_VIEW_BLOB:
		if (line->type == LINE_DIFF_STAT) {
			string_copy_rev(request == REQ_VIEW_BLAME ? view->env->ref : view->env->commit, view->vid);
			view->env->goto_lineno = 0;
			return request;
		}
		return diff_trace_origin(view, request, line);

	case REQ_EDIT:
		return diff_common_edit(view, request, line);

	case REQ_ENTER:
		return diff_common_enter(view, request, line);

	case REQ_REFRESH:
		reload_view(view);
		return REQ_NONE;

	default:
		return pager_request(view, request, line);
	}
}

void
diff_common_select(struct view *view, struct line *line, const char *changes_msg)
{
	if (line->type == LINE_DIFF_STAT) {
		struct line *header = diff_find_header_from_stat(view, line);
		if (header) {
			const char *file = diff_get_pathname(view, header, false);

			if (file) {
				const char *old_file = diff_get_pathname(view, header, true);
				if (old_file)
					string_format(view->env->file_old, "%s", old_file);
				else
					view->env->file_old[0] = '\0';
				string_format(view->env->file, "%s", file);
				view->env->lineno = view->env->goto_lineno = 0;
				view->env->blob[0] = 0;
			}
		}

		string_format(view->ref, "Press '%s' to jump to file diff",
			      get_view_key(view, REQ_ENTER));
	} else {
		const char *file = diff_get_pathname(view, line, false);

		if (file) {
			const char *old_file = diff_get_pathname(view, line, true);
			if (old_file)
				string_format(view->env->file_old, "%s", old_file);
			else
				view->env->file_old[0] = '\0';
			if (changes_msg)
				string_format(view->ref, "%s to '%s'", changes_msg, file);
			string_format(view->env->file, "%s", file);
			view->env->lineno = view->env->goto_lineno = diff_get_lineno(view, line, false);
			if (view->env->goto_lineno > 0)
				view->env->goto_lineno--;
			view->env->lineno_old = diff_get_lineno(view, line, true);
			view->env->blob[0] = 0;
		} else {
			view->env->lineno = view->env->goto_lineno = (line - view->line) + 1;
			string_ncopy(view->ref, view->ops->id, strlen(view->ops->id));
		}
	}
	pager_select(view, line);
}

static void
diff_select(struct view *view, struct line *line)
{
	diff_common_select(view, line, "Changes");
}

static struct view_ops diff_ops = {
	"line",
	argv_env.commit,
	VIEW_DIFF_LIKE | VIEW_ADD_DESCRIBE_REF | VIEW_ADD_PAGER_REFS | VIEW_FILE_FILTER | VIEW_REFRESH | VIEW_FLEX_WIDTH,
	sizeof(struct diff_state),
	diff_open,
	diff_read,
	view_column_draw,
	diff_request,
	view_column_grep,
	diff_select,
	NULL,
	view_column_bit(LINE_NUMBER) | view_column_bit(TEXT),
	pager_get_column_data,
};

DEFINE_VIEW(diff);

/* vim: set ts=8 sw=8 noexpandtab: */
