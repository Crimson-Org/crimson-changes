#pragma once

#include <glib.h>

/* What the installed git can do, read once, since newer commands only exist in newer versions */
typedef struct {
	int major, minor;
	/* git history reword and drop, from 2.54 and 2.56 */
	gboolean can_reword, can_drop;
	/* git branch --delete-merged and git add --resolved, from 2.56 */
	gboolean can_delete_merged, can_add_resolved;
	/* git repo structure, from 2.52 */
	gboolean can_describe_structure;
} GitFeatures;

/* Reads the version the first time it is asked for */
const GitFeatures *get_git_features(void);
/* Reads a line such as "git version 2.56.0" */
void read_git_version(const char *text, int *major, int *minor);
