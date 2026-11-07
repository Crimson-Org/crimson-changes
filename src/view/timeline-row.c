#include <adwaita.h>

#include "view/icons.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "graph/graph-view.h"
#include "view/format.h"
#include "view/chips.h"
#include "view/timeline-row.h"

#define ICON_SIZE 30
/* With the column's own small margin this makes the normal gap between a row's icon and its text */
#define ICON_TEXT_GAP SPACING_SMALL

/* The parts of a row, shown or hidden by kind */
typedef struct {
	/* The graph, or for rows without a commit an icon in a column as wide, so every row's text starts at one edge */
	GtkWidget *graph, *marker, *icon, *heading, *texts, *title, *details, *chips;
	/* The first row follows the repository itself, since the list does not draw a row again for the same item */
	Repository *repository;
	gulong changes_handler;
} RowParts;

#define ROW_CLASS "timeline-row"
static const char *const KIND_CLASSES[] = {"now", "stash", "heading", "commit"};


static RowParts *get_parts(GtkWidget *row) {
	return g_object_get_data(G_OBJECT(row), "parts");
}


static void stop_following(RowParts *parts) {
	if (!parts->repository)
		return;
	g_clear_signal_handler(&parts->changes_handler, parts->repository);
	g_clear_object(&parts->repository);
}


static void free_parts(gpointer data) {
	RowParts *parts = data;
	stop_following(parts);
	g_free(parts);
}


GtkWidget *create_timeline_row(void) {
	RowParts *parts = g_new0(RowParts, 1);
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(row, ROW_CLASS);
	g_object_set_data_full(G_OBJECT(row), "parts", parts, free_parts);
	parts->graph = create_graph_view();
	parts->icon = gtk_image_new();
	gtk_image_set_pixel_size(GTK_IMAGE(parts->icon), ICON_SIZE / 2);
	gtk_widget_set_size_request(parts->icon, ICON_SIZE, ICON_SIZE);
	/* The icon's centre sits on the first lane, where the dots of the commits below it are */
	gtk_widget_set_halign(parts->icon, GTK_ALIGN_START);
	gtk_widget_set_margin_start(parts->icon, get_first_lane_centre() - ICON_SIZE / 2);
	gtk_widget_set_valign(parts->icon, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(parts->icon, "row-icon");
	parts->marker = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_add_css_class(parts->marker, "row-marker");
	gtk_box_append(GTK_BOX(parts->marker), parts->icon);
	parts->heading = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(parts->heading), 0);
	gtk_widget_add_css_class(parts->heading, "timeline-heading");
	parts->title = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(parts->title), 0);
	gtk_label_set_ellipsize(GTK_LABEL(parts->title), PANGO_ELLIPSIZE_END);
	gtk_widget_add_css_class(parts->title, "row-title");
	parts->details = create_change_counts();
	gtk_widget_set_hexpand(parts->details, TRUE);
	gtk_widget_add_css_class(parts->details, "row-details");
	parts->chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	GtkWidget *detail_line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_box_append(GTK_BOX(detail_line), parts->chips);
	gtk_box_append(GTK_BOX(detail_line), parts->details);
	parts->texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_HAIRLINE);
	gtk_widget_set_valign(parts->texts, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(parts->texts, TRUE);
	gtk_box_append(GTK_BOX(parts->texts), parts->title);
	gtk_box_append(GTK_BOX(parts->texts), detail_line);
	gtk_box_append(GTK_BOX(row), parts->graph);
	gtk_box_append(GTK_BOX(row), parts->marker);
	gtk_box_append(GTK_BOX(row), parts->heading);
	gtk_box_append(GTK_BOX(row), parts->texts);
	return row;
}


GtkWidget *get_timeline_row_graph(GtkWidget *row) {
	return get_parts(row)->graph;
}


static void set_kind_class(GtkWidget *row, TimelineKind kind) {
	for (guint index = 0; index < G_N_ELEMENTS(KIND_CLASSES); index++)
		gtk_widget_remove_css_class(row, KIND_CLASSES[index]);
	gtk_widget_add_css_class(row, KIND_CLASSES[kind]);
}


