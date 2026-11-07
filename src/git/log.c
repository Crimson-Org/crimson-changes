#include <string.h>
#include <stdlib.h>

#include "git/records.h"
#include "git/log.h"

#define LIST_FIELDS 8
#define DETAILS_FIELDS 13
#define SHORT_HASH_LENGTH 7
#define HEAD_ARROW "HEAD -> "
#define DETACHED_MARK "HEAD"
#define TAG_MARK "tag: "
#define NO_SIGNATURE 'N'

enum {
	FIELD_HASH, FIELD_PARENTS, FIELD_AUTHOR_NAME, FIELD_AUTHOR_EMAIL, FIELD_AUTHOR_TIME, FIELD_COMMIT_TIME,
	FIELD_REFERENCES, FIELD_SUBJECT, FIELD_COMMITTER_NAME, FIELD_COMMITTER_EMAIL, FIELD_SIGNATURE, FIELD_SIGNER,
	FIELD_BODY,
};


static void free_reference(gpointer data) {
	CommitReference *reference = data;
	g_free(reference->name);
	g_free(reference);
}


static void add_reference(GPtrArray *references, const char *full_name, gboolean is_current) {
	CommitReference *reference = g_new0(CommitReference, 1);
	reference->is_current = is_current;
	if (g_str_has_prefix(full_name, BRANCH_PREFIX)) {
		reference->kind = REFERENCE_BRANCH;
		reference->name = g_strdup(full_name + strlen(BRANCH_PREFIX));
	} else if (g_str_has_prefix(full_name, REMOTE_PREFIX)) {
		reference->kind = REFERENCE_REMOTE;
		reference->name = g_strdup(full_name + strlen(REMOTE_PREFIX));
	} else if (g_str_has_prefix(full_name, TAG_PREFIX)) {
		reference->kind = REFERENCE_TAG;
		reference->name = g_strdup(full_name + strlen(TAG_PREFIX));
	} else {
		/* Other references, such as the stash, are shown elsewhere */
		free_reference(reference);
		return;
	}
	g_ptr_array_add(references, reference);
}


/* Reads decorations such as "HEAD -> refs/heads/main, tag: refs/tags/v1, refs/remotes/origin/main" */
static GPtrArray *parse_references(const char *text) {
	GPtrArray *references = g_ptr_array_new_with_free_func(free_reference);
	g_auto(GStrv) parts = g_strsplit(text, ", ", -1);
	for (char **part = parts; *part; part++) {
		const char *name = *part;
		if (!*name || g_str_has_suffix(name, REMOTE_HEAD_SUFFIX))
			continue;
		if (g_str_has_prefix(name, HEAD_ARROW))
			add_reference(references, name + strlen(HEAD_ARROW), TRUE);
		else if (strcmp(name, DETACHED_MARK) == 0) {
			CommitReference *reference = g_new0(CommitReference, 1);
			reference->kind = REFERENCE_DETACHED;
			reference->name = g_strdup(DETACHED_MARK);
			reference->is_current = TRUE;
			g_ptr_array_add(references, reference);
		} else if (g_str_has_prefix(name, TAG_MARK))
			add_reference(references, name + strlen(TAG_MARK), FALSE);
		else
			add_reference(references, name, FALSE);
	}
	return references;
}


static Commit *read_commit_fields(char **fields) {
	Commit *commit = g_new0(Commit, 1);
	commit->hash = g_strdup(fields[FIELD_HASH]);
	commit->parents = *fields[FIELD_PARENTS] ? g_strsplit(fields[FIELD_PARENTS], " ", -1) : g_new0(char *, 1);
	commit->author_name = g_strdup(fields[FIELD_AUTHOR_NAME]);
	commit->author_email = g_strdup(fields[FIELD_AUTHOR_EMAIL]);
	commit->author_time = g_ascii_strtoll(fields[FIELD_AUTHOR_TIME], NULL, 10);
	commit->commit_time = g_ascii_strtoll(fields[FIELD_COMMIT_TIME], NULL, 10);
	commit->references = parse_references(fields[FIELD_REFERENCES]);
	commit->subject = g_strdup(fields[FIELD_SUBJECT]);
	commit->signature = NO_SIGNATURE;
	return commit;
}


GPtrArray *parse_log(const char *output, gsize length) {
	GPtrArray *commits = g_ptr_array_new_with_free_func(free_commit);
	gsize position = 0;
	const char *record;
	gsize record_length;
	while (read_next_record(output, length, &position, &record, &record_length)) {
		g_autofree char *text = g_strstrip(g_strndup(record, record_length));
		g_auto(GStrv) fields = g_strsplit(text, FIELD_SEPARATOR, LIST_FIELDS);
		if (g_strv_length(fields) == LIST_FIELDS)
			g_ptr_array_add(commits, read_commit_fields(fields));
	}
	return commits;
}


Commit *parse_commit_details(const char *output, gsize length) {
	g_autofree char *text = g_strndup(output, length);
	g_auto(GStrv) fields = g_strsplit(text, FIELD_SEPARATOR, DETAILS_FIELDS);
	if (g_strv_length(fields) != DETAILS_FIELDS)
		return NULL;
	Commit *commit = read_commit_fields(fields);
	commit->committer_name = g_strdup(fields[FIELD_COMMITTER_NAME]);
	commit->committer_email = g_strdup(fields[FIELD_COMMITTER_EMAIL]);
	commit->signature = *fields[FIELD_SIGNATURE] ? fields[FIELD_SIGNATURE][0] : NO_SIGNATURE;
	commit->signer = g_strdup(fields[FIELD_SIGNER]);
	commit->body = g_strstrip(g_strdup(fields[FIELD_BODY]));
	return commit;
}


void free_commit(gpointer data) {
	Commit *commit = data;
	if (!commit)
		return;
	g_free(commit->hash);
	g_strfreev(commit->parents);
	g_free(commit->author_name);
	g_free(commit->author_email);
	g_free(commit->subject);
	if (commit->references)
		g_ptr_array_unref(commit->references);
	g_free(commit->committer_name);
	g_free(commit->committer_email);
	g_free(commit->body);
	g_free(commit->signer);
	g_free(commit);
}


Commit *copy_commit(const Commit *source) {
	Commit *commit = g_new0(Commit, 1);
	commit->hash = g_strdup(source->hash);
	commit->parents = g_strdupv(source->parents);
	commit->author_name = g_strdup(source->author_name);
	commit->author_email = g_strdup(source->author_email);
	commit->author_time = source->author_time;
	commit->subject = g_strdup(source->subject);
	commit->references = g_ptr_array_new_with_free_func(free_reference);
	for (guint index = 0; index < source->references->len; index++) {
		CommitReference *original = g_ptr_array_index(source->references, index);
		CommitReference *reference = g_new0(CommitReference, 1);
		*reference = *original;
		reference->name = g_strdup(original->name);
		g_ptr_array_add(commit->references, reference);
	}
	commit->committer_name = g_strdup(source->committer_name);
	commit->committer_email = g_strdup(source->committer_email);
	commit->commit_time = source->commit_time;
	commit->body = g_strdup(source->body);
	commit->signature = source->signature;
	commit->signer = g_strdup(source->signer);
	return commit;
}


char *get_short_hash(const char *hash) {
	return g_strndup(hash, SHORT_HASH_LENGTH);
}


gboolean is_merge_commit(const Commit *commit) {
	return g_strv_length(commit->parents) > 1;
}
