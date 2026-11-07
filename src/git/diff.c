#include <string.h>
#include <stdlib.h>

#include "git/diff.h"

#define FILE_START "diff --git "
#define OLD_FILE_MARK "--- "
#define NEW_FILE_MARK "+++ "
#define HUNK_MARK "@@ "
#define NO_FILE "/dev/null"
#define OLD_PREFIX "a/"
#define NEW_PREFIX "b/"
#define QUOTE '"'
/* Git writes a submodule's mode as this, a commit inside the tree */
#define SUBMODULE_MODE "160000"
#define NO_NEWLINE_MARK '\\'


/* Line slices of the diff text, so the parser can read one line at a time without copying the whole text */
typedef struct {
	const char *text;
	gsize length, position;
} LineReader;


static gboolean read_line(LineReader *reader, const char **line, gsize *line_length) {
	if (reader->position >= reader->length)
		return FALSE;
	*line = reader->text + reader->position;
	const char *end = memchr(*line, '\n', reader->length - reader->position);
	*line_length = end ? (gsize)(end - *line) : reader->length - reader->position;
	reader->position += *line_length + (end ? 1 : 0);
	return TRUE;
}


static gboolean peek_line_starts(const LineReader *reader, const char *prefix) {
	gsize length = strlen(prefix);
	return reader->position + length <= reader->length &&
		memcmp(reader->text + reader->position, prefix, length) == 0;
}


static gboolean line_starts(const char *line, gsize line_length, const char *prefix) {
	gsize length = strlen(prefix);
	return line_length >= length && memcmp(line, prefix, length) == 0;
}


char *unquote_git_path(const char *path) {
	gsize length = strlen(path);
	if (length >= 2 && path[0] == QUOTE && path[length - 1] == QUOTE) {
		g_autofree char *inner = g_strndup(path + 1, length - 2);
		return g_strcompress(inner);
	}
	return g_strdup(path);
}


/* A path from a ---, +++ or rename line, without the a/ or b/ git puts before it */
static char *read_marked_path(const char *text, const char *prefix) {
	/* Git ends a name holding spaces with a tab here, so tools can tell where the name ends */
	g_autofree char *trimmed = g_strdup(text);
	gsize length = strlen(trimmed);
	if (length > 0 && trimmed[length - 1] == '\t')
		trimmed[length - 1] = 0;
	g_autofree char *path = unquote_git_path(trimmed);
	if (strcmp(path, NO_FILE) == 0)
		return NULL;
	if (g_str_has_prefix(path, prefix))
		return g_strdup(path + strlen(prefix));
	return g_steal_pointer(&path);
}


/* "diff --git a/x b/x" names the file twice, and when the two names are the same the line splits in the middle,
 * which is the only safe split for names with spaces. A renamed file gets its names from the rename lines */
static char *read_start_path(const char *rest) {
	gsize length = strlen(rest);
	if (length % 2 == 1) {
		gsize half = length / 2;
		g_autofree char *old_half = g_strndup(rest, half);
		const char *new_half = rest + half + 1;
		if (rest[half] == ' ' && g_str_has_prefix(old_half, OLD_PREFIX) && g_str_has_prefix(new_half, NEW_PREFIX) &&
			strcmp(old_half + strlen(OLD_PREFIX), new_half + strlen(NEW_PREFIX)) == 0)
			return g_strdup(new_half + strlen(NEW_PREFIX));
	}
	const char *separator = strstr(rest, " " NEW_PREFIX);
	return separator ? g_strdup(separator + 1 + strlen(NEW_PREFIX)) : g_strdup(rest);
}


static void free_diff_line(gpointer data) {
	DiffLine *line = data;
	g_free(line->text);
	g_free(line);
}


static void free_diff_hunk(gpointer data) {
	DiffHunk *hunk = data;
	g_free(hunk->section);
	g_ptr_array_unref(hunk->lines);
	g_free(hunk);
}


void free_diff_file(gpointer data) {
	DiffFile *file = data;
	if (!file)
		return;
	g_free(file->path);
	g_free(file->old_path);
	g_free(file->header);
	g_ptr_array_unref(file->hunks);
	g_free(file);
}


static DiffFile *create_diff_file(void) {
	DiffFile *file = g_new0(DiffFile, 1);
	file->kind = CHANGE_MODIFIED;
	file->hunks = g_ptr_array_new_with_free_func(free_diff_hunk);
	return file;
}


