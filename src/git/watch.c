#include <string.h>
#include <gio/gio.h>

#include "git/watch.h"
#include "git/runner.h"
#include "git/records.h"

/* Changes that come in a burst, such as a build or a checkout, wait this long for the rest of the burst */
#define SETTLE_MILLISECONDS 250
/* Every folder takes one system watch, and huge trees would use them all up, so past this the window's focus is
 * what notices changes */
#define MAXIMUM_WATCHED_FOLDERS 4000
#define GIT_FOLDER_NAME ".git"
#define INDEX_NAME "index"
#define LOCK_SUFFIX ".lock"
#define REFERENCES_FOLDER "refs"
#define REFTABLE_FOLDER "reftable"
#define OBJECTS_FOLDER "objects"

struct FolderWatch {
	char *work_folder, *git_folder, *common_folder;
	FoldersChanged changed;
	gpointer data;
	/* Each GFileMonitor keyed by its folder's path */
	GHashTable *monitors;
	/* Folders git ignores, such as build output, which are not watched */
	GHashTable *ignored;
	WatchChanges pending;
	/* Folders made since the last report, which are walked and checked against what git ignores */
	GPtrArray *new_folders;
	guint settle_source;
	GCancellable *cancellable;
};

static void add_folder_tree(FolderWatch *watch, GFile *folder, WatchChanges kind);
static void walk_work_folders(FolderWatch *watch, GPtrArray *roots, gboolean is_first);


static gboolean report_changes(gpointer data) {
	FolderWatch *watch = data;
	watch->settle_source = 0;
	/* The folders made during the burst are walked together, once it settled */
	if (watch->new_folders->len > 0) {
		walk_work_folders(watch, watch->new_folders, FALSE);
		watch->new_folders = g_ptr_array_new_with_free_func(g_free);
	}
	WatchChanges changes = watch->pending;
	watch->pending = 0;
	watch->changed(changes, watch->data);
	return G_SOURCE_REMOVE;
}


static void note_change(FolderWatch *watch, WatchChanges change) {
	watch->pending |= change;
	if (!watch->settle_source)
		watch->settle_source = g_timeout_add(SETTLE_MILLISECONDS, report_changes, watch);
}


/* Lock files come and go around every write, and the write itself shows up under the real name */
static gboolean is_lock_file(GFile *file) {
	g_autofree char *name = file ? g_file_get_basename(file) : NULL;
	return name && g_str_has_suffix(name, LOCK_SUFFIX);
}


static void change_git_folder(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
	FolderWatch *watch) {
	GFile *named = other && is_lock_file(file) ? other : file;
	if (is_lock_file(named))
		return;
	g_autofree char *name = g_file_get_basename(named);
	if (strcmp(name, OBJECTS_FOLDER) == 0)
		return;
	note_change(watch, strcmp(name, INDEX_NAME) == 0 ? WATCH_INDEX : WATCH_REFERENCES);
}


static void change_references(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
	FolderWatch *watch) {
	if (event == G_FILE_MONITOR_EVENT_CREATED && g_file_query_file_type(file, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
		NULL) == G_FILE_TYPE_DIRECTORY)
		add_folder_tree(watch, file, WATCH_REFERENCES);
	if (!is_lock_file(file) || other)
		note_change(watch, WATCH_REFERENCES);
}


static void forget_folder(FolderWatch *watch, GFile *file) {
	g_autofree char *path = g_file_get_path(file);
	if (path)
		g_hash_table_remove(watch->monitors, path);
}


