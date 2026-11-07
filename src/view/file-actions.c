#include <string.h>
#include <gio/gio.h>
#include <glib/gstdio.h>

#include "lang/en.h"
#include "view/file-actions.h"
#include "view/format.h"
#include "view/dialogs.h"
#include "git/features.h"

/* A file larger than this is not kept in memory for Undo, so its toast offers no Undo */
#define LARGEST_KEPT_FILE (64 * 1024 * 1024)

/* A file as it was before a change, so Undo can put it back exactly, or remove it if it did not exist */
typedef struct {
	char *path;
	gboolean existed;
	/* The file's bytes, or NULL for a file too large to keep, which Undo then leaves alone */
	GBytes *bytes;
} FileSnapshot;

typedef struct {
	Window *window;
	char *what;
	GPtrArray *snapshots;
	/* The toast to show once the command worked, with Undo when snapshots were kept */
	char *success;
} ActionRequest;


static void free_snapshot(gpointer data) {
	FileSnapshot *snapshot = data;
	g_free(snapshot->path);
	if (snapshot->bytes)
		g_bytes_unref(snapshot->bytes);
	g_free(snapshot);
}


static FileSnapshot *take_snapshot(Repository *repository, const char *path) {
	FileSnapshot *snapshot = g_new0(FileSnapshot, 1);
	snapshot->path = g_build_filename(get_repository_folder(repository), path, NULL);
	GStatBuf status;
	snapshot->existed = g_stat(snapshot->path, &status) == 0;
	char *contents = NULL;
	gsize length = 0;
	/* The bytes are copied, since a mapped file would break if an editor shortened the file before Undo */
	if (snapshot->existed && status.st_size <= LARGEST_KEPT_FILE &&
		g_file_get_contents(snapshot->path, &contents, &length, NULL))
		snapshot->bytes = g_bytes_new_take(contents, length);
	return snapshot;
}


/* Undo is only offered when every file can be put back exactly */
static gboolean can_restore_all(GPtrArray *snapshots) {
	for (guint index = 0; index < snapshots->len; index++) {
		FileSnapshot *snapshot = g_ptr_array_index(snapshots, index);
		if (snapshot->existed && !snapshot->bytes)
			return FALSE;
	}
	return TRUE;
}


static void restore_snapshots(gpointer data) {
	GPtrArray *snapshots = data;
	for (guint index = 0; index < snapshots->len; index++) {
		FileSnapshot *snapshot = g_ptr_array_index(snapshots, index);
		g_autoptr(GFile) file = g_file_new_for_path(snapshot->path);
		g_autoptr(GError) error = NULL;
		if (!snapshot->existed)
			g_file_delete(file, NULL, NULL);
		else if (snapshot->bytes) {
			g_autoptr(GFile) parent = g_file_get_parent(file);
			g_file_make_directory_with_parents(parent, NULL, NULL);
			if (!g_file_replace_contents(file, g_bytes_get_data(snapshot->bytes, NULL), g_bytes_get_size(snapshot->bytes),
				NULL, FALSE, G_FILE_CREATE_NONE, NULL, NULL, &error))
				g_warning("%s could not be put back: %s", snapshot->path, error->message);
		}
	}
}


static void free_request(ActionRequest *request) {
	g_free(request->what);
	g_free(request->success);
	if (request->snapshots)
		g_ptr_array_unref(request->snapshots);
	release_window(request->window);
	g_free(request);
}


static void finish_action(GitResult *result, gpointer data) {
	ActionRequest *request = data;
	if (result->is_cancelled) {
		free_request(request);
		return;
	}
	if (!has_git_succeeded(result))
		show_git_failure(request->window, request->what, result);
	else if (request->success && request->snapshots && can_restore_all(request->snapshots))
		show_undo_toast(request->window, request->success, restore_snapshots, g_steal_pointer(&request->snapshots),
			(GDestroyNotify)g_ptr_array_unref);
	else if (request->success)
		show_toast(request->window, request->success);
	free_request(request);
}


static ActionRequest *create_request(Window *window, const char *what) {
	ActionRequest *request = g_new0(ActionRequest, 1);
	request->window = keep_window(window);
	request->what = g_strdup(what);
	return request;
}


