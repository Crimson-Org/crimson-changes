#pragma once

#include <gio/gio.h>

#include "git/runner.h"
#include "git/status.h"
#include "git/diff.h"
#include "git/refs.h"

/* Work git stopped in the middle of, which the user continues or abandons */
typedef enum {
	OPERATION_NONE,
	OPERATION_MERGE,
	OPERATION_REBASE,
	OPERATION_CHERRY_PICK,
	OPERATION_REVERT,
	OPERATION_BISECT,
	/* Patches from mail being applied with git am */
	OPERATION_APPLY,
} Operation;

/* The changes not committed yet, all read at the same moment */
typedef struct {
	Status *status;
	/* DiffFile items: what the next commit takes, and what is only in the work folder, new files included */
	GPtrArray *staged, *unstaged;
	int added, removed;
} WorkChanges;

#define DEFAULT_CONTEXT_LINES 3

/* How diffs are read, from the settings */
typedef struct {
	gboolean ignores_whitespace;
	int context_lines;
} DiffOptions;

/* One repository's state, kept fresh by watching its folders, with signals for each part that changed:
 * "changes-loaded" for the work folder, "references-loaded" for branches, tags and stashes, and "history-changed"
 * when the commits shown may differ */
#define TYPE_REPOSITORY (repository_get_type())
G_DECLARE_FINAL_TYPE(Repository, repository, CHANGES, REPOSITORY, GObject)

typedef enum {
	FIND_REPOSITORY_FOUND,
	FIND_REPOSITORY_NONE,
	/* A repository without a work folder, which has no files to show */
	FIND_REPOSITORY_BARE,
	FIND_REPOSITORY_NO_GIT,
	/* The folder is gone, such as a recent folder that was moved */
	FIND_REPOSITORY_MISSING,
	FIND_REPOSITORY_FAILED,
} FindResult;

/* Called with the repository, or NULL and why there is none */
typedef void (*RepositoryFound)(Repository *repository, FindResult result, const char *message, gpointer data);

/* Looks for the repository holding the folder or file, which may be a folder deep inside it */
void find_repository(const char *path, RepositoryFound found, gpointer data);

const char *get_repository_folder(Repository *repository);
const char *get_repository_git_folder(Repository *repository);
/* The work folder's own name */
const char *get_repository_name(Repository *repository);
/* NULL until the first read has finished */
const WorkChanges *get_work_changes(Repository *repository);
/* Reference and Stash items */
GPtrArray *get_repository_references(Repository *repository);
GPtrArray *get_repository_stashes(Repository *repository);
const Reference *find_current_branch(Repository *repository);
gboolean has_repository_remote(Repository *repository);
gboolean has_repository_commits(Repository *repository);
Operation get_repository_operation(Repository *repository);

void set_diff_options(Repository *repository, const DiffOptions *options);
const DiffOptions *get_diff_options(Repository *repository);
/* The diff options as git arguments, appended to the list */
void add_diff_arguments(Repository *repository, GPtrArray *arguments);

/* Reads everything again, such as when the window comes back into focus */
void refresh_repository(Repository *repository);
void start_watching_repository(Repository *repository);

/* Runs a git command that changes the repository, then reads what changed. Done may be NULL */
void run_repository_command(Repository *repository, const char *const *arguments, const GitOptions *options,
	GitDone done, gpointer data);
/* Runs a git command that only reads, in the repository's folder */
void read_repository(Repository *repository, const char *const *arguments, GitDone done, gpointer data);
/* The cancellable all reads of this repository share, cancelled when it closes */
GCancellable *get_repository_cancellable(Repository *repository);
void close_repository(Repository *repository);