static void change_work_folder(GFileMonitor *monitor, GFile *file, GFile *other, GFileMonitorEvent event,
	FolderWatch *watch) {
	/* A folder renamed in place is a folder gone under its old name and new under the new one */
	GFile *arrived = event == G_FILE_MONITOR_EVENT_RENAMED ? other : file;
	if ((event == G_FILE_MONITOR_EVENT_CREATED || event == G_FILE_MONITOR_EVENT_MOVED_IN ||
		event == G_FILE_MONITOR_EVENT_RENAMED) && arrived &&
		g_file_query_file_type(arrived, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL) == G_FILE_TYPE_DIRECTORY)
		g_ptr_array_add(watch->new_folders, g_file_get_path(arrived));
	/* A folder that went away takes its watch with it, so the list never fills with gone folders */
	if (event == G_FILE_MONITOR_EVENT_DELETED || event == G_FILE_MONITOR_EVENT_MOVED_OUT ||
		event == G_FILE_MONITOR_EVENT_RENAMED)
		forget_folder(watch, file);
	note_change(watch, WATCH_WORK_FOLDER);
}


static gboolean watch_folder(FolderWatch *watch, GFile *folder, WatchChanges kind) {
	g_autofree char *path = g_file_get_path(folder);
	if (!path || g_hash_table_contains(watch->monitors, path) ||
		g_hash_table_size(watch->monitors) >= MAXIMUM_WATCHED_FOLDERS)
		return FALSE;
	GFileMonitor *monitor = g_file_monitor_directory(folder, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
	if (!monitor)
		return FALSE;
	GCallback callback = kind == WATCH_WORK_FOLDER ? G_CALLBACK(change_work_folder) :
		kind == WATCH_REFERENCES ? G_CALLBACK(change_references) : G_CALLBACK(change_git_folder);
	g_signal_connect(monitor, "changed", callback, watch);
	g_hash_table_insert(watch->monitors, g_steal_pointer(&path), monitor);
	return TRUE;
}


static gboolean is_skipped_folder(FolderWatch *watch, GFile *folder) {
	g_autofree char *name = g_file_get_basename(folder);
	g_autofree char *path = g_file_get_path(folder);
	return strcmp(name, GIT_FOLDER_NAME) == 0 || (path && g_hash_table_contains(watch->ignored, path));
}


/* Walks the folders breadth first, so the top of a huge tree is watched before the watch limit is reached */
static void add_folder_tree(FolderWatch *watch, GFile *root, WatchChanges kind) {
	g_autoptr(GQueue) queue = g_queue_new();
	g_queue_push_tail(queue, g_object_ref(root));
	GFile *folder;
	while ((folder = g_queue_pop_head(queue))) {
		g_autoptr(GFile) owned = folder;
		if (is_skipped_folder(watch, folder) || !watch_folder(watch, folder, kind))
			continue;
		g_autoptr(GFileEnumerator) children = g_file_enumerate_children(folder,
			G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
			NULL, NULL);
		GFileInfo *info;
		while (children && (info = g_file_enumerator_next_file(children, NULL, NULL))) {
			if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
				g_queue_push_tail(queue, g_file_get_child(folder, g_file_info_get_name(info)));
			g_object_unref(info);
		}
	}
}


/* A linked work tree has its own staging area and checked out commit, while the branches live in the main
 * repository's folder */
static void watch_git_folder(FolderWatch *watch) {
	g_autoptr(GFile) git_folder = g_file_new_for_path(watch->git_folder);
	g_autoptr(GFile) common_folder = g_file_new_for_path(watch->common_folder);
	watch_folder(watch, git_folder, WATCH_INDEX);
	watch_folder(watch, common_folder, WATCH_INDEX);
	const char *reference_folders[] = {REFERENCES_FOLDER, REFTABLE_FOLDER};
	for (guint index = 0; index < G_N_ELEMENTS(reference_folders); index++) {
		g_autoptr(GFile) folder = g_file_get_child(common_folder, reference_folders[index]);
		if (g_file_query_exists(folder, NULL))
			add_folder_tree(watch, folder, WATCH_REFERENCES);
	}
}


/* The folders of the work folder found away from the window, breadth first and up to the watch limit, since a
 * large tree takes a while to walk */
typedef struct {
	GPtrArray *roots;
	/* The first walk knows every ignored folder already, while later walks ask git about what they found */
	gboolean is_first;
	GHashTable *ignored;
	GPtrArray *found;
	/* Folders already watched, so a walk from a new folder stays within the limit */
	guint watched;
} FolderWalk;


static void free_folder_walk(gpointer data) {
	FolderWalk *walk = data;
	g_ptr_array_unref(walk->roots);
	g_hash_table_unref(walk->ignored);
	if (walk->found)
		g_ptr_array_unref(walk->found);
	g_free(walk);
}


static void walk_folders(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	FolderWalk *walk = data;
	walk->found = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GQueue) queue = g_queue_new();
	for (guint index = 0; index < walk->roots->len; index++)
		g_queue_push_tail(queue, g_strdup(g_ptr_array_index(walk->roots, index)));
	char *path;
	while ((path = g_queue_pop_head(queue))) {
		g_autofree char *name = g_path_get_basename(path);
		if (walk->watched + walk->found->len >= MAXIMUM_WATCHED_FOLDERS || g_cancellable_is_cancelled(cancellable) ||
			strcmp(name, GIT_FOLDER_NAME) == 0 || g_hash_table_contains(walk->ignored, path)) {
			g_free(path);
			continue;
		}
		g_ptr_array_add(walk->found, path);
		g_autoptr(GFile) folder = g_file_new_for_path(path);
		g_autoptr(GFileEnumerator) children = g_file_enumerate_children(folder,
			G_FILE_ATTRIBUTE_STANDARD_NAME "," G_FILE_ATTRIBUTE_STANDARD_TYPE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
			cancellable, NULL);
		GFileInfo *info;
		while (children && (info = g_file_enumerator_next_file(children, cancellable, NULL))) {
			if (g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY)
				g_queue_push_tail(queue, g_build_filename(path, g_file_info_get_name(info), NULL));
			g_object_unref(info);
		}
	}
	g_queue_free_full(g_steal_pointer(&queue), g_free);
	g_task_return_boolean(task, TRUE);
}


