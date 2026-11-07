#include <string.h>

#include <glib/gstdio.h>

#include "git/repository.h"
#include "git/watch.h"

#define NOT_REPOSITORY_MARK "not a git repository"
#define BARE_MARK "must be run in a work tree"
/* A new file larger than this shows only its size, since nobody reads megabytes of added lines */
#define LARGEST_SHOWN_NEW_FILE (1024 * 1024)
/* New files past this many are listed without being read, since every change to the folder reads them again */
#define MOST_READ_NEW_FILES 400
#define UNTRACKED_PATH_KEY "path"
#define CONFIG_FILE "config"
#define REMOTE_SECTION "[remote "

struct _Repository {
	GObject parent;
	char *folder, *git_folder, *common_folder, *name;
	WorkChanges *changes;
	GPtrArray *references, *stashes;
	Operation operation;
	DiffOptions diff_options;
	FolderWatch *watch;
	GCancellable *cancellable;
	/* A read that is asked for while the same read runs waits and runs once more after it */
	gboolean is_loading_changes, wants_changes_again;
	gboolean is_loading_references, wants_references_again;
	/* What the history depended on when it was last announced, so it is only announced again when it changed */
	guint history_fingerprint;
	/* What git last said about the branches and the stashes, so a read that says the same changes nothing */
	GBytes *references_output, *stashes_output;
	gboolean has_read_references, has_remote;
	/* Commands that change the repository, waiting behind the one that runs */
	GQueue *commands;
	gboolean is_running_command;
	gboolean is_closed;
};

G_DEFINE_FINAL_TYPE(Repository, repository, G_TYPE_OBJECT)

enum {
	SIGNAL_CHANGES_LOADED,
	SIGNAL_REFERENCES_LOADED,
	SIGNAL_HISTORY_CHANGED,
	SIGNAL_COUNT,
};

static guint signals[SIGNAL_COUNT];

static void load_changes(Repository *repository);
static void load_references(Repository *repository);


static void free_work_changes(WorkChanges *changes) {
	if (!changes)
		return;
	free_status(changes->status);
	g_ptr_array_unref(changes->staged);
	g_ptr_array_unref(changes->unstaged);
	g_free(changes);
}


static void finalize_repository(GObject *object) {
	Repository *repository = CHANGES_REPOSITORY(object);
	stop_watching_folders(repository->watch);
	g_clear_object(&repository->cancellable);
	g_queue_free(repository->commands);
	free_work_changes(repository->changes);
	g_clear_pointer(&repository->references, g_ptr_array_unref);
	g_clear_pointer(&repository->stashes, g_ptr_array_unref);
	g_clear_pointer(&repository->references_output, g_bytes_unref);
	g_clear_pointer(&repository->stashes_output, g_bytes_unref);
	g_free(repository->folder);
	g_free(repository->git_folder);
	g_free(repository->common_folder);
	g_free(repository->name);
	G_OBJECT_CLASS(repository_parent_class)->finalize(object);
}


static void repository_class_init(RepositoryClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_repository;
	signals[SIGNAL_CHANGES_LOADED] = g_signal_new("changes-loaded", TYPE_REPOSITORY, G_SIGNAL_RUN_LAST, 0, NULL, NULL,
		NULL, G_TYPE_NONE, 0);
	signals[SIGNAL_REFERENCES_LOADED] = g_signal_new("references-loaded", TYPE_REPOSITORY, G_SIGNAL_RUN_LAST, 0,
		NULL, NULL, NULL, G_TYPE_NONE, 0);
	signals[SIGNAL_HISTORY_CHANGED] = g_signal_new("history-changed", TYPE_REPOSITORY, G_SIGNAL_RUN_LAST, 0, NULL,
		NULL, NULL, G_TYPE_NONE, 0);
}


static void repository_init(Repository *repository) {
	repository->cancellable = g_cancellable_new();
	repository->commands = g_queue_new();
	repository->references = g_ptr_array_new_with_free_func(free_reference_item);
	repository->stashes = g_ptr_array_new_with_free_func(free_stash);
	repository->diff_options.context_lines = DEFAULT_CONTEXT_LINES;
}


/* A repository finished with is never written to again, even by answers still on their way */
static gboolean is_usable(Repository *repository, GitResult *result) {
	return !repository->is_closed && !(result && result->is_cancelled);
}


