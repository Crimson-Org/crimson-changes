#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "common/widgets/smooth-scroll.h"
#include "app/settings.h"
#include "git/history.h"
#include "graph/graph-view.h"
#include "view/page-scroller.h"
#include "view/format.h"
#include "view/timeline-row.h"
#include "view/timeline.h"

/* More history is read once the view is this many screens from the end */
#define SCREENS_AHEAD 3
#define PAGE_COMMITS 600
/* A search keeps reading until it found this many commits, so the list fills the view */
#define SEARCH_FILL 200
/* A commit asked for that is not found within this much history is shown on its own instead */
#define FARTHEST_SEARCH 20000

struct Timeline {
	TimelineChosen chosen;
	TimelinePressed pressed;
	CommitMissing missing;
	gpointer data;
	Repository *repository;
	gulong changes_handler, history_handler;
	GtkWidget *box, *list, *scroller, *search_bar, *search_entry, *code_toggle;
	GListStore *store;
	GtkSingleSelection *selection;
	HistoryStream *stream;
	LaneLayout *layout;
	/* Items read since the reload started, shown together once the first batch is in, so the list never empties */
	GPtrArray *waiting_items;
	gboolean has_replaced;
	int last_day;
	int graph_columns;
	/* The rows the list made, whose graphs widen together */
	GPtrArray *rows;
	char *wanted_hash;
	char *selected_hash;
	/* The commit that was selected before a reload, looked for in each batch until found */
	char *restoring_hash;
	/* The selection is being set again after a reload, which is not the user's choice */
	gboolean is_restoring;
	/* Commits shown in a search, which reads on until the view is full */
	guint match_count;
};


static gboolean is_searching(Timeline *timeline) {
	return *gtk_editable_get_text(GTK_EDITABLE(timeline->search_entry)) != 0;
}


static gboolean is_searching_code(Timeline *timeline) {
	return is_searching(timeline) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(timeline->code_toggle));
}


static gboolean contains_folded(const char *text, const char *folded_search) {
	g_autofree char *folded = g_utf8_casefold(text ? text : "", -1);
	return strstr(folded, folded_search) != NULL;
}


/* Git can only match messages and authors together, so a search for either one is matched here, along with the
 * start of a hash. Code searches are matched by git itself */
static gboolean matches_search(Timeline *timeline, const Commit *commit) {
	if (!is_searching(timeline) || is_searching_code(timeline))
		return TRUE;
	g_autofree char *search = g_utf8_casefold(gtk_editable_get_text(GTK_EDITABLE(timeline->search_entry)), -1);
	return contains_folded(commit->subject, search) || contains_folded(commit->author_name, search) ||
		contains_folded(commit->author_email, search) || g_str_has_prefix(commit->hash, search);
}


/* A row leaves the list of graphs to widen whenever the list frees it, however that happens */
static void forget_row(gpointer data, GObject *row) {
	Timeline *timeline = data;
	g_ptr_array_remove_fast(timeline->rows, row);
}


static void setup_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, Timeline *timeline) {
	GtkWidget *row = create_timeline_row();
	gtk_list_item_set_child(list_item, row);
	g_ptr_array_add(timeline->rows, row);
	g_object_weak_ref(G_OBJECT(row), forget_row, timeline);
}


static void bind_row(GtkSignalListItemFactory *factory, GtkListItem *list_item, Timeline *timeline) {
	TimelineItem *item = gtk_list_item_get_item(list_item);
	gboolean is_heading = item->kind == TIMELINE_HEADING;
	gtk_list_item_set_selectable(list_item, !is_heading);
	gtk_list_item_set_activatable(list_item, !is_heading);
	gtk_list_item_set_focusable(list_item, !is_heading);
	bind_timeline_row(gtk_list_item_get_child(list_item), item, timeline->repository, timeline->graph_columns,
		!is_searching(timeline));
}


static void widen_graphs(Timeline *timeline, int columns) {
	if (columns <= timeline->graph_columns)
		return;
	timeline->graph_columns = columns;
	for (guint index = 0; index < timeline->rows->len; index++)
		set_graph_view_columns(get_timeline_row_graph(g_ptr_array_index(timeline->rows, index)), columns);
}


static TimelineItem *get_item(Timeline *timeline, guint position) {
	return g_list_model_get_item(G_LIST_MODEL(timeline->store), position);
}


static gboolean select_hash(Timeline *timeline, const char *hash) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(timeline->store));
	for (guint position = 0; position < count; position++) {
		g_autoptr(TimelineItem) item = get_item(timeline, position);
		if (item->commit && strcmp(item->commit->hash, hash) == 0) {
			gtk_list_view_scroll_to(GTK_LIST_VIEW(timeline->list), position,
				GTK_LIST_SCROLL_SELECT | GTK_LIST_SCROLL_FOCUS, NULL);
			return TRUE;
		}
	}
	return FALSE;
}


