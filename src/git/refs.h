#pragma once

#include <glib.h>

#include "git/log.h"

/* A branch or tag in the repository */
typedef struct {
	/* Such as "main", "origin/main" or "v1.0" */
	char *name;
	char *full_name;
	ReferenceKind kind;
	char *commit;
	char *upstream;
	int ahead, behind;
	/* The branch it follows was deleted on the remote */
	gboolean is_upstream_gone;
	gboolean is_current;
	gint64 time;
	char *subject;
} Reference;

/* An annotated tag names a tag object, so the commit under it is read instead */
#define REFERENCES_FORMAT "--format=%(refname)%1f%(if)%(*objectname)%(then)%(*objectname)%(else)%(objectname)%(end)" \
	"%1f%(upstream:short)%1f%(upstream:track,nobracket)" \
	"%1f%(if)%(*committerdate)%(then)%(*committerdate:unix)%(else)%(committerdate:unix)%(end)%1f%(HEAD)%1f%(contents:subject)"

/* Reads `git for-each-ref` written with REFERENCES_FORMAT */
GPtrArray *parse_references_list(const char *output, gsize length);
void free_reference_item(gpointer reference);

typedef struct {
	char *hash;
	/* Such as "stash@{0}" */
	char *name;
	gint64 time;
	/* The name it was given, or NULL when it was put aside without one, such as by `git stash` alone */
	char *message;
	/* The branch it was put aside on */
	char *branch;
	/* What git wrote, such as "On main: message", which a dropped stash is stored back with */
	char *subject;
} Stash;

#define STASH_FORMAT "--format=%H%x1f%gd%x1f%ct%x1f%gs"

/* Reads `git stash list -z` written with STASH_FORMAT */
GPtrArray *parse_stashes(const char *output, gsize length);
void free_stash(gpointer stash);