/* Reads a count such as "12,4" or "12", where a missing length means one line */
static const char *read_range(const char *text, int *start, int *count) {
	char *end = NULL;
	*start = (int)strtol(text, &end, 10);
	*count = 1;
	if (*end == ',')
		*count = (int)strtol(end + 1, &end, 10);
	return end;
}


static DiffHunk *read_hunk_header(const char *line, gsize line_length) {
	g_autofree char *text = g_strndup(line, line_length);
	DiffHunk *hunk = g_new0(DiffHunk, 1);
	hunk->lines = g_ptr_array_new_with_free_func(free_diff_line);
	const char *position = strchr(text, '-');
	if (position)
		position = read_range(position + 1, &hunk->old_start, &hunk->old_count);
	position = position ? strchr(position, '+') : NULL;
	if (position)
		position = read_range(position + 1, &hunk->new_start, &hunk->new_count);
	const char *section = position ? strstr(position, " @@") : NULL;
	hunk->section = g_strdup(section ? g_strchug((char *)section + strlen(" @@")) : "");
	return hunk;
}


/* Reads a hunk's lines, counting which line of the old and new file each one is */
static void read_hunk_lines(LineReader *reader, DiffFile *file, DiffHunk *hunk) {
	int old_left = hunk->old_count, new_left = hunk->new_count;
	int old_number = hunk->old_start, new_number = hunk->new_start;
	while (old_left > 0 || new_left > 0 || peek_line_starts(reader, "\\")) {
		const char *line;
		gsize line_length;
		gsize before = reader->position;
		if (!read_line(reader, &line, &line_length))
			break;
		char mark = line_length > 0 ? line[0] : ' ';
		if (mark != ' ' && mark != '+' && mark != '-' && mark != NO_NEWLINE_MARK) {
			reader->position = before;
			break;
		}
		DiffLine *diff_line = g_new0(DiffLine, 1);
		diff_line->kind = (DiffLineKind)mark;
		/* The bytes are kept as they are, so a patch built from them applies to files in any encoding */
		diff_line->text = g_strndup(line_length > 0 ? line + 1 : line, line_length > 0 ? line_length - 1 : 0);
		if (mark == ' ' || mark == '-')
			diff_line->old_number = old_number++, old_left--;
		if (mark == ' ' || mark == '+')
			diff_line->new_number = new_number++, new_left--;
		file->added += mark == '+';
		file->removed += mark == '-';
		g_ptr_array_add(hunk->lines, diff_line);
	}
}


/* Reads the lines between "diff --git" and the first hunk, which say what kind of change it is */
static void read_file_header(LineReader *reader, DiffFile *file, GString *header) {
	const char *line;
	gsize line_length;
	while (!peek_line_starts(reader, FILE_START) && !peek_line_starts(reader, HUNK_MARK) &&
		read_line(reader, &line, &line_length)) {
		g_string_append_len(header, line, line_length);
		g_string_append_c(header, '\n');
		g_autofree char *text = g_strndup(line, line_length);
		if (g_str_has_prefix(text, "new file mode"))
			file->kind = CHANGE_ADDED;
		else if (g_str_has_prefix(text, "deleted file mode"))
			file->kind = CHANGE_DELETED;
		else if (g_str_has_prefix(text, "rename from ")) {
			file->kind = CHANGE_RENAMED;
			file->old_path = unquote_git_path(text + strlen("rename from "));
		} else if (g_str_has_prefix(text, "rename to ")) {
			g_free(file->path);
			file->path = unquote_git_path(text + strlen("rename to "));
		} else if (g_str_has_prefix(text, "copy from ")) {
			file->kind = CHANGE_COPIED;
			file->old_path = unquote_git_path(text + strlen("copy from "));
		} else if (g_str_has_prefix(text, "copy to ")) {
			g_free(file->path);
			file->path = unquote_git_path(text + strlen("copy to "));
		} else if (g_str_has_prefix(text, "old mode "))
			file->is_mode_change = TRUE;
		else if (g_str_has_prefix(text, "Binary files ") || g_str_has_prefix(text, "GIT binary patch"))
			file->is_binary = TRUE;
		else if (g_str_has_prefix(text, NEW_FILE_MARK)) {
			g_autofree char *path = read_marked_path(text + strlen(NEW_FILE_MARK), NEW_PREFIX);
			if (path)
				g_set_str(&file->path, path);
		}
		/* Such as "new file mode 160000" or "index 1a2b..3c4d 160000" */
		if (strstr(text, "mode " SUBMODULE_MODE) || (g_str_has_prefix(text, "index ") &&
			g_str_has_suffix(text, " " SUBMODULE_MODE)))
			file->is_submodule = TRUE;
	}
}


