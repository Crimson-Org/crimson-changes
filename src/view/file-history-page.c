#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "view/layout.h"
#include "view/page-scroller.h"
#include "git/records.h"
#include "git/diff.h"
#include "view/diff-view.h"
#include "view/format.h"
#include "view/side-page.h"
#include "view/file-history-page.h"

#define PAGE_KEY "history"
#define AVATAR_SIZE 32
/* A file changed in more commits than this lists the newest ones, which is what people look for */
#define MOST_COMMITS "2000"
#define RECORD_START '\x1e'
#define LINE_FIELDS 4

typedef struct {
	Window *window;
	char *path;
} HistoryPage;


static void free_history_page(gpointer data) {
	HistoryPage *page = data;
	g_free(page->path);
	g_free(page);
}


/* A commit's avatar, message, author and time, which opens the commit when clicked */
static GtkWidget *create_commit_line(const char *hash, const char *author, gint64 time, const char *subject) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_add_css_class(row, "history-line");
	gtk_box_append(GTK_BOX(row), adw_avatar_new(AVATAR_SIZE, author, TRUE));
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_HAIRLINE);
	gtk_widget_set_hexpand(texts, TRUE);
	gtk_widget_set_valign(texts, GTK_ALIGN_CENTER);
	GtkWidget *title = gtk_label_new(subject);
	gtk_label_set_xalign(GTK_LABEL(title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(title, "row-title");
	g_autofree char *ago = format_time_ago(time);
	g_autofree char *details_text = g_strdup_printf(TEXT_COMMIT_DETAILS, author, ago);
	GtkWidget *details = gtk_label_new(details_text);
	gtk_label_set_xalign(GTK_LABEL(details), 0);
	gtk_label_set_ellipsize(GTK_LABEL(details), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(details, "row-details");
	gtk_box_append(GTK_BOX(texts), title);
	gtk_box_append(GTK_BOX(texts), details);
	gtk_box_append(GTK_BOX(row), texts);
	g_autofree char *short_hash = get_short_hash(hash);
	GtkWidget *hash_label = gtk_label_new(short_hash);
	gtk_widget_add_css_class(hash_label, "monospace");
	gtk_widget_add_css_class(hash_label, "dim-label");
	gtk_box_append(GTK_BOX(row), hash_label);
	g_object_set_data_full(G_OBJECT(row), "hash", g_strdup(hash), g_free);
	return row;
}


static GtkWidget *wrap_in_scroller(GtkWidget *content) {
	return create_page_scroller(content);
}


/* A whole file's history */

static void open_row(GtkListBox *list, GtkListBoxRow *row, HistoryPage *page) {
	const char *hash = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "hash");
	if (hash)
		go_to_commit(page->window, hash);
}


static void read_file_history(GitResult *result, gpointer data) {
	AdwNavigationPage *navigation_page = data;
	HistoryPage *page = g_object_get_data(G_OBJECT(navigation_page), PAGE_KEY);
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(page->window, TEXT_HISTORY_FAILED, result);
	if (result->is_cancelled || !has_git_succeeded(result)) {
		g_object_unref(navigation_page);
		return;
	}
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	g_autoptr(GPtrArray) commits = parse_log(output ? output : "", length);
	GtkWidget *list = gtk_list_box_new();
	gtk_widget_add_css_class(list, "boxed-list");
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	g_signal_connect(list, "row-activated", G_CALLBACK(open_row), page);
	for (guint index = 0; index < commits->len; index++) {
		Commit *commit = g_ptr_array_index(commits, index);
		GtkWidget *line = create_commit_line(commit->hash, commit->author_name, commit->commit_time, commit->subject);
		gtk_list_box_append(GTK_LIST_BOX(list), line);
		gtk_list_box_row_set_activatable(gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), (int)index), TRUE);
	}
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "page-content");
	gtk_box_append(GTK_BOX(box), list);
	set_side_page_content(navigation_page, wrap_in_scroller(box));
	g_object_unref(navigation_page);
}


static AdwNavigationPage *create_history_page(Window *window, const char *path, const char *subtitle) {
	HistoryPage *page = g_new0(HistoryPage, 1);
	page->window = window;
	page->path = g_strdup(path);
	AdwNavigationPage *navigation_page = create_side_page(get_path_name(path), subtitle, NULL, NULL);
	g_object_set_data_full(G_OBJECT(navigation_page), PAGE_KEY, page, free_history_page);
	push_window_page(window, navigation_page);
	return navigation_page;
}


