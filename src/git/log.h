#pragma once

#include <glib.h>

typedef enum {
	REFERENCE_BRANCH,
	REFERENCE_REMOTE,
	REFERENCE_TAG,
	/* A commit checked out on its own, with no branch */
	REFERENCE_DETACHED,
} ReferenceKind;

/* A branch or tag pointing at a commit, shown as a chip beside it */
typedef struct {
	char *name;
	ReferenceKind kind;
	/* The branch that is checked out */
	gboolean is_current;
} CommitReference;

typedef struct {
	char *hash;
	GStrv parents;
	char *author_name, *author_email;
	gint64 author_time, commit_time;
	char *subject;
	GPtrArray *references;
	/* Only filled for one commit's details */
	char *committer_name, *committer_email;
	char *body;
	/* The letter git gives a signature, such as G for good and N for none */
	char signature;
	char *signer;
} Commit;

/* The format these parsers read, with -z between commits */
#define LOG_FORMAT "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%D%x1f%s"
#define DETAILS_FORMAT "--format=%H%x1f%P%x1f%an%x1f%ae%x1f%at%x1f%ct%x1f%D%x1f%s%x1f%cn%x1f%ce%x1f%G?%x1f%GS%x1f%b"
#define DECORATE_OPTION "--decorate=full"

/* Reads commits written with LOG_FORMAT and -z */
GPtrArray *parse_log(const char *output, gsize length);
/* Reads one commit written with DETAILS_FORMAT */
Commit *parse_commit_details(const char *output, gsize length);
void free_commit(gpointer commit);
Commit *copy_commit(const Commit *commit);

/* The short form people know a commit by */
char *get_short_hash(const char *hash);
gboolean is_merge_commit(const Commit *commit);
