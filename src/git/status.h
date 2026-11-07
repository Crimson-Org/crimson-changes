#pragma once

#include <glib.h>

/* How a file changed, the same letters git uses */
typedef enum {
	CHANGE_NONE = '.',
	CHANGE_MODIFIED = 'M',
	CHANGE_TYPE = 'T',
	CHANGE_ADDED = 'A',
	CHANGE_DELETED = 'D',
	CHANGE_RENAMED = 'R',
	CHANGE_COPIED = 'C',
	CHANGE_UNMERGED = 'U',
	CHANGE_UNTRACKED = '?',
} ChangeKind;

/* One file that differs between the last commit, the staging area and the work folder */
typedef struct {
	char *path;
	/* The name before a rename, or NULL */
	char *old_path;
	/* The change already staged, and the change only in the work folder */
	ChangeKind staged, unstaged;
	gboolean is_conflicted;
	gboolean is_submodule;
} StatusFile;

/* The branch and its place against the branch it follows */
typedef struct {
	/* The commit checked out, or NULL in a repository without commits */
	char *head_commit;
	/* The branch name, or NULL when a commit is checked out on its own */
	char *branch;
	char *upstream;
	int ahead, behind;
	gboolean has_upstream;
	int stash_count;
	GPtrArray *files;
} Status;

/* Reads `git status --porcelain=v2 -z --branch --show-stash` */
Status *parse_status(const char *output, gsize length);
void free_status(Status *status);

guint count_staged_files(const Status *status);
guint count_unstaged_files(const Status *status);
guint count_conflicted_files(const Status *status);
gboolean is_status_clean(const Status *status);