/* Finding */

typedef struct {
	char *path;
	RepositoryFound found;
	gpointer data;
} FindRequest;


static void report_found(GitResult *result, gpointer data) {
	FindRequest *request = data;
	if (!has_git_succeeded(result)) {
		g_autofree char *message = get_git_error_summary(result);
		FindResult kind = result->exit_status < 0 ? FIND_REPOSITORY_NO_GIT :
			strstr(result->error, NOT_REPOSITORY_MARK) ? FIND_REPOSITORY_NONE :
			strstr(result->error, BARE_MARK) ? FIND_REPOSITORY_BARE : FIND_REPOSITORY_FAILED;
		request->found(NULL, kind, message, request->data);
	} else {
		g_autofree char *text = get_git_output_text(result);
		g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
		Repository *repository = g_object_new(TYPE_REPOSITORY, NULL);
		repository->folder = g_strdup(lines[0]);
		repository->git_folder = g_strdup(lines[0] && lines[1] ? lines[1] : lines[0]);
		/* A linked work tree keeps its branches in the main repository's folder */
		repository->common_folder = g_strdup(lines[0] && lines[1] && lines[2] ? lines[2] : repository->git_folder);
		repository->name = g_path_get_basename(repository->folder);
		request->found(repository, FIND_REPOSITORY_FOUND, NULL, request->data);
		g_object_unref(repository);
	}
	g_free(request->path);
	g_free(request);
}


void find_repository(const char *path, RepositoryFound found, gpointer data) {
	FindRequest *request = g_new0(FindRequest, 1);
	request->found = found;
	request->data = data;
	request->path = g_file_test(path, G_FILE_TEST_IS_DIR) ? g_strdup(path) : g_path_get_dirname(path);
	if (!g_file_test(request->path, G_FILE_TEST_IS_DIR)) {
		request->found(NULL, FIND_REPOSITORY_MISSING, NULL, request->data);
		g_free(request->path);
		g_free(request);
		return;
	}
	if (!is_git_installed()) {
		GitResult result = {.exit_status = -1, .error = (char *)""};
		report_found(&result, request);
		return;
	}
	const char *arguments[] = {"rev-parse", "--path-format=absolute", "--show-toplevel", "--git-dir",
		"--git-common-dir", NULL};
	run_git(request->path, arguments, NULL, report_found, request);
}


/* Plain getters */

const char *get_repository_folder(Repository *repository) {
	return repository->folder;
}


const char *get_repository_git_folder(Repository *repository) {
	return repository->git_folder;
}


const char *get_repository_name(Repository *repository) {
	return repository->name;
}


const WorkChanges *get_work_changes(Repository *repository) {
	return repository->changes;
}


GPtrArray *get_repository_references(Repository *repository) {
	return repository->references;
}


GPtrArray *get_repository_stashes(Repository *repository) {
	return repository->stashes;
}


const Reference *find_current_branch(Repository *repository) {
	for (guint index = 0; index < repository->references->len; index++) {
		Reference *reference = g_ptr_array_index(repository->references, index);
		if (reference->is_current)
			return reference;
	}
	return NULL;
}


gboolean has_repository_remote(Repository *repository) {
	return repository->has_remote;
}


/* A remote without fetched branches yet still counts, so its settings are read too, once per read of the branches */
static gboolean find_remote(Repository *repository) {
	for (guint index = 0; index < repository->references->len; index++)
		if (((Reference *)g_ptr_array_index(repository->references, index))->kind == REFERENCE_REMOTE)
			return TRUE;
	g_autofree char *config = g_build_filename(repository->common_folder, CONFIG_FILE, NULL);
	g_autofree char *text = NULL;
	return g_file_get_contents(config, &text, NULL, NULL) && strstr(text, REMOTE_SECTION);
}


gboolean has_repository_commits(Repository *repository) {
	return repository->changes && repository->changes->status->head_commit;
}


Operation get_repository_operation(Repository *repository) {
	return repository->operation;
}


GCancellable *get_repository_cancellable(Repository *repository) {
	return repository->cancellable;
}


void set_diff_options(Repository *repository, const DiffOptions *options) {
	repository->diff_options = *options;
	load_changes(repository);
}