/* The first row says at a glance whether anything is left to commit */
static void show_now(RowParts *parts, GtkWidget *row, Repository *repository) {
	const WorkChanges *changes = repository ? get_work_changes(repository) : NULL;
	Operation operation = repository ? get_repository_operation(repository) : OPERATION_NONE;
	gboolean is_clean = changes && is_status_clean(changes->status);
	gtk_widget_set_visible(parts->marker, TRUE);
	gtk_widget_set_visible(parts->texts, TRUE);
	gtk_widget_remove_css_class(row, "clean");
	gtk_widget_remove_css_class(row, "busy");
	gtk_widget_remove_css_class(row, "pending");
	if (operation != OPERATION_NONE) {
		gtk_image_set_from_icon_name(GTK_IMAGE(parts->icon), "dialog-warning-symbolic");
		gtk_label_set_text(GTK_LABEL(parts->title), get_operation_title(operation));
		gtk_widget_add_css_class(row, "busy");
	} else if (is_clean) {
		gtk_image_set_from_icon_name(GTK_IMAGE(parts->icon), "object-select-symbolic");
		gtk_label_set_text(GTK_LABEL(parts->title), TEXT_NOTHING_CHANGED);
		gtk_widget_add_css_class(row, "clean");
	} else {
		gtk_image_set_from_icon_name(GTK_IMAGE(parts->icon), "document-edit-symbolic");
		gtk_label_set_text(GTK_LABEL(parts->title), TEXT_NOT_COMMITTED);
		gtk_widget_add_css_class(row, "pending");
	}
	if (!changes) {
		set_change_counts(parts->details, TEXT_READING_CHANGES, 0, 0);
		return;
	}
	if (is_clean) {
		set_change_counts(parts->details, TEXT_ALL_COMMITTED, 0, 0);
		return;
	}
	g_autofree char *files = format_file_count(changes->status->files->len);
	set_change_counts(parts->details, files, changes->added, changes->removed);
}


static void bind_commit(RowParts *parts, TimelineItem *item, gboolean is_stash) {
	Commit *commit = item->commit;
	gtk_widget_set_visible(parts->texts, TRUE);
	gtk_widget_set_visible(parts->marker, is_stash);
	gtk_label_set_text(GTK_LABEL(parts->title), commit->subject);
	/* The day's heading already says which day, so a commit of an earlier day shows its time of day */
	g_autofree char *time = is_stash || get_day_number(commit->commit_time) == get_day_number(g_get_real_time() /
		G_USEC_PER_SEC) ? format_time_ago(commit->commit_time) : format_time_of_day(commit->commit_time);
	g_autofree char *details = is_stash ? g_strdup_printf(TEXT_STASH_DETAILS, time) :
		g_strdup_printf(TEXT_COMMIT_DETAILS, commit->author_name, time);
	set_change_counts(parts->details, details, 0, 0);
	g_autofree char *tooltip = format_full_time(commit->commit_time);
	gtk_widget_set_tooltip_text(parts->details, tooltip);
	if (is_stash)
		gtk_image_set_from_icon_name(GTK_IMAGE(parts->icon), ICON_STASH);
	fill_reference_chips(parts->chips, commit->references);
}


static void follow_changes(Repository *repository, GtkWidget *row) {
	show_now(get_parts(row), row, repository);
}


static void bind_now(RowParts *parts, GtkWidget *row, Repository *repository) {
	if (repository) {
		parts->repository = g_object_ref(repository);
		parts->changes_handler = g_signal_connect(repository, "changes-loaded", G_CALLBACK(follow_changes), row);
	}
	show_now(parts, row, repository);
}


/* The column before the text is as wide as the graph, but never so narrow that the icon of a row without a graph
 * would touch its text, and every row shares it so all text starts on one line */
static int measure_column_width(int graph_columns) {
	return MAX(measure_graph_width(graph_columns), get_first_lane_centre() + ICON_SIZE / 2 + ICON_TEXT_GAP);
}


void bind_timeline_row(GtkWidget *row, TimelineItem *item, Repository *repository, int graph_columns,
	gboolean shows_graph) {
	RowParts *parts = get_parts(row);
	stop_following(parts);
	set_kind_class(row, item->kind);
	gtk_widget_set_visible(parts->marker, FALSE);
	int column_width = measure_column_width(shows_graph ? graph_columns : 0);
	gtk_widget_set_size_request(parts->marker, column_width, -1);
	gtk_widget_set_size_request(parts->graph, column_width, -1);
	gtk_widget_set_visible(parts->heading, item->kind == TIMELINE_HEADING);
	gtk_widget_set_visible(parts->texts, FALSE);
	gtk_widget_set_tooltip_text(parts->details, NULL);
	clear_reference_chips(parts->chips);
	gboolean has_graph = shows_graph && item->graph;
	gtk_widget_set_visible(parts->graph, has_graph);
	if (has_graph) {
		set_graph_view_columns(parts->graph, graph_columns);
		gboolean is_current = FALSE;
		for (guint index = 0; item->commit && index < item->commit->references->len; index++)
			is_current |= ((CommitReference *)g_ptr_array_index(item->commit->references, index))->is_current;
		set_graph_view_row(parts->graph, item->graph, is_current, item->commit && is_merge_commit(item->commit));
	}
	switch (item->kind) {
	case TIMELINE_NOW:
		bind_now(parts, row, repository);
		break;
	case TIMELINE_STASH:
		bind_commit(parts, item, TRUE);
		break;
	case TIMELINE_HEADING:
		gtk_label_set_text(GTK_LABEL(parts->heading), item->heading);
		break;
	case TIMELINE_COMMIT:
		bind_commit(parts, item, FALSE);
		break;
	}
}


gboolean is_in_choosable_row(GtkWidget *widget) {
	while (widget && !gtk_widget_has_css_class(widget, ROW_CLASS))
		widget = gtk_widget_get_parent(widget);
	return widget && !gtk_widget_has_css_class(widget, KIND_CLASSES[TIMELINE_HEADING]);
}