/* A commit asked for while history is still on its way is looked for in each new batch */
static void look_for_wanted(Timeline *timeline) {
	if (!timeline->wanted_hash || !timeline->has_replaced)
		return;
	if (select_hash(timeline, timeline->wanted_hash)) {
		g_clear_pointer(&timeline->wanted_hash, g_free);
		return;
	}
	if (!timeline->stream || has_history_ended(timeline->stream) ||
		get_history_commit_count(timeline->stream) >= FARTHEST_SEARCH) {
		g_autofree char *hash = g_steal_pointer(&timeline->wanted_hash);
		timeline->missing(hash, timeline->data);
		return;
	}
	want_history_commits(timeline->stream, get_history_commit_count(timeline->stream) + PAGE_COMMITS);
}


/* Each commit gets its row of the graph, with a heading above the first commit of each day */
static void add_commits(Timeline *timeline, GPtrArray *commits, GPtrArray *items) {
	for (guint index = 0; index < commits->len; index++) {
		Commit *commit = g_ptr_array_index(commits, index);
		if (!matches_search(timeline, commit))
			continue;
		commits->pdata[index] = NULL;
		timeline->match_count++;
		int day = get_day_number(commit->commit_time);
		if (day != timeline->last_day) {
			timeline->last_day = day;
			GraphRow *passing = is_searching(timeline) ? NULL : create_passing_row(timeline->layout);
			g_ptr_array_add(items, create_heading_item(format_day_heading(commit->commit_time), passing));
		}
		GraphRow *row = is_searching(timeline) ? NULL : add_graph_row(timeline->layout, commit->hash,
			(const char *const *)commit->parents);
		if (row)
			widen_graphs(timeline, row->width);
		g_ptr_array_add(items, create_commit_item(commit, row));
	}
}


/* A commit selected far down is found again as the history comes back, a batch at a time */
static void restore_selection(Timeline *timeline) {
	timeline->is_restoring = TRUE;
	g_clear_pointer(&timeline->restoring_hash, g_free);
	if (!timeline->selected_hash)
		gtk_single_selection_set_selected(timeline->selection, 0);
	else if (!select_hash(timeline, timeline->selected_hash)) {
		gtk_single_selection_set_selected(timeline->selection, GTK_INVALID_LIST_POSITION);
		timeline->restoring_hash = g_strdup(timeline->selected_hash);
	}
	timeline->is_restoring = FALSE;
}


static void look_for_restored(Timeline *timeline) {
	if (!timeline->restoring_hash)
		return;
	timeline->is_restoring = TRUE;
	gboolean is_found = select_hash(timeline, timeline->restoring_hash);
	timeline->is_restoring = FALSE;
	gboolean is_done = is_found || !timeline->stream || has_history_ended(timeline->stream) ||
		get_history_commit_count(timeline->stream) >= FARTHEST_SEARCH;
	if (is_done)
		g_clear_pointer(&timeline->restoring_hash, g_free);
	else
		want_history_commits(timeline->stream, get_history_commit_count(timeline->stream) + PAGE_COMMITS);
}


/* The new list replaces the old one in one step once its first batch is in, so the view never flashes empty */
static void show_waiting_items(Timeline *timeline) {
	guint count = g_list_model_get_n_items(G_LIST_MODEL(timeline->store));
	g_list_store_splice(timeline->store, 0, count, timeline->waiting_items->pdata, timeline->waiting_items->len);
	g_ptr_array_set_size(timeline->waiting_items, 0);
	timeline->has_replaced = TRUE;
	restore_selection(timeline);
}


static void read_commits(GPtrArray *commits, gpointer data) {
	Timeline *timeline = data;
	if (commits) {
		g_autoptr(GPtrArray) owned = commits;
		if (timeline->has_replaced) {
			g_autoptr(GPtrArray) items = g_ptr_array_new_with_free_func(g_object_unref);
			add_commits(timeline, owned, items);
			g_list_store_splice(timeline->store, g_list_model_get_n_items(G_LIST_MODEL(timeline->store)), 0,
				items->pdata, items->len);
		} else
			add_commits(timeline, owned, timeline->waiting_items);
	}
	if (!timeline->has_replaced)
		show_waiting_items(timeline);
	if (commits && is_searching(timeline) && timeline->match_count < SEARCH_FILL)
		want_history_commits(timeline->stream, get_history_commit_count(timeline->stream) + PAGE_COMMITS);
	look_for_restored(timeline);
	look_for_wanted(timeline);
}