const DiffOptions *get_diff_options(Repository *repository) {
	return &repository->diff_options;
}


void add_diff_arguments(Repository *repository, GPtrArray *arguments) {
	g_ptr_array_add(arguments, g_strdup("--no-ext-diff"));
	g_ptr_array_add(arguments, g_strdup("--no-color"));
	g_ptr_array_add(arguments, g_strdup("-M"));
	g_ptr_array_add(arguments, g_strdup_printf("-U%d", repository->diff_options.context_lines));
	if (repository->diff_options.ignores_whitespace)
		g_ptr_array_add(arguments, g_strdup("-w"));
}


/* The work folder's changes */

/* The parts of one read of the work folder, which arrive one by one and are shown together */
typedef struct {
	Repository *repository;
	Status *status;
	GPtrArray *staged, *unstaged, *untracked;
	int waiting;
} ChangesLoad;


static Operation find_operation(Repository *repository) {
	const struct {
		const char *name;
		Operation operation;
	} MARKS[] = {
		{"rebase-apply/applying", OPERATION_APPLY}, {"rebase-merge", OPERATION_REBASE},
		{"rebase-apply", OPERATION_REBASE}, {"MERGE_HEAD", OPERATION_MERGE},
		{"CHERRY_PICK_HEAD", OPERATION_CHERRY_PICK}, {"REVERT_HEAD", OPERATION_REVERT},
		{"BISECT_LOG", OPERATION_BISECT},
	};
	for (guint index = 0; index < G_N_ELEMENTS(MARKS); index++) {
		g_autofree char *path = g_build_filename(repository->git_folder, MARKS[index].name, NULL);
		if (g_file_test(path, G_FILE_TEST_EXISTS))
			return MARKS[index].operation;
	}
	return OPERATION_NONE;
}


static void count_lines(WorkChanges *changes, GPtrArray *files) {
	for (guint index = 0; index < files->len; index++) {
		DiffFile *file = g_ptr_array_index(files, index);
		changes->added += file->added;
		changes->removed += file->removed;
	}
}


static void free_changes_load(ChangesLoad *load) {
	free_status(load->status);
	g_clear_pointer(&load->staged, g_ptr_array_unref);
	g_clear_pointer(&load->unstaged, g_ptr_array_unref);
	g_clear_pointer(&load->untracked, g_ptr_array_unref);
	g_object_unref(load->repository);
	g_free(load);
}


/* The fingerprint of what the timeline shows: the commit checked out and which branch it is */
static guint fingerprint_head(const Status *status) {
	return (status->head_commit ? g_str_hash(status->head_commit) : 0) ^
		(status->branch ? g_str_hash(status->branch) : 1);
}


static void finish_changes_part(ChangesLoad *load) {
	if (--load->waiting > 0)
		return;
	Repository *repository = load->repository;
	if (!repository->is_closed && load->staged && load->unstaged && load->untracked) {
		WorkChanges *changes = g_new0(WorkChanges, 1);
		changes->status = g_steal_pointer(&load->status);
		changes->staged = g_steal_pointer(&load->staged);
		changes->unstaged = g_steal_pointer(&load->unstaged);
		g_ptr_array_extend_and_steal(changes->unstaged, g_steal_pointer(&load->untracked));
		count_lines(changes, changes->staged);
		count_lines(changes, changes->unstaged);
		guint head = fingerprint_head(changes->status);
		/* A branch that moves changes the branches too, so only a commit checked out on its own is news here */
		gboolean has_moved = repository->changes && head != fingerprint_head(repository->changes->status) &&
			!changes->status->branch;
		free_work_changes(repository->changes);
		repository->changes = changes;
		repository->operation = find_operation(repository);
		g_signal_emit(repository, signals[SIGNAL_CHANGES_LOADED], 0);
		if (has_moved)
			g_signal_emit(repository, signals[SIGNAL_HISTORY_CHANGED], 0);
	}
	repository->is_loading_changes = FALSE;
	gboolean is_wanted_again = repository->wants_changes_again && !repository->is_closed;
	free_changes_load(load);
	if (is_wanted_again)
		load_changes(repository);
}


