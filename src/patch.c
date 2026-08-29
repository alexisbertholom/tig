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

#include "tig/repo.h"
#include "tig/options.h"
#include "tig/parse.h"
#include "tig/view.h"
#include "tig/patch.h"

/*
 * Turn what a diff-like view has on screen back into a patch and feed it to
 * `git apply`, so that the chunk under the cursor can be staged, unstaged,
 * applied to or reverted from the working tree.
 */

static inline bool
patch_diff_done(struct line *line, struct line *end)
{
	return line >= end ||
	       line->type == LINE_DIFF_CHUNK ||
	       line->type == LINE_DIFF_HEADER;
}

static bool
patch_diff_write(struct io *io, struct line *line, struct line *end)
{
	while (line < end) {
		const char *text = box_text(line);

		if (!io_write(io, text, strlen(text)) ||
		    !io_write(io, "\n", 1))
			return false;
		line++;
		if (patch_diff_done(line, end))
			break;
	}

	return true;
}

/*
 * Write the chunk with only the changes between `first` and `last` kept: the
 * other ones are either dropped or turned into context, depending on which
 * side of the diff the patch is applied against.
 */
static bool
patch_diff_range_write(struct io *io, bool reverse,
		       struct line *line, struct line *first,
		       struct line *last, struct line *end)
{
	enum line_type write_as_normal = reverse ? LINE_DIFF_ADD : LINE_DIFF_DEL;
	enum line_type ignore = reverse ? LINE_DIFF_DEL : LINE_DIFF_ADD;

	while (line < end) {
		const char *prefix = "";
		const char *data = box_text(line);

		if (line >= first && line <= last) {
			/* Write the complete line. */

		} else if (line->type == write_as_normal) {
			prefix = " ";
			data = data + 1;

		} else if (line->type == ignore) {
			data = NULL;
		}

		if (data && !io_printf(io, "%s%s\n", prefix, data))
			return false;

		line++;
		if (patch_diff_done(line, end))
			break;
	}

	return true;
}

static bool
patch_apply_line(struct io *io, bool reverse, struct line *diff_hdr,
		 struct line *chunk, struct line *single, struct line *end)
{
	struct chunk_header header;
	int diff = single->type == LINE_DIFF_DEL ? -1 : 1;

	if (!parse_chunk_header(&header, box_text(chunk)))
		return false;

	if (reverse)
		header.old.lines = header.new.lines - diff;
	else
		header.new.lines = header.old.lines + diff;

	return patch_diff_write(io, diff_hdr, chunk) &&
	       io_printf(io, "@@ -%lu,%lu +%lu,%lu @@\n",
		       header.old.position, header.old.lines,
		       header.new.position, header.new.lines) &&
	       patch_diff_range_write(io, reverse, chunk + 1, single, single, end);
}

static bool
patch_apply_part(struct io *io, bool reverse, struct line *diff_hdr,
		 struct line *chunk, struct line *current, struct line *end)
{
	struct chunk_header header;
	struct line *first, *last, *line;
	int diff;

	if (!parse_chunk_header(&header, box_text(chunk)))
		return false;

	/* find beginning of the partial chunk */
	for (first = NULL, line = chunk; line < current; line++) {
		bool change;

		change = (line->type == LINE_DIFF_DEL || line->type == LINE_DIFF_ADD);
		if (!first && change)
			first = line;
		else if (first && !change)
			first = NULL;
	}
	if (!first)
		first = current;
	/* find the end of the partial chunk */
	last = first;
	for (line = first, diff = 0; line < end; line++)
	{
		if (line->type == LINE_DIFF_DEL) {
			last = line;
			diff--;
		}
		else if (line->type == LINE_DIFF_ADD) {
			last = line;
			diff++;
		}
		else if (line->type == LINE_DIFF_NO_NEWLINE) {
			last = line;
		}
		else
			break;
	}
	if (reverse)
		header.old.lines = header.new.lines - diff;
	else
		header.new.lines = header.old.lines + diff;

	return patch_diff_write(io, diff_hdr, chunk) &&
	       io_printf(io, "@@ -%lu,%lu +%lu,%lu @@\n",
		       header.old.position, header.old.lines,
		       header.new.position, header.new.lines) &&
	       patch_diff_range_write(io, reverse, chunk + 1, first, last, end);
}

bool
patch_apply_chunk(struct view *view, struct line *chunk, struct line *single,
		  struct patch_target target, enum patch_update update)
{
	const char *apply_argv[SIZEOF_ARG] = {
		"git", "apply", "--whitespace=nowarn", NULL
	};
	struct line *diff_hdr;
	struct io io;
	int argc = 3;

	diff_hdr = find_prev_line_by_type(view, chunk, LINE_DIFF_HEADER);
	if (!diff_hdr)
		return false;

	if (opt_diff_noprefix)
		apply_argv[argc++] = "-p0";
	if (target.cached)
		apply_argv[argc++] = "--cached";
	if (target.reverse)
		apply_argv[argc++] = "-R";
	apply_argv[argc++] = "-";
	apply_argv[argc++] = NULL;
	if (!io_run(&io, IO_WR, repo.exec_dir, NULL, apply_argv))
		return false;

	switch (update)
	{
	case PATCH_SINGLE_LINE:
		if (!patch_apply_line(&io, target.reverse, diff_hdr, chunk, single,
				      view->line + view->lines))
			chunk = NULL;
		break;
	case PATCH_PART:
		if (!patch_apply_part(&io, target.reverse, diff_hdr, chunk, single,
				      view->line + view->lines))
			chunk = NULL;
		break;
	case PATCH_CHUNK:
		if (!patch_diff_write(&io, diff_hdr, chunk) ||
		    !patch_diff_write(&io, chunk, view->line + view->lines))
			chunk = NULL;
		break;
	}

	return io_done(&io) && chunk;
}

/* The last line the chunk under the cursor covers. */
struct line *
patch_chunk_end(struct view *view, struct line *chunk)
{
	struct line *end = view->line + view->lines;
	struct line *line = chunk + 1;

	while (!patch_diff_done(line, end))
		line++;

	return line - 1;
}

/*
 * A wrapped line is drawn, and thus stored, as several lines: what they hold
 * no longer adds up to the chunk git handed out.
 */
bool
patch_chunk_is_wrapped(struct view *view, struct line *line)
{
	struct line *pos = find_prev_line_by_type(view, line, LINE_DIFF_HEADER);

	if (!opt_wrap_lines || !pos)
		return false;

	for (; pos <= line; pos++)
		if (pos->wrapped)
			return true;

	return false;
}

/* vim: set ts=8 sw=8 noexpandtab: */
