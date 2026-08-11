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

#ifndef TIG_DIFF_H
#define TIG_DIFF_H

#include "tig/view.h"
#include "tig/bdiff.h"

struct diff_refine;
struct diff_stat_group;
struct diff_stat_rows;

/*
 * A --bdiff commit is shown as a sequence of sections: what changed besides
 * the patch, the diff between the two patches, and the patch of each side.
 */
enum bdiff_section_kind {
	BDIFF_SECTION_META,
	BDIFF_SECTION_RANGE,
	BDIFF_SECTION_NEW,
	BDIFF_SECTION_OLD,
};

struct bdiff_section {
	enum bdiff_section_kind kind;
	unsigned long start;		/* First line of the section. */
	const char *id;			/* Commit the section shows. */
};

#define BDIFF_SECTIONS 4

struct diff_state {
	bool after_commit_title;
	bool after_diff;
	bool reading_diff_chunk;
	bool reading_diff_stat;
	bool combined_diff;
	bool adding_describe_ref;
	bool highlight;
	bool native_refine;
	bool stage;
	unsigned int parents;
	const char *file;
	unsigned int lineno;
	struct position pos;
	struct io view_io;
	struct diff_refine *refine;
	struct diff_stat_group *stat_group;
	struct diff_stat_rows *stat_rows;

	/* Base diff state. */
	const struct bdiff_commit *bdiff;
	struct bdiff_section bdiff_section[BDIFF_SECTIONS];
	int bdiff_sections;		/* Sections added so far. */
	int bdiff_planned;		/* Sections still to be loaded. */
	enum bdiff_section_kind bdiff_plan[BDIFF_SECTIONS];
	int bdiff_next;			/* Next section of the plan. */
	int bdiff_range_inner;		/* Inner marker of the buffered block. */
	char bdiff_new_id[SIZEOF_REV];
	char bdiff_old_id[SIZEOF_REV];
	char bdiff_new_arg[SIZEOF_REV + 2];
	char bdiff_old_arg[SIZEOF_REV + 2];
};

enum request diff_common_edit(struct view *view, enum request request, struct line *line);
bool diff_common_read(struct view *view, const char *data, struct diff_state *state);
enum request diff_common_enter(struct view *view, enum request request, struct line *line);
struct line *diff_common_add_diff_stat(struct view *view, const char *text, size_t offset);
void diff_common_select(struct view *view, struct line *line, const char *changes_msg);
void diff_save_line(struct view *view, struct diff_state *state, enum open_flags flags);
void diff_restore_line(struct view *view, struct diff_state *state);
enum status_code diff_init_highlight(struct view *view, struct diff_state *state);
bool diff_done_highlight(struct view *view, struct diff_state *state);

unsigned int diff_get_lineno(struct view *view, struct line *line, bool old);
const char *diff_get_pathname(struct view *view, struct line *line, bool old);
const char *diff_stat_pathname(struct view *view, struct line *line, bool old);
bool diff_stat_row_under(struct view *view, struct line *header, struct line *line);

const char *diff_stat_width_arg(void);
const char *diff_stat_name_width_arg(void);
const char *diff_stat_graph_width_arg(void);

extern struct view diff_view;

static inline void
open_diff_view(struct view *prev, enum open_flags flags)
{
	open_view(prev, &diff_view, flags);
}

#endif
/* vim: set ts=8 sw=8 noexpandtab: */
