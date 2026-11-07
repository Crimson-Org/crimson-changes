#pragma once

#include <glib.h>

#include "git/diff.h"

/* A span of bytes within one line's text */
typedef struct {
	int start, end;
} ByteRange;

/* The words that changed between a removed line and the added line that replaced it, as ByteRange items. Lines
 * too different to compare word by word, or too long, give no ranges, so the whole line reads as changed */
gboolean find_changed_words(const char *old_text, const char *new_text, GArray *old_ranges, GArray *new_ranges);

/* Pairs each removed line with the added line that replaced it, in hunk order, as indexes into the hunk's lines,
 * with -1 for a line without a partner */
GArray *pair_replaced_lines(const DiffHunk *hunk);