static void watch_found(FolderWatch *watch, GPtrArray *found, GHashTable *ignored) {
	for (guint index = 0; index < found->len; index++) {
		const char *path = g_ptr_array_index(found, index);
		if (ignored && g_hash_table_contains(ignored, path)) {
			g_hash_table_add(watch->ignored, g_strdup(path));
			continue;
		}
		g_autoptr(GFile) folder = g_file_new_for_path(path);
		watch_folder(watch, folder, WATCH_WORK_FOLDER);
	}
	g_message("Watching %u folders", g_hash_table_size(watch->monitors));
}


/* Git names the ignored ones among the folders it was given, one per NUL ended record */
typedef struct {
	FolderWatch *watch;
	GPtrArray *found;
} IgnoreCheck;


static void read_ignored_found(GitResult *result, gpointer data) {
	IgnoreCheck *check = data;
	if (!result->is_cancelled) {
		g_autoptr(GHashTable) ignored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
		gsize length = 0;
		const char *output = g_bytes_get_data(result->output, &length);
		gsize position = 0;
		const char *entry;
		gsize entry_length;
		while (read_next_record(output, length, &position, &entry, &entry_length))
			if (entry_length > 0)
				g_hash_table_add(ignored, g_strndup(entry, entry_length));
		watch_found(check->watch, check->found, ignored);
	}
	g_ptr_array_unref(check->found);
	g_free(check);
}


