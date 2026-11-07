#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "lang/en.h"
#include "git/blame.h"
#include "view/page-scroller.h"
#include "view/diff-view.h"
#include "view/colours.h"
#include "view/format.h"
#include "view/side-page.h"
#include "view/file-history-page.h"
#include "view/blame-page.h"

#define PAGE_KEY "blame"
#define GUTTER_PADDING 8
#define LONGEST_NAME 16
#define AGE_MARK "▌"
/* Recent lines glow Crimson red, and the oldest fade to grey, so fresh work stands out */
#define OLDEST_SHADE (120 / 255.0)
#define COLOUR_SCALE 255

typedef struct {
	Window *window;
	char *path, *revision;
	Blame *blame;
	GtkWidget *view, *lines_button;
	AdwNavigationPage *page;
	gboolean is_file_changed;
} BlamePage;


static void free_blame_page(gpointer data) {
	BlamePage *page = data;
	g_free(page->path);
	g_free(page->revision);
	free_blame(page->blame);
	g_free(page);
}


static const BlameCommit *get_line_commit(BlamePage *page, guint line) {
	if (!page->blame || line >= page->blame->line_commits->len)
		return NULL;
	return g_ptr_array_index(page->blame->commits, g_array_index(page->blame->line_commits, guint, line));
}


/* A colour between Crimson red for the newest lines and grey for the oldest */
static char *get_age_colour(BlamePage *page, const BlameCommit *commit) {
	double span = (double)MAX(page->blame->newest_time - page->blame->oldest_time, 1);
	double age = commit->is_uncommitted ? 0 : (double)(page->blame->newest_time - commit->time) / span;
	GdkRGBA newest = get_colour_with_alpha(CRIMSON_COLOUR, 1);
	int red = (int)((newest.red + (OLDEST_SHADE - newest.red) * age) * COLOUR_SCALE);
	int green = (int)((newest.green + (OLDEST_SHADE - newest.green) * age) * COLOUR_SCALE);
	int blue = (int)((newest.blue + (OLDEST_SHADE - newest.blue) * age) * COLOUR_SCALE);
	return g_strdup_printf("#%02x%02x%02x", red, green, blue);
}


/* Each line gets a mark in its age's colour, and the first line of each change names who made it and when */
static void show_blame_line(GtkSourceGutterRenderer *renderer, GObject *lines, guint line, BlamePage *page) {
	const BlameCommit *commit = get_line_commit(page, line);
	if (!commit) {
		gtk_source_gutter_renderer_text_set_text(GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), "", -1);
		return;
	}
	g_autofree char *colour = get_age_colour(page, commit);
	gboolean is_start = line == 0 || get_line_commit(page, line - 1) != commit;
	g_autofree char *who = NULL;
	if (is_start && commit->is_uncommitted)
		who = g_strdup(TEXT_NOT_COMMITTED_YET);
	else if (is_start) {
		g_autofree char *ago = format_time_ago(commit->time);
		g_autofree char *name = g_utf8_substring(commit->author ? commit->author : "", 0, LONGEST_NAME);
		who = g_strdup_printf(TEXT_BLAME_LINE, name, ago);
	}
	g_autofree char *markup = g_markup_printf_escaped("<span foreground=\"%s\">" AGE_MARK "</span> %s", colour,
		who ? who : "");
	gtk_source_gutter_renderer_text_set_markup(GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), markup, -1);
}


static gboolean can_open_line(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area,
	BlamePage *page) {
	const BlameCommit *commit = get_line_commit(page, (guint)gtk_text_iter_get_line(iter));
	return commit && !commit->is_uncommitted;
}


static void open_line(GtkSourceGutterRenderer *renderer, GtkTextIter *iter, GdkRectangle *area, guint button,
	GdkModifierType state, int presses, BlamePage *page) {
	const BlameCommit *commit = get_line_commit(page, (guint)gtk_text_iter_get_line(iter));
	if (commit && !commit->is_uncommitted)
		go_to_commit(page->window, commit->hash);
}


/* Hovering a line names the commit that last changed it, who made it and when */
static gboolean explain_line(GtkWidget *renderer, int x, int y, gboolean is_keyboard, GtkTooltip *tooltip,
	BlamePage *page) {
	graphene_point_t point = GRAPHENE_POINT_INIT(x, y), view_point;
	if (!gtk_widget_compute_point(renderer, page->view, &point, &view_point))
		return FALSE;
	int buffer_x = 0, buffer_y = 0;
	gtk_text_view_window_to_buffer_coords(GTK_TEXT_VIEW(page->view), GTK_TEXT_WINDOW_WIDGET, (int)view_point.x,
		(int)view_point.y, &buffer_x, &buffer_y);
	GtkTextIter iter;
	gtk_text_view_get_line_at_y(GTK_TEXT_VIEW(page->view), &iter, buffer_y, NULL);
	const BlameCommit *commit = get_line_commit(page, (guint)gtk_text_iter_get_line(&iter));
	if (!commit)
		return FALSE;
	if (commit->is_uncommitted) {
		gtk_tooltip_set_text(tooltip, TEXT_NOT_COMMITTED_YET);
		return TRUE;
	}
	g_autofree char *time = format_full_time(commit->time);
	g_autofree char *text = g_strdup_printf(TEXT_BLAME_TOOLTIP, commit->summary ? commit->summary : "",
		commit->author ? commit->author : "", time);
	gtk_tooltip_set_text(tooltip, text);
	return TRUE;
}


