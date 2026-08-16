/* Copyright (c) 2006-2026 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 */

#ifndef TIG_CSEARCH_H
#define TIG_CSEARCH_H

#include "tig/tig.h"
#include "tig/util.h"

/*
 * Content search: which of the listed commits carry a pattern, be it in their
 * patch or in their message.
 */

/* What a matching commit is prefixed with. */
#define CSEARCH_MARKER "[match]"

bool csearch_is_active(void);
const char *csearch_pattern(void);
bool csearch_matched(const char *id);

/* Look for a pattern, dropping what a previous one found; an empty pattern
 * only drops it. */
enum status_code csearch_start(const char *pattern);
/* Look again for the pattern in force, keeping what it already found. */
enum status_code csearch_refresh(void);
/* Give up on what is left to scan, keeping what was found so far. */
void csearch_stop(void);

/* The descriptor the scan reads from, or -1 when nothing is running. */
int csearch_fd(void);
/* Take what the scan has to give; says whether the display is now stale. */
bool csearch_update(void);

#endif
/* vim: set ts=8 sw=8 noexpandtab: */