static void run_action(Window *window, const char *what, const char *const *arguments, GBytes *input,
	ActionRequest *request) {
	if (!request)
		request = create_request(window, what);
	GitOptions options = {.input = input};
	run_repository_command(get_window_repository(window), arguments, &options, finish_action, request);
}


/* A file's path and its old path after a rename, ended with NULL, after the given arguments */
static GPtrArray *build_arguments(const char *const *start, const DiffFile *file) {
	GPtrArray *arguments = g_ptr_array_new();
	for (const char *const *argument = start; *argument; argument++)
		g_ptr_array_add(arguments, (gpointer)*argument);
	g_ptr_array_add(arguments, (gpointer)"--");
	g_ptr_array_add(arguments, file->path);
	if (file->old_path)
		g_ptr_array_add(arguments, file->old_path);
	g_ptr_array_add(arguments, NULL);
	return arguments;
}


void stage_file(Window *window, const DiffFile *file) {
	const char *start[] = {"add", "-A", NULL};
	g_autoptr(GPtrArray) arguments = build_arguments(start, file);
	run_action(window, TEXT_STAGE_FAILED, (const char *const *)arguments->pdata, NULL, NULL);
}


void unstage_file(Window *window, const DiffFile *file) {
	/* Before the first commit there is nothing to restore from, so the file only leaves the staging area */
	const char *restore[] = {"restore", "--staged", NULL};
	const char *remove[] = {"rm", "--cached", "-r", "-q", NULL};
	gboolean has_commits = has_repository_commits(get_window_repository(window));
	g_autoptr(GPtrArray) arguments = build_arguments(has_commits ? restore : remove, file);
	run_action(window, TEXT_UNSTAGE_FAILED, (const char *const *)arguments->pdata, NULL, NULL);
}


static ActionRequest *create_undoable_request(Window *window, const char *what, const char *success,
	const char *path) {
	ActionRequest *request = create_request(window, what);
	request->success = g_strdup(success);
	request->snapshots = g_ptr_array_new_with_free_func(free_snapshot);
	if (path)
		g_ptr_array_add(request->snapshots, take_snapshot(get_window_repository(window), path));
	return request;
}


/* A new file has no older version, so discarding it moves it to the trash, where it can still be found later */
static void discard_new_file(Window *window, const DiffFile *file) {
	Repository *repository = get_window_repository(window);
	g_autofree char *path = g_build_filename(get_repository_folder(repository), file->path, NULL);
	g_autoptr(GFile) target = g_file_new_for_path(path);
	g_autoptr(GError) error = NULL;
	FileSnapshot *snapshot = take_snapshot(repository, file->path);
	/* A file the trash cannot take is kept, since deleting it for good cannot be undone */
	if (!g_file_trash(target, NULL, &error)) {
		g_warning("%s could not go to the trash: %s", path, error->message);
		show_toast(window, TEXT_TRASH_FAILED);
		free_snapshot(snapshot);
		return;
	}
	GPtrArray *snapshots = g_ptr_array_new_with_free_func(free_snapshot);
	g_ptr_array_add(snapshots, snapshot);
	g_autofree char *text = g_strdup_printf(TEXT_DISCARDED_FILE, get_path_name(file->path));
	show_undo_toast(window, text, restore_snapshots, snapshots, (GDestroyNotify)g_ptr_array_unref);
	refresh_repository(repository);
}


void discard_file(Window *window, const DiffFile *file) {
	if (file->kind == CHANGE_UNTRACKED) {
		discard_new_file(window, file);
		return;
	}
	g_autofree char *text = g_strdup_printf(TEXT_DISCARDED_FILE, get_path_name(file->path));
	ActionRequest *request = create_undoable_request(window, TEXT_DISCARD_FAILED, text, file->path);
	const char *start[] = {"restore", "--worktree", NULL};
	g_autoptr(GPtrArray) arguments = build_arguments(start, file);
	run_action(window, NULL, (const char *const *)arguments->pdata, NULL, request);
}