static GPtrArray *read_diff_result(GitResult *result) {
	if (!has_git_succeeded(result))
		return NULL;
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	return parse_diff(output ? output : "", length);
}


static void read_staged(GitResult *result, gpointer data) {
	ChangesLoad *load = data;
	load->staged = read_diff_result(result);
	finish_changes_part(load);
}


static void read_unstaged(GitResult *result, gpointer data) {
	ChangesLoad *load = data;
	load->unstaged = read_diff_result(result);
	finish_changes_part(load);
}


/* New files are read away from the window, since a folder may hold many of them */
static void read_untracked_files(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	GPtrArray *paths = data;
	const char *folder = g_object_get_data(G_OBJECT(task), UNTRACKED_PATH_KEY);
	GPtrArray *files = g_ptr_array_new_with_free_func(free_diff_file);
	for (guint index = 0; index < paths->len && !g_cancellable_is_cancelled(cancellable); index++) {
		const char *path = g_ptr_array_index(paths, index);
		g_autofree char *full_path = g_build_filename(folder, path, NULL);
		/* Only plain files are read, never links or pipes, which could point anywhere or never end */
		g_autoptr(GFile) source = g_file_new_for_path(full_path);
		g_autoptr(GFileInfo) info = g_file_query_info(source, G_FILE_ATTRIBUTE_STANDARD_TYPE ","
			G_FILE_ATTRIBUTE_STANDARD_SIZE, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, cancellable, NULL);
		gboolean is_readable = index < MOST_READ_NEW_FILES && info &&
			g_file_info_get_file_type(info) == G_FILE_TYPE_REGULAR && g_file_info_get_size(info) <= LARGEST_SHOWN_NEW_FILE;
		g_autofree char *contents = NULL;
		gsize length = 0;
		if (is_readable && !g_file_get_contents(full_path, &contents, &length, NULL))
			length = 0;
		DiffFile *file = create_added_file_diff(path, contents ? contents : "", length);
		file->is_large = !is_readable;
		g_ptr_array_add(files, file);
	}
	g_task_return_pointer(task, files, (GDestroyNotify)g_ptr_array_unref);
}


static void finish_untracked(GObject *source, GAsyncResult *result, gpointer data) {
	ChangesLoad *load = data;
	load->untracked = g_task_propagate_pointer(G_TASK(result), NULL);
	finish_changes_part(load);
}


static void start_untracked_read(ChangesLoad *load) {
	GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
	for (guint index = 0; index < load->status->files->len; index++) {
		StatusFile *file = g_ptr_array_index(load->status->files, index);
		if (file->unstaged == CHANGE_UNTRACKED)
			g_ptr_array_add(paths, g_strdup(file->path));
	}
	g_autoptr(GTask) task = g_task_new(NULL, load->repository->cancellable, finish_untracked, load);
	g_object_set_data_full(G_OBJECT(task), UNTRACKED_PATH_KEY, g_strdup(load->repository->folder), g_free);
	g_task_set_task_data(task, paths, (GDestroyNotify)g_ptr_array_unref);
	g_task_run_in_thread(task, read_untracked_files);
}


static void run_diff(ChangesLoad *load, gboolean is_staged, GitDone done) {
	g_autoptr(GPtrArray) arguments = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(arguments, g_strdup("diff"));
	if (is_staged)
		g_ptr_array_add(arguments, g_strdup("--cached"));
	add_diff_arguments(load->repository, arguments);
	g_ptr_array_add(arguments, NULL);
	GitOptions options = {.cancellable = load->repository->cancellable};
	run_git(load->repository->folder, (const char *const *)arguments->pdata, &options, done, load);
}


static void read_status(GitResult *result, gpointer data) {
	ChangesLoad *load = data;
	if (!is_usable(load->repository, result) || !has_git_succeeded(result)) {
		if (!result->is_cancelled && !has_git_succeeded(result))
			g_warning("Status could not be read: %s", result->error);
		load->waiting = 1;
		finish_changes_part(load);
		return;
	}
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	load->status = parse_status(output ? output : "", length);
	load->waiting = 3;
	run_diff(load, TRUE, read_staged);
	run_diff(load, FALSE, read_unstaged);
	start_untracked_read(load);
}