static void add_history_arguments(Timeline *timeline, GPtrArray *arguments) {
	if (is_searching_code(timeline))
		g_ptr_array_add(arguments, g_strdup_printf("-S%s", gtk_editable_get_text(GTK_EDITABLE(timeline->search_entry))));
	if (get_settings()->shows_all_branches) {
		g_ptr_array_add(arguments, g_strdup("--branches"));
		g_ptr_array_add(arguments, g_strdup("--remotes"));
		g_ptr_array_add(arguments, g_strdup("--tags"));
	}
	g_ptr_array_add(arguments, g_strdup("HEAD"));
	g_ptr_array_add(arguments, g_strdup("--"));
	g_ptr_array_add(arguments, NULL);
}


void reload_timeline_history(Timeline *timeline) {
	if (!timeline->repository)
		return;
	g_clear_pointer(&timeline->stream, stop_history_stream);
	g_clear_pointer(&timeline->layout, free_lane_layout);
	g_ptr_array_set_size(timeline->waiting_items, 0);
	timeline->layout = create_lane_layout();
	timeline->has_replaced = FALSE;
	timeline->last_day = 0;
	timeline->match_count = 0;
	timeline->graph_columns = 1;
	g_ptr_array_add(timeline->waiting_items, create_now_item());
	GPtrArray *stashes = get_repository_stashes(timeline->repository);
	for (guint index = 0; index < stashes->len && !is_searching(timeline); index++)
		g_ptr_array_add(timeline->waiting_items, create_stash_item(g_ptr_array_index(stashes, index)));
	if (!has_repository_commits(timeline->repository)) {
		show_waiting_items(timeline);
		return;
	}
	g_autoptr(GPtrArray) arguments = g_ptr_array_new_with_free_func(g_free);
	add_history_arguments(timeline, arguments);
	timeline->stream = start_history_stream(get_repository_folder(timeline->repository),
		(const char *const *)arguments->pdata, read_commits, timeline);
}


static void choose_row(GtkSingleSelection *selection, GParamSpec *spec, Timeline *timeline) {
	TimelineItem *item = gtk_single_selection_get_selected_item(selection);
	if (!item)
		return;
	g_set_str(&timeline->selected_hash, item->kind == TIMELINE_NOW ? NULL : item->commit->hash);
	timeline->chosen(item, !timeline->is_restoring, timeline->data);
}


/* The first read of a new repository tells whether it has commits at all, and the first row follows the changes
 * on its own */
static void update_now_row(Repository *repository, Timeline *timeline) {
	if (!timeline->stream && has_repository_commits(repository))
		reload_timeline_history(timeline);
}


static void follow_history(Repository *repository, Timeline *timeline) {
	reload_timeline_history(timeline);
}


/* Reading on as the end comes near, so scrolling never meets an end that is not the real one */
static void read_further(GtkAdjustment *adjustment, Timeline *timeline) {
	if (!timeline->stream || has_history_ended(timeline->stream))
		return;
	double left = gtk_adjustment_get_upper(adjustment) - gtk_adjustment_get_value(adjustment);
	if (left < gtk_adjustment_get_page_size(adjustment) * SCREENS_AHEAD)
		want_history_commits(timeline->stream, get_history_commit_count(timeline->stream) + PAGE_COMMITS);
}


static void change_search(Timeline *timeline) {
	reload_timeline_history(timeline);
}


static GtkWidget *create_search_bar(Timeline *timeline) {
	timeline->search_entry = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(timeline->search_entry), TEXT_SEARCH_HISTORY);
	gtk_widget_set_hexpand(timeline->search_entry, TRUE);
	g_signal_connect_swapped(timeline->search_entry, "search-changed", G_CALLBACK(change_search), timeline);
	timeline->code_toggle = gtk_toggle_button_new_with_label(TEXT_SEARCH_CODE);
	gtk_widget_set_tooltip_text(timeline->code_toggle, TEXT_SEARCH_CODE_TOOLTIP);
	g_signal_connect_swapped(timeline->code_toggle, "toggled", G_CALLBACK(change_search), timeline);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_box_append(GTK_BOX(box), timeline->search_entry);
	gtk_box_append(GTK_BOX(box), timeline->code_toggle);
	timeline->search_bar = gtk_search_bar_new();
	gtk_search_bar_set_child(GTK_SEARCH_BAR(timeline->search_bar), box);
	gtk_search_bar_connect_entry(GTK_SEARCH_BAR(timeline->search_bar), GTK_EDITABLE(timeline->search_entry));
	gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(timeline->search_bar), FALSE);
	return timeline->search_bar;
}


/* A click lands on a row once the list has handled it, so the choice has already changed when this runs. A day's
 * heading and the space below the last row choose nothing, so a click there does nothing */
static void press_row(GtkGestureClick *gesture, int presses, double x, double y, Timeline *timeline) {
	if (is_in_choosable_row(gtk_widget_pick(timeline->list, x, y, GTK_PICK_DEFAULT)))
		timeline->pressed(timeline->data);
}