void show_file_history_page(Window *window, const char *path) {
	AdwNavigationPage *page = create_history_page(window, path, TEXT_FILE_HISTORY);
	const char *arguments[] = {"log", "-z", "--follow", "-n", MOST_COMMITS, LOG_FORMAT, "--", path, NULL};
	read_repository(get_window_repository(window), arguments, read_file_history, g_object_ref(page));
}


/* The history of a few lines, each commit with how it changed them */

static void click_commit_line(GtkGestureClick *gesture, int presses, double x, double y, HistoryPage *page) {
	GtkWidget *line = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(gesture));
	go_to_commit(page->window, g_object_get_data(G_OBJECT(line), "hash"));
}


/* Each record starts with the commit's fields on one line, followed by its diff */
static GtkWidget *create_line_change(HistoryPage *page, const char *record, gsize length) {
	const char *end = memchr(record, '\n', length);
	g_autofree char *header = g_strndup(record, end ? (gsize)(end - record) : length);
	g_auto(GStrv) fields = g_strsplit(header, FIELD_SEPARATOR, LINE_FIELDS);
	if (g_strv_length(fields) != LINE_FIELDS)
		return NULL;
	GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(card, "file-card");
	gtk_widget_add_css_class(card, "expanded");
	GtkWidget *line = create_commit_line(fields[0], fields[1], g_ascii_strtoll(fields[2], NULL, 10), fields[3]);
	gtk_widget_add_css_class(line, "card-header");
	gtk_widget_set_cursor_from_name(line, "pointer");
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(click_commit_line), page);
	gtk_widget_add_controller(line, GTK_EVENT_CONTROLLER(click));
	gtk_box_append(GTK_BOX(card), line);
	if (end) {
		g_autoptr(GPtrArray) files = parse_diff(end + 1, length - (gsize)(end + 1 - record));
		for (guint file_index = 0; file_index < files->len; file_index++) {
			DiffFile *file = g_ptr_array_index(files, file_index);
			for (guint index = 0; index < file->hunks->len; index++)
				gtk_box_append(GTK_BOX(card), create_hunk_view(file, g_ptr_array_index(file->hunks, index)));
		}
	}
	return card;
}


static void read_line_history(GitResult *result, gpointer data) {
	AdwNavigationPage *navigation_page = data;
	HistoryPage *page = g_object_get_data(G_OBJECT(navigation_page), PAGE_KEY);
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(page->window, TEXT_HISTORY_FAILED, result);
	if (result->is_cancelled || !has_git_succeeded(result)) {
		g_object_unref(navigation_page);
		return;
	}
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "page-content");
	gsize position = 0;
	while (position < length) {
		const char *start = memchr(output + position, RECORD_START, length - position);
		if (!start)
			break;
		start++;
		const char *next = memchr(start, RECORD_START, length - (gsize)(start - output));
		gsize record_length = next ? (gsize)(next - start) : length - (gsize)(start - output);
		GtkWidget *card = create_line_change(page, start, record_length);
		if (card)
			gtk_box_append(GTK_BOX(box), card);
		position = (gsize)(start - output) + record_length;
	}
	set_side_page_content(navigation_page, wrap_in_scroller(box));
	g_object_unref(navigation_page);
}


void show_line_history_page(Window *window, const char *path, const char *revision, int first_line, int last_line) {
	g_autofree char *subtitle = first_line == last_line ? g_strdup_printf(TEXT_LINE_SINGLE, first_line) :
		g_strdup_printf(TEXT_LINE_RANGE, first_line, last_line);
	AdwNavigationPage *page = create_history_page(window, path, subtitle);
	g_autofree char *range = g_strdup_printf("-L%d,%d:%s", first_line, last_line, path);
	const char *arguments[] = {"log", "--format=%x1e%H%x1f%an%x1f%ct%x1f%s", "--no-ext-diff", range,
		revision ? revision : "HEAD", NULL};
	read_repository(get_window_repository(window), arguments, read_line_history, g_object_ref(page));
}