/* A hunk shown without lines around it only applies when git is told so */
static void apply_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk, gboolean is_cached,
	gboolean is_reverse, ActionRequest *request) {
	g_autofree char *patch = build_hunk_patch(file, hunk);
	g_autoptr(GBytes) input = g_bytes_new(patch, strlen(patch));
	gboolean has_no_context = get_diff_options(get_window_repository(window))->context_lines == 0;
	g_autoptr(GPtrArray) arguments = g_ptr_array_new();
	g_ptr_array_add(arguments, (gpointer)"apply");
	g_ptr_array_add(arguments, (gpointer)"--whitespace=nowarn");
	if (is_cached)
		g_ptr_array_add(arguments, (gpointer)"--cached");
	if (is_reverse)
		g_ptr_array_add(arguments, (gpointer)"--reverse");
	if (has_no_context)
		g_ptr_array_add(arguments, (gpointer)"--unidiff-zero");
	g_ptr_array_add(arguments, (gpointer)"-");
	g_ptr_array_add(arguments, NULL);
	run_action(window, NULL, (const char *const *)arguments->pdata, input, request);
}


void stage_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk) {
	apply_hunk(window, file, hunk, TRUE, FALSE, create_request(window, TEXT_STAGE_FAILED));
}


void unstage_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk) {
	apply_hunk(window, file, hunk, TRUE, TRUE, create_request(window, TEXT_UNSTAGE_FAILED));
}


void discard_hunk(Window *window, const DiffFile *file, const DiffHunk *hunk) {
	apply_hunk(window, file, hunk, FALSE, TRUE, create_undoable_request(window, TEXT_DISCARD_FAILED,
		TEXT_DISCARDED_LINES, file->path));
}


void stage_all(Window *window) {
	const char *arguments[] = {"add", "-A", NULL};
	run_action(window, TEXT_STAGE_FAILED, arguments, NULL, NULL);
}


void unstage_all(Window *window) {
	gboolean has_commits = has_repository_commits(get_window_repository(window));
	const char *restore[] = {"restore", "--staged", ":/", NULL};
	const char *remove[] = {"rm", "--cached", "-r", "-q", ":/", NULL};
	run_action(window, TEXT_UNSTAGE_FAILED, has_commits ? restore : remove, NULL, NULL);
}


static void stash_with_name(const char *name, gpointer data) {
	Window *window = data;
	const char *arguments[] = {"stash", "push", "--include-untracked", "--message", name, NULL};
	ActionRequest *request = create_request(window, TEXT_STASH_PUSH_FAILED);
	request->success = g_strdup(TEXT_STASH_PUSHED);
	run_action(window, NULL, arguments, NULL, request);
}


void stash_all(Window *window) {
	ask_for_text(get_window_widget(window), TEXT_STASH_PUSH_HEADING, TEXT_STASH_PUSH_BODY, TEXT_STASH_PUSH_NAME,
		TEXT_STASH_PUSH, stash_with_name, window, NULL);
}


/* Every file not staged is put back, and new files go to the trash once that worked, all with one Undo */
typedef struct {
	ActionRequest *request;
	GPtrArray *new_files;
} DiscardAll;


static void trash_new_files(GitResult *result, gpointer data) {
	DiscardAll *discard = data;
	if (has_git_succeeded(result))
		for (guint index = 0; index < discard->new_files->len; index++) {
			g_autoptr(GFile) target = g_file_new_for_path(g_ptr_array_index(discard->new_files, index));
			g_autoptr(GError) error = NULL;
			if (!g_file_trash(target, NULL, &error))
				g_warning("%s could not go to the trash: %s", (char *)g_ptr_array_index(discard->new_files, index),
					error->message);
		}
	finish_action(result, discard->request);
	g_ptr_array_unref(discard->new_files);
	g_free(discard);
}