/* Skips the encoded data of a binary patch, which ends at the next file */
static void skip_to_next_file(LineReader *reader) {
	const char *line;
	gsize line_length;
	while (!peek_line_starts(reader, FILE_START) && read_line(reader, &line, &line_length))
		;
}


GPtrArray *parse_diff(const char *text, gsize length) {
	GPtrArray *files = g_ptr_array_new_with_free_func(free_diff_file);
	LineReader reader = {text, length, 0};
	const char *line;
	gsize line_length;
	while (read_line(&reader, &line, &line_length)) {
		if (!line_starts(line, line_length, FILE_START))
			continue;
		DiffFile *file = create_diff_file();
		g_autofree char *rest = g_strndup(line + strlen(FILE_START), line_length - strlen(FILE_START));
		file->path = read_start_path(rest);
		GString *header = g_string_new(NULL);
		g_string_append_len(header, line, line_length);
		g_string_append_c(header, '\n');
		read_file_header(&reader, file, header);
		file->header = g_string_free(header, FALSE);
		while (peek_line_starts(&reader, HUNK_MARK) && read_line(&reader, &line, &line_length)) {
			DiffHunk *hunk = read_hunk_header(line, line_length);
			read_hunk_lines(&reader, file, hunk);
			g_ptr_array_add(file->hunks, hunk);
		}
		if (file->is_binary)
			skip_to_next_file(&reader);
		g_ptr_array_add(files, file);
	}
	return files;
}


DiffFile *create_added_file_diff(const char *path, const char *text, gsize length) {
	DiffFile *file = create_diff_file();
	file->path = g_strdup(path);
	file->kind = CHANGE_UNTRACKED;
	file->header = g_strdup("");
	if (memchr(text, 0, length)) {
		file->is_binary = TRUE;
		return file;
	}
	DiffHunk *hunk = g_new0(DiffHunk, 1);
	hunk->lines = g_ptr_array_new_with_free_func(free_diff_line);
	hunk->section = g_strdup("");
	hunk->new_start = 1;
	LineReader reader = {text, length, 0};
	const char *line;
	gsize line_length;
	while (read_line(&reader, &line, &line_length)) {
		DiffLine *diff_line = g_new0(DiffLine, 1);
		diff_line->kind = DIFF_LINE_ADDED;
		diff_line->new_number = ++hunk->new_count;
		diff_line->text = g_strndup(line, line_length);
		g_ptr_array_add(hunk->lines, diff_line);
	}
	file->added = hunk->new_count;
	if (hunk->lines->len > 0)
		g_ptr_array_add(file->hunks, hunk);
	else
		free_diff_hunk(hunk);
	return file;
}


char *build_hunk_patch(const DiffFile *file, const DiffHunk *hunk) {
	GString *patch = g_string_new(file->header);
	g_string_append_printf(patch, "@@ -%d,%d +%d,%d @@\n", hunk->old_start, hunk->old_count, hunk->new_start,
		hunk->new_count);
	for (guint index = 0; index < hunk->lines->len; index++) {
		DiffLine *line = g_ptr_array_index(hunk->lines, index);
		g_string_append_c(patch, (char)line->kind);
		g_string_append(patch, line->text);
		g_string_append_c(patch, '\n');
	}
	return g_string_free(patch, FALSE);
}


guint hash_diff_file(const DiffFile *file) {
	guint hash = g_str_hash(file->path) ^ (guint)file->kind;
	for (guint hunk_index = 0; hunk_index < file->hunks->len; hunk_index++) {
		DiffHunk *hunk = g_ptr_array_index(file->hunks, hunk_index);
		hash = hash * 31 + (guint)hunk->old_start * 7 + (guint)hunk->new_start;
		for (guint index = 0; index < hunk->lines->len; index++) {
			DiffLine *line = g_ptr_array_index(hunk->lines, index);
			hash = hash * 31 + g_str_hash(line->text) + (guint)line->kind;
		}
	}
	return hash;
}