static void load_changes(Repository *repository) {
	if (repository->is_closed)
		return;
	if (repository->is_loading_changes) {
		repository->wants_changes_again = TRUE;
		return;
	}
	repository->is_loading_changes = TRUE;
	repository->wants_changes_again = FALSE;
	ChangesLoad *load = g_new0(ChangesLoad, 1);
	load->repository = g_object_ref(repository);
	const char *arguments[] = {"status", "--porcelain=v2", "-z", "--branch", "--show-stash", "--untracked-files=all",
		NULL};
	GitOptions options = {.cancellable = repository->cancellable};
	run_git(repository->folder, arguments, &options, read_status, load);
}


/* Branches, tags and stashes */

typedef struct {
	Repository *repository;
	GPtrArray *references, *stashes;
	GBytes *references_output, *stashes_output;
	int waiting;
} ReferencesLoad;


static gboolean is_same_output(GBytes *before, GBytes *now) {
	return before && now && g_bytes_equal(before, now);
}


static guint fingerprint_references(GPtrArray *references) {
	guint hash = 0;
	for (guint index = 0; index < references->len; index++) {
		Reference *reference = g_ptr_array_index(references, index);
		hash = hash * 31 + g_str_hash(reference->full_name) + g_str_hash(reference->commit) + reference->is_current;
	}
	return hash;
}


static void finish_references_part(ReferencesLoad *load) {
	if (--load->waiting > 0)
		return;
	Repository *repository = load->repository;
	/* A read that says exactly what the last one said is no news, so open lists and menus stay as they are */
	gboolean is_unchanged = is_same_output(repository->references_output, load->references_output) &&
		is_same_output(repository->stashes_output, load->stashes_output);
	if (!repository->is_closed && load->references && load->stashes && !is_unchanged) {
		g_clear_pointer(&repository->references_output, g_bytes_unref);
		g_clear_pointer(&repository->stashes_output, g_bytes_unref);
		repository->references_output = g_bytes_ref(load->references_output);
		repository->stashes_output = g_bytes_ref(load->stashes_output);
		g_ptr_array_unref(repository->references);
		g_ptr_array_unref(repository->stashes);
		repository->references = g_steal_pointer(&load->references);
		repository->stashes = g_steal_pointer(&load->stashes);
		guint fingerprint = fingerprint_references(repository->references);
		for (guint index = 0; index < repository->stashes->len; index++)
			fingerprint = fingerprint * 31 + g_str_hash(((Stash *)g_ptr_array_index(repository->stashes, index))->hash);
		/* The first read is not news, since the history is read with the first changes anyway */
		gboolean has_changed = repository->has_read_references && fingerprint != repository->history_fingerprint;
		repository->history_fingerprint = fingerprint;
		repository->has_read_references = TRUE;
		repository->has_remote = find_remote(repository);
		g_signal_emit(repository, signals[SIGNAL_REFERENCES_LOADED], 0);
		if (has_changed)
			g_signal_emit(repository, signals[SIGNAL_HISTORY_CHANGED], 0);
	}
	repository->is_loading_references = FALSE;
	gboolean is_wanted_again = repository->wants_references_again && !repository->is_closed;
	g_clear_pointer(&load->references, g_ptr_array_unref);
	g_clear_pointer(&load->stashes, g_ptr_array_unref);
	g_clear_pointer(&load->references_output, g_bytes_unref);
	g_clear_pointer(&load->stashes_output, g_bytes_unref);
	g_object_unref(load->repository);
	g_free(load);
	if (is_wanted_again)
		load_references(repository);
}


static void read_references(GitResult *result, gpointer data) {
	ReferencesLoad *load = data;
	if (is_usable(load->repository, result) && has_git_succeeded(result)) {
		gsize length = 0;
		const char *output = g_bytes_get_data(result->output, &length);
		load->references = parse_references_list(output ? output : "", length);
		load->references_output = g_bytes_new(output, length);
	}
	finish_references_part(load);
}


static void read_stashes(GitResult *result, gpointer data) {
	ReferencesLoad *load = data;
	if (is_usable(load->repository, result)) {
		gsize length = 0;
		const char *output = g_bytes_get_data(result->output, &length);
		/* A repository without commits has no stash list at all, which is the same as an empty one */
		load->stashes = has_git_succeeded(result) ? parse_stashes(output ? output : "", length) :
			g_ptr_array_new_with_free_func(free_stash);
		load->stashes_output = g_bytes_new(output, length);
	}
	finish_references_part(load);
}


