#include <string.h>

#include "git/runner.h"

#define GIT_PROGRAM "git"
#define ERROR_MARKS_COUNT 4
/* A quiet command never asks for a password or passphrase, and fails instead */
#define QUIET_SSH_COMMAND "ssh -o BatchMode=yes"
#define QUIET_ASK_PASS "true"

/* Settings every command runs with whatever the user's own git settings say, since the parsers rely on them:
 * unquoted paths, no colours, and the a/ and b/ marks before the paths of a diff */
static const char *const FIXED_SETTINGS[] = {
	"core.quotePath=false", "color.ui=false", "color.diff=false", "diff.noprefix=false", "diff.mnemonicPrefix=false",
	"diff.relative=false", "log.showSignature=false",
};

/* Marks git puts before a message, which say nothing to a person reading it */
static const char *const ERROR_MARKS[ERROR_MARKS_COUNT] = {"fatal: ", "error: ", "warning: ", "hint: "};

typedef struct {
	GitDone done;
	gpointer data;
} RunRequest;


/* Every command reads and writes in plain English with machine output, never asks on a terminal and never takes
 * the optional locks a read would hold against the user's own git in a terminal */
GSubprocessLauncher *create_git_launcher(const char *folder, GSubprocessFlags flags, const char *editor,
	gboolean is_quiet) {
	GSubprocessLauncher *launcher = g_subprocess_launcher_new(flags);
	g_subprocess_launcher_set_cwd(launcher, folder);
	g_subprocess_launcher_setenv(launcher, "LC_ALL", "C", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_OPTIONAL_LOCKS", "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_TERMINAL_PROMPT", "0", TRUE);
	g_subprocess_launcher_setenv(launcher, "GIT_PAGER", "cat", TRUE);
	/* A command that wants a message never waits on an editor nobody sees, unless one was prepared for it */
	g_subprocess_launcher_setenv(launcher, "GIT_EDITOR", editor ? editor : "true", TRUE);
	g_subprocess_launcher_unsetenv(launcher, "GIT_DIR");
	g_subprocess_launcher_unsetenv(launcher, "GIT_WORK_TREE");
	if (is_quiet) {
		g_subprocess_launcher_setenv(launcher, "GIT_SSH_COMMAND", QUIET_SSH_COMMAND, FALSE);
		g_subprocess_launcher_setenv(launcher, "GIT_ASKPASS", QUIET_ASK_PASS, TRUE);
		g_subprocess_launcher_setenv(launcher, "SSH_ASKPASS_REQUIRE", "never", TRUE);
	}
	return launcher;
}


GPtrArray *build_git_command(const char *const *arguments) {
	GPtrArray *command = g_ptr_array_new();
	g_ptr_array_add(command, (gpointer)GIT_PROGRAM);
	for (guint index = 0; index < G_N_ELEMENTS(FIXED_SETTINGS); index++) {
		g_ptr_array_add(command, (gpointer)"-c");
		g_ptr_array_add(command, (gpointer)FIXED_SETTINGS[index]);
	}
	for (const char *const *argument = arguments; *argument; argument++)
		g_ptr_array_add(command, (gpointer)*argument);
	g_ptr_array_add(command, NULL);
	return command;
}


/* The input is always a pipe, closed at once when there is nothing to send, so git, ssh or gpg can never read the
 * terminal the app was started from */
static GSubprocessLauncher *create_launcher(const char *folder, const char *editor, gboolean is_quiet) {
	GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
		G_SUBPROCESS_FLAGS_STDIN_PIPE;
	return create_git_launcher(folder, flags, editor, is_quiet);
}


static GitResult *create_failed_result(GError *error) {
	GitResult *result = g_new0(GitResult, 1);
	result->exit_status = -1;
	result->is_cancelled = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
	result->error = g_strdup(error->message);
	result->output = g_bytes_new(NULL, 0);
	return result;
}


static GitResult *create_result(GSubprocess *process, GBytes *output, GBytes *error_output) {
	GitResult *result = g_new0(GitResult, 1);
	result->exit_status = g_subprocess_get_if_exited(process) ? g_subprocess_get_exit_status(process) : -1;
	result->output = output ? g_bytes_ref(output) : g_bytes_new(NULL, 0);
	gsize length = 0;
	const char *text = error_output ? g_bytes_get_data(error_output, &length) : NULL;
	result->error = g_strndup(text ? text : "", length);
	return result;
}


static void finish_run(GObject *source, GAsyncResult *async_result, gpointer data) {
	RunRequest *request = data;
	GSubprocess *process = G_SUBPROCESS(source);
	g_autoptr(GBytes) output = NULL;
	g_autoptr(GBytes) error_output = NULL;
	g_autoptr(GError) error = NULL;
	GitResult *result;
	if (g_subprocess_communicate_finish(process, async_result, &output, &error_output, &error))
		result = create_result(process, output, error_output);
	else
		result = create_failed_result(error);
	request->done(result, request->data);
	free_git_result(result);
	g_object_unref(process);
	g_free(request);
}


/* A request that could not start still answers through the main loop, so every caller sees the same order */
typedef struct {
	RunRequest *request;
	GError *error;
	GCancellable *cancellable;
} FailedStart;


static gboolean report_failed_start(gpointer data) {
	FailedStart *start = data;
	GitResult *result = create_failed_result(start->error);
	/* A caller that cancelled meanwhile may have freed what the answer would reach */
	result->is_cancelled |= start->cancellable && g_cancellable_is_cancelled(start->cancellable);
	start->request->done(result, start->request->data);
	free_git_result(result);
	g_free(start->request);
	g_error_free(start->error);
	g_clear_object(&start->cancellable);
	g_free(start);
	return G_SOURCE_REMOVE;
}


void run_git(const char *folder, const char *const *arguments, const GitOptions *options, GitDone done,
	gpointer data) {
	GitOptions defaults = {0};
	if (!options)
		options = &defaults;
	g_autoptr(GSubprocessLauncher) launcher = create_launcher(folder, options->editor, options->is_quiet);
	g_autoptr(GPtrArray) command = build_git_command(arguments);
	GError *error = NULL;
	GSubprocess *process = g_subprocess_launcher_spawnv(launcher, (const char *const *)command->pdata, &error);
	RunRequest *request = g_new0(RunRequest, 1);
	request->done = done;
	request->data = data;
	if (!process) {
		FailedStart *start = g_new0(FailedStart, 1);
		start->request = request;
		start->error = error;
		start->cancellable = options->cancellable ? g_object_ref(options->cancellable) : NULL;
		g_idle_add(report_failed_start, start);
		return;
	}
	g_autoptr(GBytes) nothing = g_bytes_new_static("", 0);
	g_subprocess_communicate_async(process, options->input ? options->input : nothing, options->cancellable,
		finish_run, request);
}


GitResult *run_git_now(const char *folder, const char *const *arguments, GBytes *input) {
	g_autoptr(GSubprocessLauncher) launcher = create_launcher(folder, NULL, FALSE);
	g_autoptr(GPtrArray) command = build_git_command(arguments);
	g_autoptr(GError) error = NULL;
	g_autoptr(GSubprocess) process = g_subprocess_launcher_spawnv(launcher, (const char *const *)command->pdata,
		&error);
	if (!process)
		return create_failed_result(error);
	g_autoptr(GBytes) output = NULL;
	g_autoptr(GBytes) error_output = NULL;
	g_autoptr(GBytes) nothing = g_bytes_new_static("", 0);
	if (!g_subprocess_communicate(process, input ? input : nothing, NULL, &output, &error_output, &error))
		return create_failed_result(error);
	return create_result(process, output, error_output);
}


void free_git_result(GitResult *result) {
	if (!result)
		return;
	g_bytes_unref(result->output);
	g_free(result->error);
	g_free(result);
}


gboolean has_git_succeeded(const GitResult *result) {
	return result->exit_status == 0 && !result->is_cancelled;
}


char *get_git_output_text(const GitResult *result) {
	gsize length = 0;
	const char *text = g_bytes_get_data(result->output, &length);
	while (length > 0 && (text[length - 1] == '\n' || text[length - 1] == '\r'))
		length--;
	return g_strndup(text ? text : "", length);
}


/* The most telling line is usually the first fatal or error line, and otherwise the first line at all */
static const char *find_telling_line(char **lines) {
	for (char **line = lines; *line; line++)
		if (g_str_has_prefix(*line, ERROR_MARKS[0]) || g_str_has_prefix(*line, ERROR_MARKS[1]))
			return *line;
	for (char **line = lines; *line; line++)
		if (**line)
			return *line;
	return "";
}


char *get_git_error_summary(const GitResult *result) {
	g_auto(GStrv) lines = g_strsplit(result->error ? result->error : "", "\n", -1);
	const char *line = find_telling_line(lines);
	for (int index = 0; index < ERROR_MARKS_COUNT; index++)
		if (g_str_has_prefix(line, ERROR_MARKS[index]))
			line += strlen(ERROR_MARKS[index]);
	g_autofree char *summary = g_strstrip(g_strdup(line));
	if (*summary)
		summary[0] = g_ascii_toupper(summary[0]);
	return g_steal_pointer(&summary);
}


gboolean is_git_installed(void) {
	g_autofree char *path = g_find_program_in_path(GIT_PROGRAM);
	return path != NULL;
}
