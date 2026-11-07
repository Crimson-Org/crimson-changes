#include <string.h>

#include "git/history.h"
#include "git/log.h"
#include "git/runner.h"

#define CHUNK_BYTES (64 * 1024)
/* Enough for a tall window, so the first view fills from the first batch */
#define FIRST_BATCH_COMMITS 400

struct HistoryStream {
	GSubprocess *process;
	GInputStream *output;
	GCancellable *cancellable;
	/* Bytes after the last complete commit, waiting for the rest of it */
	GByteArray *partial;
	CommitsRead read;
	gpointer data;
	guint count, wanted;
	gboolean is_reading, has_ended;
};

static void read_chunk(HistoryStream *stream);


/* Hands over every complete commit in what has been read so far */
static void take_commits(HistoryStream *stream) {
	const guint8 *end = memchr(stream->partial->data, 0, stream->partial->len);
	if (!end)
		return;
	/* The last NUL ends the last complete commit */
	gsize complete = 0;
	for (gsize index = stream->partial->len; index > 0; index--)
		if (stream->partial->data[index - 1] == 0) {
			complete = index;
			break;
		}
	GPtrArray *commits = parse_log((const char *)stream->partial->data, complete);
	g_byte_array_remove_range(stream->partial, 0, (guint)complete);
	stream->count += commits->len;
	if (commits->len > 0)
		stream->read(commits, stream->data);
	else
		g_ptr_array_unref(commits);
}


static void finish_chunk(GObject *source, GAsyncResult *result, gpointer data) {
	g_autoptr(GError) error = NULL;
	g_autoptr(GBytes) bytes = g_input_stream_read_bytes_finish(G_INPUT_STREAM(source), result, &error);
	if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
		return;
	HistoryStream *stream = data;
	stream->is_reading = FALSE;
	gsize length = bytes ? g_bytes_get_size(bytes) : 0;
	if (length == 0) {
		/* The last commit has no NUL after it when git ends the output */
		if (stream->partial->len > 0) {
			guint8 end = 0;
			g_byte_array_append(stream->partial, &end, 1);
			take_commits(stream);
		}
		stream->has_ended = TRUE;
		stream->read(NULL, stream->data);
		return;
	}
	g_byte_array_append(stream->partial, g_bytes_get_data(bytes, NULL), (guint)length);
	take_commits(stream);
	if (stream->count < stream->wanted)
		read_chunk(stream);
}


static void read_chunk(HistoryStream *stream) {
	if (stream->is_reading || stream->has_ended)
		return;
	stream->is_reading = TRUE;
	g_input_stream_read_bytes_async(stream->output, CHUNK_BYTES, G_PRIORITY_DEFAULT, stream->cancellable,
		finish_chunk, stream);
}


HistoryStream *start_history_stream(const char *folder, const char *const *extra_arguments, CommitsRead read,
	gpointer data) {
	HistoryStream *stream = g_new0(HistoryStream, 1);
	stream->read = read;
	stream->data = data;
	stream->cancellable = g_cancellable_new();
	stream->partial = g_byte_array_new();
	stream->wanted = FIRST_BATCH_COMMITS;
	g_autoptr(GPtrArray) arguments = g_ptr_array_new();
	const char *start[] = {"log", "-z", "--date-order", DECORATE_OPTION, LOG_FORMAT};
	for (guint index = 0; index < G_N_ELEMENTS(start); index++)
		g_ptr_array_add(arguments, (gpointer)start[index]);
	for (const char *const *argument = extra_arguments; argument && *argument; argument++)
		g_ptr_array_add(arguments, (gpointer)*argument);
	g_ptr_array_add(arguments, NULL);
	g_autoptr(GPtrArray) command = build_git_command((const char *const *)arguments->pdata);
	g_autoptr(GSubprocessLauncher) launcher = create_git_launcher(folder, G_SUBPROCESS_FLAGS_STDOUT_PIPE |
		G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, FALSE);
	g_autoptr(GError) error = NULL;
	stream->process = g_subprocess_launcher_spawnv(launcher, (const char *const *)command->pdata, &error);
	if (!stream->process) {
		g_warning("History could not be read: %s", error->message);
		stream->has_ended = TRUE;
		return stream;
	}
	stream->output = g_object_ref(g_subprocess_get_stdout_pipe(stream->process));
	read_chunk(stream);
	return stream;
}


void want_history_commits(HistoryStream *stream, guint count) {
	stream->wanted = MAX(stream->wanted, count);
	if (stream->count < stream->wanted)
		read_chunk(stream);
}


guint get_history_commit_count(HistoryStream *stream) {
	return stream->count;
}


gboolean has_history_ended(HistoryStream *stream) {
	return stream->has_ended;
}


void stop_history_stream(HistoryStream *stream) {
	if (!stream)
		return;
	g_cancellable_cancel(stream->cancellable);
	g_clear_object(&stream->cancellable);
	g_clear_object(&stream->output);
	/* A log still writing would wait on its full pipe forever */
	if (stream->process && !stream->has_ended)
		g_subprocess_force_exit(stream->process);
	g_clear_object(&stream->process);
	g_byte_array_unref(stream->partial);
	g_free(stream);
}
