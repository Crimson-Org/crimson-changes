#pragma once

#include <gio/gio.h>

/* What one git command gave back, with the output kept as raw bytes since paths and file contents need not be
 * text */
typedef struct {
	int exit_status;
	GBytes *output;
	char *error;
	/* The command was stopped before it ended, so nothing it gave should be used */
	gboolean is_cancelled;
} GitResult;

typedef void (*GitDone)(GitResult *result, gpointer data);

/* Extra settings for one command, all optional */
typedef struct {
	/* Bytes written to the command's input, such as a patch or a commit message */
	GBytes *input;
	GCancellable *cancellable;
	/* The editor git starts for a message, such as a command that copies a prepared file over it */
	const char *editor;
	/* A command run without being asked for, such as a fetch when the window comes back, which must fail rather
	 * than ask for a password */
	gboolean is_quiet;
} GitOptions;

/* Runs git in the folder without waiting, then calls done on the main thread, which owns nothing in the result
 * after it returns. The arguments end with NULL */
void run_git(const char *folder, const char *const *arguments, const GitOptions *options, GitDone done,
	gpointer data);
/* The same, waiting for the end, for the tests and for the version check before any window opens */
GitResult *run_git_now(const char *folder, const char *const *arguments, GBytes *input);
void free_git_result(GitResult *result);

/* The output as text without the last line break, or an empty string */
char *get_git_output_text(const GitResult *result);
/* The first line of what git printed when it failed, without "fatal: " and similar marks, for a short message */
char *get_git_error_summary(const GitResult *result);
gboolean has_git_succeeded(const GitResult *result);

/* The launcher and command line every git command uses, for callers that read the output as it comes */
GSubprocessLauncher *create_git_launcher(const char *folder, GSubprocessFlags flags, const char *editor,
	gboolean is_quiet);
/* The full command line, starting with git and ending with NULL, which only borrows the arguments */
GPtrArray *build_git_command(const char *const *arguments);

/* Whether the git program can be found at all */
gboolean is_git_installed(void);
