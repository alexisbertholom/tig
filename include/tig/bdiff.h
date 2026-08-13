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

#ifndef TIG_BDIFF_H
#define TIG_BDIFF_H

#include "tig/tig.h"
#include "tig/line.h"
#include "tig/util.h"

/*
 * Base diff: compare HEAD to another revision commit by commit, starting
 * from their common ancestor, and classify every commit of both sides.
 */

/* The body of a range diff is indented, to leave room for the marker telling
 * which of the two patches each of its lines belongs to. */
#define BDIFF_RANGE_INDENT 4

enum bdiff_state {
	BDIFF_NONE = 0,		/* Not part of the compared range. */
	BDIFF_ADD,		/* Only exists on HEAD. */
	BDIFF_DEL,		/* Only exists on the other revision. */
	BDIFF_MOVE,		/* Reordered, but otherwise unchanged. */
	BDIFF_FROM,		/* Where a moved commit used to be. */
	BDIFF_CHANGE,		/* Kept its position, but was modified. */
	BDIFF_SAME,		/* Unchanged, save for its parent. */
	BDIFF_MOVE_CHANGE,	/* Both reordered and modified. */
	BDIFF_CTX,		/* Only the context around it changed. */
	BDIFF_MOVE_CTX,		/* Reordered, and only its context changed. */
};

struct bdiff_commit {
	const char *id;		/* Commit ID on its own side. */
	const char *peer;	/* Matching commit on the other side, if any. */
	const char *parents;	/* Parent IDs, as reported by git log. */
	const char *subject;	/* First line of the commit message. */
	const struct ident *author;
	struct time author_time;
	uint64_t message_hash;	/* Hash of the whole commit message. */
	int pos;		/* 1-based position within its own side. */
	int peer_pos;		/* Position of the matching commit. */
	enum bdiff_state state;
	bool old_side;		/* Belongs to the other revision, not HEAD. */
	bool merge;
	bool patch_differs;	/* The two sides do not have the same patch. */
	bool range_diffed;	/* range-diff reported how the two sides differ. */
	bool patch_body_differs;/* What differs is more than the context. */

	/* Commits injected into the main view are chained together in the
	 * order they are displayed; the last one points at the commit it is
	 * anchored to. */
	struct bdiff_commit *next_injected;
	const char *graph_parent;
};

bool bdiff_is_active(void);
enum status_code bdiff_load(const char *rev, const char *base, const char *onto);
enum status_code bdiff_refresh(void);

const struct bdiff_commit *bdiff_lookup(const char *id);
bool bdiff_parents_differ(const struct bdiff_commit *new_commit, const struct bdiff_commit *old_commit);
struct bdiff_commit *bdiff_injected_at(const char *id);

const char *bdiff_state_label(enum bdiff_state state);
enum line_type bdiff_state_line_type(enum bdiff_state state);
bool bdiff_state_shows_new(enum bdiff_state state);
bool bdiff_state_shows_old(enum bdiff_state state);
bool bdiff_state_shows_range(enum bdiff_state state);
bool bdiff_state_moved(enum bdiff_state state);

/* The commit IDs of the two sides of a classified commit; either may be
 * NULL when the commit only exists on one side. */
void bdiff_sides(const struct bdiff_commit *commit, const char **new_id, const char **old_id);

const char *bdiff_rev(void);
const char *bdiff_rev_spec(void);
const char *bdiff_base_spec(void);
const char *bdiff_onto_spec(void);
const char *bdiff_base(void);

#endif
/* vim: set ts=8 sw=8 noexpandtab: */