static void add_blame_gutter(BlamePage *page) {
	GtkSourceGutterRenderer *renderer = gtk_source_gutter_renderer_text_new();
	gtk_source_gutter_renderer_set_xalign(renderer, 0);
	gtk_source_gutter_renderer_set_xpad(renderer, GUTTER_PADDING);
	gtk_widget_add_css_class(GTK_WIDGET(renderer), "blame-gutter");
	gtk_widget_set_has_tooltip(GTK_WIDGET(renderer), TRUE);
	g_signal_connect(renderer, "query-tooltip", G_CALLBACK(explain_line), page);
	g_signal_connect(renderer, "query-data", G_CALLBACK(show_blame_line), page);
	g_signal_connect(renderer, "query-activatable", G_CALLBACK(can_open_line), page);
	g_signal_connect(renderer, "activate", G_CALLBACK(open_line), page);
	GtkSourceGutter *gutter = gtk_source_view_get_gutter(GTK_SOURCE_VIEW(page->view), GTK_TEXT_WINDOW_LEFT);
	gtk_source_gutter_insert(gutter, renderer, 0);
}


static void follow_selection(GtkTextBuffer *buffer, GParamSpec *spec, BlamePage *page) {
	gtk_widget_set_sensitive(page->lines_button, gtk_text_buffer_get_has_selection(buffer) && !page->is_file_changed);
}


/* The history of lines follows them through the commits, so a file with changes not committed yet has line numbers
 * the commits do not know */
static gboolean is_file_changed(Window *window, const char *path) {
	const WorkChanges *changes = get_work_changes(get_window_repository(window));
	for (guint index = 0; changes && index < changes->status->files->len; index++)
		if (g_strcmp0(((StatusFile *)g_ptr_array_index(changes->status->files, index))->path, path) == 0)
			return TRUE;
	return FALSE;
}


static void show_line_history(GtkButton *button, BlamePage *page) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->view));
	GtkTextIter start, end;
	if (!gtk_text_buffer_get_selection_bounds(buffer, &start, &end))
		return;
	/* A selection that ends at the start of a line does not take that line */
	if (gtk_text_iter_starts_line(&end) && gtk_text_iter_get_line(&end) > gtk_text_iter_get_line(&start))
		gtk_text_iter_backward_line(&end);
	show_line_history_page(page->window, page->path, page->revision, gtk_text_iter_get_line(&start) + 1,
		gtk_text_iter_get_line(&end) + 1);
}


static void read_blame(GitResult *result, gpointer data) {
	AdwNavigationPage *navigation_page = data;
	BlamePage *page = g_object_get_data(G_OBJECT(navigation_page), PAGE_KEY);
	if (result->is_cancelled || !page) {
		g_object_unref(navigation_page);
		return;
	}
	if (!has_git_succeeded(result)) {
		show_git_failure(page->window, TEXT_BLAME_FAILED, result);
		g_object_unref(navigation_page);
		return;
	}
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	page->blame = parse_blame(output ? output : "", length);
	page->view = create_code_view(page->path, page->blame->text->str, 1);
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(page->view), GTK_WRAP_NONE);
	gtk_widget_add_css_class(page->view, "blame-view");
	add_blame_gutter(page);
	g_signal_connect(gtk_text_view_get_buffer(GTK_TEXT_VIEW(page->view)), "notify::has-selection",
		G_CALLBACK(follow_selection), page);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), page->view);
	set_side_page_content(navigation_page, add_top_button(GTK_SCROLLED_WINDOW(scroller)));
	g_object_unref(navigation_page);
}


void show_blame_page(Window *window, const char *path, const char *revision) {
	BlamePage *page = g_new0(BlamePage, 1);
	page->window = window;
	page->path = g_strdup(path);
	page->revision = g_strdup(revision);
	page->is_file_changed = !revision && is_file_changed(window, path);
	page->lines_button = gtk_button_new_with_label(TEXT_LINE_HISTORY);
	gtk_widget_set_tooltip_text(page->lines_button, page->is_file_changed ? TEXT_LINE_HISTORY_CHANGED :
		TEXT_LINE_HISTORY_TOOLTIP);
	gtk_widget_set_sensitive(page->lines_button, FALSE);
	g_signal_connect(page->lines_button, "clicked", G_CALLBACK(show_line_history), page);
	g_autofree char *subtitle = NULL;
	if (revision) {
		g_autofree char *short_hash = get_short_hash(revision);
		subtitle = g_strdup_printf(TEXT_BLAME_AT, short_hash);
	} else
		subtitle = g_strdup(TEXT_BLAME_NOW);
	page->page = create_side_page(get_path_name(path), subtitle, NULL, page->lines_button);
	g_object_set_data_full(G_OBJECT(page->page), PAGE_KEY, page, free_blame_page);
	push_window_page(window, page->page);
	/* Moved and copied lines keep their first author, and whitespace changes do not take a line over */
	g_autoptr(GPtrArray) arguments = g_ptr_array_new();
	const char *start[] = {"blame", "--porcelain", "-w", "-M", "-C"};
	for (guint index = 0; index < G_N_ELEMENTS(start); index++)
		g_ptr_array_add(arguments, (gpointer)start[index]);
	if (revision)
		g_ptr_array_add(arguments, (gpointer)revision);
	g_ptr_array_add(arguments, (gpointer)"--");
	g_ptr_array_add(arguments, (gpointer)path);
	g_ptr_array_add(arguments, NULL);
	read_repository(get_window_repository(window), (const char *const *)arguments->pdata, read_blame,
		g_object_ref(page->page));
}
