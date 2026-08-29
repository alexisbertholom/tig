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

#ifndef TIG_PATCH_H
#define TIG_PATCH_H

#include "tig/tig.h"
#include "tig/view.h"

/* How much of the chunk under the cursor the patch is made of. */
enum patch_update {
	PATCH_CHUNK,
	PATCH_SINGLE_LINE,
	PATCH_PART,
};

/* Where the patch goes, and which way round it is applied. */
struct patch_target {
	bool reverse;			/* Undo the change instead of making it. */
	bool cached;			/* Patch the index, not the working tree. */
};

bool patch_apply_chunk(struct view *view, struct line *chunk, struct line *single,
		       struct patch_target target, enum patch_update update);
bool patch_chunk_is_wrapped(struct view *view, struct line *line);
struct line *patch_chunk_end(struct view *view, struct line *chunk);

#endif
/* vim: set ts=8 sw=8 noexpandtab: */
