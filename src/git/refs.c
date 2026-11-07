#include <string.h>
#include <stdlib.h>

#include "git/records.h"
#include "git/refs.h"

#define REFERENCE_FIELDS 7
#define STASH_FIELDS 4
#define CURRENT_MARK "*"
#define AHEAD_WORD "ahead "
#define BEHIND_WORD "behind "
#define GONE_WORD "gone"
/* The stash message git writes before the branch, such as "WIP on main: " or "On main: " */
#define STASH_BRANCH_END ": "
#define WORK_IN_PROGRESS_MARK "WIP on "
#define NAMED_MARK "On "

enum {
	FIELD_NAME, FIELD_COMMIT, FIELD_UPSTREAM, FIELD_TRACK, FIELD_TIME, FIELD_HEAD, FIELD_SUBJECT,
};

enum {
	STASH_HASH, STASH_NAME, STASH_TIME, STASH_SUBJECT,
};


void free_reference_item(gpointer data) {
	Reference *reference = data;
	g_free(reference->name);
	g_free(reference->full_name);
	g_free(reference->commit);
	g_free(reference->upstream);
	g_free(reference->subject);
	g_free(reference);
}


/* Such as "ahead 2, behind 1", "behind 3" or "gone" */
static void read_tracking(Reference *reference, const char *track) {
	const char *ahead = strstr(track, AHEAD_WORD);
	const char *behind = strstr(track, BEHIND_WORD);
	reference->ahead = ahead ? atoi(ahead + strlen(AHEAD_WORD)) : 0;
	reference->behind = behind ? atoi(behind + strlen(BEHIND_WORD)) : 0;
	reference->is_upstream_gone = strcmp(track, GONE_WORD) == 0;
}


static gboolean read_kind(Reference *reference, const char *full_name) {
	const char *prefixes[] = {BRANCH_PREFIX, REMOTE_PREFIX, TAG_PREFIX};
	const ReferenceKind kinds[] = {REFERENCE_BRANCH, REFERENCE_REMOTE, REFERENCE_TAG};
	for (guint index = 0; index < G_N_ELEMENTS(prefixes); index++)
		if (g_str_has_prefix(full_name, prefixes[index])) {
			reference->kind = kinds[index];
			reference->name = g_strdup(full_name + strlen(prefixes[index]));
			return TRUE;
		}
	return FALSE;
}


GPtrArray *parse_references_list(const char *output, gsize length) {
	GPtrArray *references = g_ptr_array_new_with_free_func(free_reference_item);
	g_autofree char *text = g_strndup(output, length);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	for (char **line = lines; *line; line++) {
		g_auto(GStrv) fields = g_strsplit(*line, FIELD_SEPARATOR, REFERENCE_FIELDS);
		if (g_strv_length(fields) != REFERENCE_FIELDS || g_str_has_suffix(fields[FIELD_NAME], REMOTE_HEAD_SUFFIX))
			continue;
		Reference *reference = g_new0(Reference, 1);
		if (!read_kind(reference, fields[FIELD_NAME])) {
			g_free(reference);
			continue;
		}
		reference->full_name = g_strdup(fields[FIELD_NAME]);
		reference->commit = g_strdup(fields[FIELD_COMMIT]);
		reference->upstream = *fields[FIELD_UPSTREAM] ? g_strdup(fields[FIELD_UPSTREAM]) : NULL;
		read_tracking(reference, fields[FIELD_TRACK]);
		reference->time = g_ascii_strtoll(fields[FIELD_TIME], NULL, 10);
		reference->is_current = strcmp(fields[FIELD_HEAD], CURRENT_MARK) == 0;
		reference->subject = g_strdup(fields[FIELD_SUBJECT]);
		g_ptr_array_add(references, reference);
	}
	return references;
}


void free_stash(gpointer data) {
	Stash *stash = data;
	g_free(stash->hash);
	g_free(stash->name);
	g_free(stash->message);
	g_free(stash->subject);
	g_free(stash->branch);
	g_free(stash);
}


/* "On main: my message" gives the branch and the message. "WIP on main: 1a2b3c4 Fix it", which git writes when no
 * message was given, only gives the branch, since the rest is the commit it was made on and no name of its own */
static void read_stash_subject(Stash *stash, const char *subject) {
	gboolean is_unnamed = g_str_has_prefix(subject, WORK_IN_PROGRESS_MARK);
	const char *mark = is_unnamed ? WORK_IN_PROGRESS_MARK : NAMED_MARK;
	const char *end = strstr(subject, STASH_BRANCH_END);
	if (!g_str_has_prefix(subject, mark) || !end) {
		stash->message = g_strdup(subject);
		return;
	}
	stash->branch = g_strndup(subject + strlen(mark), end - subject - strlen(mark));
	stash->message = is_unnamed ? NULL : g_strdup(end + strlen(STASH_BRANCH_END));
}


GPtrArray *parse_stashes(const char *output, gsize length) {
	GPtrArray *stashes = g_ptr_array_new_with_free_func(free_stash);
	gsize position = 0;
	const char *record;
	gsize record_length;
	while (read_next_record(output, length, &position, &record, &record_length)) {
		g_autofree char *text = g_strstrip(g_strndup(record, record_length));
		g_auto(GStrv) fields = g_strsplit(text, FIELD_SEPARATOR, STASH_FIELDS);
		if (g_strv_length(fields) != STASH_FIELDS)
			continue;
		Stash *stash = g_new0(Stash, 1);
		stash->hash = g_strdup(fields[STASH_HASH]);
		stash->name = g_strdup(fields[STASH_NAME]);
		stash->time = g_ascii_strtoll(fields[STASH_TIME], NULL, 10);
		read_stash_subject(stash, fields[STASH_SUBJECT]);
		stash->subject = g_strdup(fields[STASH_SUBJECT]);
		g_ptr_array_add(stashes, stash);
	}
	return stashes;
}