Timeline *create_timeline(TimelineChosen chosen, TimelinePressed pressed, CommitMissing missing, gpointer data) {
	Timeline *timeline = g_new0(Timeline, 1);
	timeline->chosen = chosen;
	timeline->pressed = pressed;
	timeline->missing = missing;
	timeline->data = data;
	timeline->rows = g_ptr_array_new();
	timeline->waiting_items = g_ptr_array_new_with_free_func(g_object_unref);
	timeline->graph_columns = 1;
	timeline->store = g_list_store_new(TYPE_TIMELINE_ITEM);
	timeline->selection = gtk_single_selection_new(G_LIST_MODEL(g_object_ref(timeline->store)));
	gtk_single_selection_set_autoselect(timeline->selection, FALSE);
	gtk_single_selection_set_can_unselect(timeline->selection, TRUE);
	g_signal_connect(timeline->selection, "notify::selected-item", G_CALLBACK(choose_row), timeline);
	GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
	g_signal_connect(factory, "setup", G_CALLBACK(setup_row), timeline);
	g_signal_connect(factory, "bind", G_CALLBACK(bind_row), timeline);
	timeline->list = gtk_list_view_new(GTK_SELECTION_MODEL(g_object_ref(timeline->selection)), factory);
	gtk_widget_add_css_class(timeline->list, "timeline");
	gtk_widget_add_css_class(timeline->list, "navigation-sidebar");
	GtkGesture *click = gtk_gesture_click_new();
	gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(click), GDK_BUTTON_PRIMARY);
	g_signal_connect(click, "released", G_CALLBACK(press_row), timeline);
	gtk_widget_add_controller(timeline->list, GTK_EVENT_CONTROLLER(click));
	timeline->scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(timeline->scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(timeline->scroller), timeline->list);
	gtk_widget_set_vexpand(timeline->scroller, TRUE);
	add_smooth_scrolling(GTK_SCROLLED_WINDOW(timeline->scroller));
	GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(timeline->scroller));
	g_signal_connect(adjustment, "value-changed", G_CALLBACK(read_further), timeline);
	g_signal_connect(adjustment, "changed", G_CALLBACK(read_further), timeline);
	timeline->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(timeline->box), add_top_button(GTK_SCROLLED_WINDOW(timeline->scroller)));
	create_search_bar(timeline);
	return timeline;
}


GtkWidget *get_timeline_widget(Timeline *timeline) {
	return timeline->box;
}


GtkWidget *get_timeline_search_bar(Timeline *timeline) {
	return timeline->search_bar;
}


static void disconnect_repository(Timeline *timeline) {
	if (!timeline->repository)
		return;
	g_clear_signal_handler(&timeline->changes_handler, timeline->repository);
	g_clear_signal_handler(&timeline->history_handler, timeline->repository);
	g_clear_object(&timeline->repository);
}


void set_timeline_repository(Timeline *timeline, Repository *repository) {
	disconnect_repository(timeline);
	g_clear_pointer(&timeline->stream, stop_history_stream);
	g_clear_pointer(&timeline->selected_hash, g_free);
	g_clear_pointer(&timeline->wanted_hash, g_free);
	g_clear_pointer(&timeline->restoring_hash, g_free);
	g_list_store_remove_all(timeline->store);
	if (!repository)
		return;
	timeline->repository = g_object_ref(repository);
	timeline->changes_handler = g_signal_connect(repository, "changes-loaded", G_CALLBACK(update_now_row), timeline);
	timeline->history_handler = g_signal_connect(repository, "history-changed", G_CALLBACK(follow_history),
		timeline);
	reload_timeline_history(timeline);
}


void select_timeline_commit(Timeline *timeline, const char *hash) {
	g_set_str(&timeline->wanted_hash, hash);
	look_for_wanted(timeline);
}


void select_timeline_now(Timeline *timeline) {
	gtk_single_selection_set_selected(timeline->selection, 0);
}


void free_timeline(Timeline *timeline) {
	disconnect_repository(timeline);
	g_clear_pointer(&timeline->stream, stop_history_stream);
	g_clear_pointer(&timeline->layout, free_lane_layout);
	g_ptr_array_unref(timeline->waiting_items);
	for (guint index = 0; index < timeline->rows->len; index++)
		g_object_weak_unref(g_ptr_array_index(timeline->rows, index), forget_row, timeline);
	g_ptr_array_unref(timeline->rows);
	g_object_unref(timeline->selection);
	g_object_unref(timeline->store);
	g_free(timeline->wanted_hash);
	g_free(timeline->selected_hash);
	g_free(timeline->restoring_hash);
	g_free(timeline);
}
