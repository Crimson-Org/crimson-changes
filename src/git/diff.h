#pragma once

#include <glib.h>

#include "git/status.h"

typedef enum {
	DIFF_LINE_CONTEXT = ' ',
	DIFF_LINE_ADDED = '+',
	DIFF_LINE_REMOVED = '-',
	/* "\ No newline at end of file", which belongs to the line before it */
	DIFF_LINE_NO_NEWLINE = '\\',
} DiffLineKind;

typedef struct {
	DiffLineKind kind;
	/* Line numbers in the old and new file, or 0 where the line is not in that file */
	int old_number, new_number;
	/* The line without its mark and line break, as the file's own bytes, which need not be UTF-8 */
	char *text;
} DiffLine;

typedef struct {
	int old_start, old_count, new_start, new_count;
	/* The function or heading git found above the hunk, or an empty string */
	char *section;
	GPtrArray *lines;
} DiffHunk;

typedef struct {
	char *path;
	/* The name before a rename or copy, or NULL */
	char *old_path;
	ChangeKind kind;
	gboolean is_binary;
	/* Only the file mode changed, such as a script made runnable */
	gboolean is_mode_change;
	gboolean is_submodule;
	/* A new file too large to show line by line */
	gboolean is_large;
	int added, removed;
	/* The lines from "diff --git" to "+++", which a patch of single hunks starts with */
	char *header;
	GPtrArray *hunks;
} DiffFile;

/* Reads the unified diff git prints with --patch, returning DiffFile items */
GPtrArray *parse_diff(const char *text, gsize length);
/* An added file made from the file's text, for files git does not track yet */
DiffFile *create_added_file_diff(const char *path, const char *text, gsize length);
void free_diff_file(gpointer file);

/* A patch of one hunk that `git apply` takes, such as for staging only that hunk */
char *build_hunk_patch(const DiffFile *file, const DiffHunk *hunk);
/* The lines of all hunks, to tell whether a file's diff changed since it was shown */
guint hash_diff_file(const DiffFile *file);
/* Removes the quotes and escapes git puts around unusual paths */
char *unquote_git_path(const char *path);
