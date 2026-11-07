#include <string.h>
#include <stdlib.h>

#include "git/records.h"
#include "git/status.h"

/* The fields before the path on each kind of line, which the path follows whole even with spaces in it */
#define ORDINARY_FIELDS 8
#define RENAMED_FIELDS 9
#define UNMERGED_FIELDS 10
#define HEADER_MARK "# "
#define INITIAL_COMMIT "(initial)"
#define DETACHED_HEAD "(detached)"
/* The first letter of the submodule field, after the kind, a space, the two change letters and a space */
#define SUBMODULE_MARK_OFFSET 5
#define NOT_SUBMODULE 'N'


static void free_status_file(gpointer data) {
	StatusFile *file = data;
	g_free(file->path);
	g_free(file->old_path);
	g_free(file);
}


/* The text after the first count of spaces, which is where the path starts */
static const char *skip_fields(const char *line, int count) {
	for (int index = 0; index < count && line; index++) {
		line = strchr(line, ' ');
		if (line)
			line++;
	}
	return line;
}


static void read_header(Status *status, const char *line) {
	if (g_str_has_prefix(line, "branch.oid "))
		status->head_commit = strcmp(line + strlen("branch.oid "), INITIAL_COMMIT) == 0 ? NULL :
			g_strdup(line + strlen("branch.oid "));
	else if (g_str_has_prefix(line, "branch.head "))
		status->branch = strcmp(line + strlen("branch.head "), DETACHED_HEAD) == 0 ? NULL :
			g_strdup(line + strlen("branch.head "));
	else if (g_str_has_prefix(line, "branch.upstream ")) {
		status->upstream = g_strdup(line + strlen("branch.upstream "));
		status->has_upstream = TRUE;
	} else if (g_str_has_prefix(line, "branch.ab ")) {
		/* Such as "+2 -1" */
		const char *counts = line + strlen("branch.ab ");
		status->ahead = abs(atoi(counts));
		const char *behind = strchr(counts, ' ');
		status->behind = behind ? abs(atoi(behind + 1)) : 0;
	} else if (g_str_has_prefix(line, "stash "))
		status->stash_count = atoi(line + strlen("stash "));
}


static StatusFile *read_changed_file(const char *line, int fields) {
	const char *path = skip_fields(line, fields);
	if (!path || strlen(line) <= SUBMODULE_MARK_OFFSET)
		return NULL;
	StatusFile *file = g_new0(StatusFile, 1);
	file->staged = line[2];
	file->unstaged = line[3];
	file->is_submodule = line[SUBMODULE_MARK_OFFSET] != NOT_SUBMODULE;
	file->path = g_strdup(path);
	return file;
}


static StatusFile *read_untracked_file(const char *line) {
	StatusFile *file = g_new0(StatusFile, 1);
	file->staged = CHANGE_NONE;
	file->unstaged = CHANGE_UNTRACKED;
	file->path = g_strdup(line + 2);
	return file;
}


Status *parse_status(const char *output, gsize length) {
	Status *status = g_new0(Status, 1);
	status->files = g_ptr_array_new_with_free_func(free_status_file);
	gsize position = 0;
	const char *record;
	gsize record_length;
	while (read_next_record(output, length, &position, &record, &record_length)) {
		/* The output need not end with a NUL, so each line is copied before it is read as text */
		g_autofree char *line = g_strndup(record, record_length);
		StatusFile *file = NULL;
		if (g_str_has_prefix(line, HEADER_MARK))
			read_header(status, line + strlen(HEADER_MARK));
		else if (line[0] == '1')
			file = read_changed_file(line, ORDINARY_FIELDS);
		else if (line[0] == '2') {
			file = read_changed_file(line, RENAMED_FIELDS);
			/* The old name follows as a record of its own */
			if (file && read_next_record(output, length, &position, &record, &record_length))
				file->old_path = g_strndup(record, record_length);
		} else if (line[0] == 'u') {
			file = read_changed_file(line, UNMERGED_FIELDS);
			if (file) {
				file->is_conflicted = TRUE;
				file->staged = CHANGE_UNMERGED;
				file->unstaged = CHANGE_UNMERGED;
			}
		} else if (line[0] == '?')
			file = read_untracked_file(line);
		if (file)
			g_ptr_array_add(status->files, file);
	}
	return status;
}


void free_status(Status *status) {
	if (!status)
		return;
	g_free(status->head_commit);
	g_free(status->branch);
	g_free(status->upstream);
	g_ptr_array_unref(status->files);
	g_free(status);
}


guint count_staged_files(const Status *status) {
	guint count = 0;
	for (guint index = 0; index < status->files->len; index++) {
		StatusFile *file = g_ptr_array_index(status->files, index);
		count += !file->is_conflicted && file->staged != CHANGE_NONE;
	}
	return count;
}


guint count_unstaged_files(const Status *status) {
	guint count = 0;
	for (guint index = 0; index < status->files->len; index++) {
		StatusFile *file = g_ptr_array_index(status->files, index);
		count += !file->is_conflicted && file->unstaged != CHANGE_NONE;
	}
	return count;
}


guint count_conflicted_files(const Status *status) {
	guint count = 0;
	for (guint index = 0; index < status->files->len; index++)
		count += ((StatusFile *)g_ptr_array_index(status->files, index))->is_conflicted;
	return count;
}


gboolean is_status_clean(const Status *status) {
	return status->files->len == 0;
}