static void load_references(Repository *repository) {
	if (repository->is_closed)
		return;
	if (repository->is_loading_references) {
		repository->wants_references_again = TRUE;
		return;
	}
	repository->is_loading_references = TRUE;
	repository->wants_references_again = FALSE;
	ReferencesLoad *load = g_new0(ReferencesLoad, 1);
	load->repository = g_object_ref(repository);
	load->waiting = 2;
	GitOptions options = {.cancellable = repository->cancellable};
	const char *references[] = {"for-each-ref", REFERENCES_FORMAT, "refs/heads", "refs/remotes", "refs/tags", NULL};
	run_git(repository->folder, references, &options, read_references, load);
	const char *stashes[] = {"stash", "list", "-z", STASH_FORMAT, NULL};
	run_git(repository->folder, stashes, &options, read_stashes, load);
}


/* Watching and commands */

static void react_to_folders(WatchChanges changes, gpointer data) {
	Repository *repository = data;
	load_changes(repository);
	if (changes & WATCH_REFERENCES)
		load_references(repository);
}


void refresh_repository(Repository *repository) {
	load_changes(repository);
	load_references(repository);
}


void start_watching_repository(Repository *repository) {
	if (!repository->watch)
		repository->watch = watch_repository_folders(repository->folder, repository->git_folder,
			repository->common_folder, react_to_folders, repository);
	refresh_repository(repository);
}


/* A command that changes the repository, waiting for its turn */
typedef struct {
	Repository *repository;
	GStrv arguments;
	GBytes *input;
	char *editor;
	gboolean is_quiet;
	GitDone done;
	gpointer data;
} CommandRequest;


static void free_command_request(CommandRequest *request) {
	g_strfreev(request->arguments);
	if (request->input)
		g_bytes_unref(request->input);
	g_free(request->editor);
	g_object_unref(request->repository);
	g_free(request);
}


static void start_next_command(Repository *repository);


static void finish_command(GitResult *result, gpointer data) {
	CommandRequest *request = data;
	Repository *repository = request->repository;
	repository->is_running_command = FALSE;
	if (!repository->is_closed) {
		/* The file events would bring the same news a moment later, and reading now keeps the window in step with
		 * the button that was pressed */
		refresh_repository(repository);
	}
	if (request->done)
		request->done(result, request->data);
	g_object_ref(repository);
	free_command_request(request);
	start_next_command(repository);
	g_object_unref(repository);
}


/* Commands that change the repository run one at a time in the order they were asked for, so two of them never
 * fight over the staging area's lock */
static void start_next_command(Repository *repository) {
	if (repository->is_running_command || g_queue_is_empty(repository->commands))
		return;
	CommandRequest *request = g_queue_pop_head(repository->commands);
	repository->is_running_command = TRUE;
	g_autofree char *line = g_strjoinv(" ", request->arguments);
	g_message("Running git %s", line);
	GitOptions options = {.input = request->input, .editor = request->editor, .is_quiet = request->is_quiet};
	run_git(repository->folder, (const char *const *)request->arguments, &options, finish_command, request);
}


void run_repository_command(Repository *repository, const char *const *arguments, const GitOptions *options,
	GitDone done, gpointer data) {
	CommandRequest *request = g_new0(CommandRequest, 1);
	request->repository = g_object_ref(repository);
	request->arguments = g_strdupv((char **)arguments);
	request->input = options && options->input ? g_bytes_ref(options->input) : NULL;
	request->editor = g_strdup(options ? options->editor : NULL);
	request->is_quiet = options && options->is_quiet;
	request->done = done;
	request->data = data;
	g_queue_push_tail(repository->commands, request);
	start_next_command(repository);
}


void read_repository(Repository *repository, const char *const *arguments, GitDone done, gpointer data) {
	GitOptions options = {.cancellable = repository->cancellable};
	run_git(repository->folder, arguments, &options, done, data);
}


void close_repository(Repository *repository) {
	repository->is_closed = TRUE;
	g_cancellable_cancel(repository->cancellable);
	g_clear_pointer(&repository->watch, stop_watching_folders);
}