void discard_all(Window *window) {
	Repository *repository = get_window_repository(window);
	const WorkChanges *changes = get_work_changes(repository);
	if (!changes)
		return;
	DiscardAll *discard = g_new0(DiscardAll, 1);
	discard->request = create_undoable_request(window, TEXT_DISCARD_FAILED, TEXT_DISCARDED_ALL, NULL);
	discard->new_files = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 0; index < changes->unstaged->len; index++) {
		DiffFile *file = g_ptr_array_index(changes->unstaged, index);
		g_ptr_array_add(discard->request->snapshots, take_snapshot(repository, file->path));
		if (file->kind == CHANGE_UNTRACKED)
			g_ptr_array_add(discard->new_files, g_build_filename(get_repository_folder(repository), file->path, NULL));
	}
	const char *arguments[] = {"restore", "--worktree", ":/", NULL};
	run_repository_command(repository, arguments, NULL, trash_new_files, discard);
}


void restore_file_version(Window *window, const char *commit, const DiffFile *file) {
	/* A file the commit removed is restored as it was just before */
	g_autofree char *source = file->kind == CHANGE_DELETED ? g_strdup_printf("--source=%s^", commit) :
		g_strdup_printf("--source=%s", commit);
	g_autofree char *text = g_strdup_printf(TEXT_RESTORED_FILE, get_path_name(file->path));
	ActionRequest *request = create_undoable_request(window, TEXT_RESTORE_FAILED, text, file->path);
	const char *arguments[] = {"restore", "--worktree", source, "--", file->path, NULL};
	run_action(window, NULL, arguments, NULL, request);
}


/* The chosen side is written first and staged after, never both at once, so the file is never staged half done */
typedef struct {
	Window *window;
	char *path;
} Resolution;


static void stage_resolution(GitResult *result, gpointer data) {
	Resolution *resolution = data;
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(resolution->window, TEXT_RESOLVE_FAILED, result);
	else if (!result->is_cancelled)
		mark_resolved(resolution->window, resolution->path);
	release_window(resolution->window);
	g_free(resolution->path);
	g_free(resolution);
}


static void resolve_with(Window *window, const char *side, const char *path) {
	Resolution *resolution = g_new0(Resolution, 1);
	resolution->window = keep_window(window);
	resolution->path = g_strdup(path);
	const char *arguments[] = {"checkout", side, "--", path, NULL};
	run_repository_command(get_window_repository(window), arguments, NULL, stage_resolution, resolution);
}


/* During a rebase git's "ours" is the branch being rebased onto, so the user's own side is "theirs" */
static gboolean is_rebasing(Window *window) {
	return get_repository_operation(get_window_repository(window)) == OPERATION_REBASE;
}


void keep_our_version(Window *window, const char *path) {
	resolve_with(window, is_rebasing(window) ? "--theirs" : "--ours", path);
}


void keep_their_version(Window *window, const char *path) {
	resolve_with(window, is_rebasing(window) ? "--ours" : "--theirs", path);
}


/* Newer git refuses to stage a file that still holds conflict markers, which catches a file marked too early */
void mark_resolved(Window *window, const char *path) {
	gboolean is_checked = get_git_features()->can_add_resolved;
	const char *checked[] = {"add", "--resolved", "--", path, NULL};
	const char *plain[] = {"add", "--", path, NULL};
	g_autofree char *text = g_strdup_printf(TEXT_RESOLVED_FILE, get_path_name(path));
	ActionRequest *request = create_request(window, TEXT_RESOLVE_FAILED);
	request->success = g_steal_pointer(&text);
	run_action(window, NULL, is_checked ? checked : plain, NULL, request);
}


static void launch_uri(Window *window, const char *path, gboolean is_folder) {
	g_autofree char *full = g_build_filename(get_repository_folder(get_window_repository(window)), path, NULL);
	g_autoptr(GFile) file = g_file_new_for_path(full);
	if (is_folder) {
		g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
		gtk_file_launcher_open_containing_folder(launcher, get_gtk_window(window), NULL, NULL, NULL);
	} else {
		g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(file);
		gtk_file_launcher_launch(launcher, get_gtk_window(window), NULL, NULL, NULL);
	}
}


void open_work_file(Window *window, const char *path) {
	launch_uri(window, path, FALSE);
}


void open_work_file_folder(Window *window, const char *path) {
	launch_uri(window, path, TRUE);
}


void copy_text(Window *window, const char *text, const char *toast) {
	gdk_clipboard_set_text(gtk_widget_get_clipboard(get_window_widget(window)), text);
	if (toast)
		show_toast(window, toast);
}
