#include <string.h>

#include "git/records.h"
#include "git/blame.h"

/* SHA-1 and SHA-256 repositories */
#define SHA1_HASH_LENGTH 40
#define SHA256_HASH_LENGTH 64
#define CONTENT_MARK '\t'
#define UNCOMMITTED_DIGITS "0"


static void free_blame_commit(gpointer data) {
	BlameCommit *commit = data;
	g_free(commit->hash);
	g_free(commit->author);
	g_free(commit->summary);
	g_free(commit);
}


static guint find_or_add_commit(Blame *blame, GHashTable *indexes, const char *hash) {
	gpointer found;
	if (g_hash_table_lookup_extended(indexes, hash, NULL, &found))
		return GPOINTER_TO_UINT(found);
	BlameCommit *commit = g_new0(BlameCommit, 1);
	commit->hash = g_strdup(hash);
	/* Lines not committed yet carry a hash of only zeros */
	commit->is_uncommitted = strspn(hash, UNCOMMITTED_DIGITS) == strlen(hash);
	guint index = blame->commits->len;
	g_ptr_array_add(blame->commits, commit);
	g_hash_table_insert(indexes, commit->hash, GUINT_TO_POINTER(index));
	return index;
}


static void read_commit_detail(BlameCommit *commit, const char *line) {
	if (g_str_has_prefix(line, "author "))
		g_set_str(&commit->author, line + strlen("author "));
	else if (g_str_has_prefix(line, "author-time "))
		commit->time = g_ascii_strtoll(line + strlen("author-time "), NULL, 10);
	else if (g_str_has_prefix(line, "summary "))
		g_set_str(&commit->summary, line + strlen("summary "));
}


static void note_time(Blame *blame, const BlameCommit *commit) {
	if (commit->is_uncommitted || commit->time == 0)
		return;
	if (blame->oldest_time == 0 || commit->time < blame->oldest_time)
		blame->oldest_time = commit->time;
	blame->newest_time = MAX(blame->newest_time, commit->time);
}


/* Each group of lines starts with the commit's hash and the line numbers */
static gboolean is_group_start(const char *text) {
	gsize hash_length = strspn(text, HEX_DIGITS);
	return (hash_length == SHA1_HASH_LENGTH || hash_length == SHA256_HASH_LENGTH) && text[hash_length] == ' ';
}


Blame *parse_blame(const char *output, gsize length) {
	Blame *blame = g_new0(Blame, 1);
	blame->commits = g_ptr_array_new_with_free_func(free_blame_commit);
	blame->line_commits = g_array_new(FALSE, FALSE, sizeof(guint));
	blame->text = g_string_new(NULL);
	g_autoptr(GHashTable) indexes = g_hash_table_new(g_str_hash, g_str_equal);
	gsize position = 0;
	BlameCommit *current = NULL;
	guint current_index = 0;
	while (position < length) {
		const char *line = output + position;
		const char *end = memchr(line, '\n', length - position);
		gsize line_length = end ? (gsize)(end - line) : length - position;
		position += line_length + 1;
		g_autofree char *text = g_strndup(line, line_length);
		if (text[0] == CONTENT_MARK) {
			if (!current)
				continue;
			g_string_append(blame->text, text + 1);
			g_string_append_c(blame->text, '\n');
			g_array_append_val(blame->line_commits, current_index);
			note_time(blame, current);
		} else if (is_group_start(text)) {
			text[strspn(text, HEX_DIGITS)] = 0;
			current_index = find_or_add_commit(blame, indexes, text);
			current = g_ptr_array_index(blame->commits, current_index);
		} else if (current)
			read_commit_detail(current, text);
	}
	return blame;
}


void free_blame(Blame *blame) {
	if (!blame)
		return;
	g_ptr_array_unref(blame->commits);
	g_array_unref(blame->line_commits);
	g_string_free(blame->text, TRUE);
	g_free(blame);
}
