#pragma once

#include <glib.h>

typedef struct {
	char *hash;
	char *author;
	gint64 time;
	char *summary;
	/* Lines changed in the work folder and not committed yet */
	gboolean is_uncommitted;
} BlameCommit;

typedef struct {
	/* BlameCommit items, each once */
	GPtrArray *commits;
	/* For each line of the file, the index of the commit that last changed it */
	GArray *line_commits;
	/* The file's text as git read it, one line per blamed line */
	GString *text;
	gint64 oldest_time, newest_time;
} Blame;

/* Reads `git blame --porcelain` */
Blame *parse_blame(const char *output, gsize length);
void free_blame(Blame *blame);