static void finish_walk(GObject *source, GAsyncResult *result, gpointer data) {
	if (!g_task_propagate_boolean(G_TASK(result), NULL) || g_cancellable_is_cancelled(g_task_get_cancellable(G_TASK(result))))
		return;
	FolderWatch *watch = data;
	FolderWalk *walk = g_task_get_task_data(G_TASK(result));
	if (walk->is_first || walk->found->len == 0) {
		watch_found(watch, walk->found, NULL);
		return;
	}
	/* One git call checks every folder found, so unpacking a large tree starts one command, not hundreds */
	GString *input = g_string_new(NULL);
	for (guint index = 0; index < walk->found->len; index++)
		g_string_append_len(input, g_ptr_array_index(walk->found, index),
			(gssize)strlen(g_ptr_array_index(walk->found, index)) + 1);
	g_autoptr(GBytes) bytes = g_string_free_to_bytes(input);
	IgnoreCheck *check = g_new0(IgnoreCheck, 1);
	check->watch = watch;
	check->found = g_ptr_array_ref(walk->found);
	const char *arguments[] = {"check-ignore", "--stdin", "-z", NULL};
	GitOptions options = {.input = bytes, .cancellable = watch->cancellable};
	run_git(watch->work_folder, arguments, &options, read_ignored_found, check);
}


/* The ignored folders come as paths relative to the work folder, ending with a slash */
static void read_ignored_folders(GitResult *result, gpointer data) {
	if (result->is_cancelled)
		return;
	FolderWatch *watch = data;
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	gsize position = 0;
	const char *entry;
	gsize entry_length;
	while (read_next_record(output, length, &position, &entry, &entry_length)) {
		if (entry_length > 1 && entry[entry_length - 1] == '/') {
			g_autofree char *relative = g_strndup(entry, entry_length - 1);
			g_hash_table_add(watch->ignored, g_build_filename(watch->work_folder, relative, NULL));
		}
	}
	g_autoptr(GPtrArray) roots = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(roots, g_strdup(watch->work_folder));
	walk_work_folders(watch, g_steal_pointer(&roots), TRUE);
}


/* The walk takes the roots and reads a copy of the ignored folders, since the window adds to its own list
 * meanwhile */
static void walk_work_folders(FolderWatch *watch, GPtrArray *roots, gboolean is_first) {
	FolderWalk *walk = g_new0(FolderWalk, 1);
	walk->roots = roots;
	walk->is_first = is_first;
	walk->ignored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GHashTableIter iterator;
	gpointer path;
	g_hash_table_iter_init(&iterator, watch->ignored);
	while (g_hash_table_iter_next(&iterator, &path, NULL))
		g_hash_table_add(walk->ignored, g_strdup(path));
	walk->watched = g_hash_table_size(watch->monitors);
	g_autoptr(GTask) task = g_task_new(NULL, watch->cancellable, finish_walk, watch);
	g_task_set_task_data(task, walk, free_folder_walk);
	g_task_run_in_thread(task, walk_folders);
}


FolderWatch *watch_repository_folders(const char *work_folder, const char *git_folder, const char *common_folder,
	FoldersChanged changed, gpointer data) {
	FolderWatch *watch = g_new0(FolderWatch, 1);
	watch->work_folder = g_strdup(work_folder);
	watch->git_folder = g_strdup(git_folder);
	watch->common_folder = g_strdup(common_folder);
	watch->changed = changed;
	watch->data = data;
	watch->monitors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
	watch->ignored = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	watch->cancellable = g_cancellable_new();
	watch->new_folders = g_ptr_array_new_with_free_func(g_free);
	watch_git_folder(watch);
	const char *arguments[] = {"ls-files", "-z", "--others", "--ignored", "--exclude-standard", "--directory", NULL};
	GitOptions options = {.cancellable = watch->cancellable};
	run_git(work_folder, arguments, &options, read_ignored_folders, watch);
	return watch;
}


void stop_watching_folders(FolderWatch *watch) {
	if (!watch)
		return;
	g_cancellable_cancel(watch->cancellable);
	g_clear_object(&watch->cancellable);
	g_clear_handle_id(&watch->settle_source, g_source_remove);
	g_hash_table_unref(watch->monitors);
	g_hash_table_unref(watch->ignored);
	g_ptr_array_unref(watch->new_folders);
	g_free(watch->work_folder);
	g_free(watch->git_folder);
	g_free(watch->common_folder);
	g_free(watch);
}
