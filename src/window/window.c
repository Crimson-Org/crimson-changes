#include <string.h>

#include "lang/en.h"
#include "common/app/startup.h"
#include "common/widgets/loading.h"
#include "common/widgets/panes.h"
#include "common/widgets/sidebar-width.h"
#include "common/widgets/spacing.h"
#include "common/window/file-drop.h"
#include "common/window/navigation.h"
#include "app/settings.h"
#include "app/state.h"
#include "view/branch-button.h"
#include "view/changes-page.h"
#include "view/commit-menu.h"
#include "view/commit-page.h"
#include "view/format.h"
#include "view/sync-button.h"
#include "view/timeline.h"
#include "window/info-dialog.h"
#include "window/start-pages.h"
#include "window/window.h"

/* Below this width the timeline and the selected page become two pages that slide */
#define NARROW_WINDOW "max-width: 860sp"
#define MINIMUM_WIDTH 360
#define MINIMUM_HEIGHT 420
#define START_PAGE "start"
#define CLOSE_ACTION "close-folder"
#define LOADING_PAGE "loading"
#define SETUP_PAGE "setup"
#define PROBLEM_PAGE "problem"
#define REPOSITORY_PAGE "repository"
#define NOW_PAGE "now"
#define COMMIT_PAGE "commit"
#define FAILURE_TOAST_SECONDS 8
/* Long enough to read what happened and reach the button */
#define UNDO_TOAST_SECONDS 10
#define DETAILS_RESPONSE "details"

struct Window {
	AdwApplicationWindow *window;
	AdwToastOverlay *toasts;
	GtkWidget *pages, *start_page, *setup_page, *problem_page;
	WindowState state;
	Repository *repository;
	CrimsonPanes *panes;
	Timeline *timeline;
	GtkWidget *sidebar_header, *branch_button, *search_button;
	AdwNavigationView *navigation;
	GtkWidget *content_header, *content_title, *detail_stack, *sync_button, *commit_menu_button, *back_button;
	ChangesPage *changes_page;
	CommitPage *commit_page;
	/* Each folder opened gets a number, so an answer about a folder opened before is ignored */
	guint open_number;
	gboolean shows_all_branches, wraps_lines;
	/* Closing stops answers about folders from opening anything */
	gboolean is_closed;
};

typedef struct {
	Window *window;
	guint number;
	char *path;
} OpenRequest;


Window *keep_window(Window *window) {
	g_object_ref(window->window);
	return window;
}


void release_window(Window *window) {
	g_object_unref(window->window);
}


GtkWindow *get_gtk_window(Window *window) {
	return GTK_WINDOW(window->window);
}


GtkWidget *get_window_widget(Window *window) {
	return GTK_WIDGET(window->window);
}


Repository *get_window_repository(Window *window) {
	return window->repository;
}


/* A folder counts from the moment it starts opening until it is closed */
gboolean has_window_folder(Window *window) {
	return g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(window->pages)), START_PAGE) != 0;
}


/* The new page fades in on its own, since a stack's crossfade that starts before the window's first frame can stop
 * halfway */
static void show_page(Window *window, const char *name) {
	if (g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(window->pages)), name) == 0)
		return;
	gtk_stack_set_visible_child_name(GTK_STACK(window->pages), name);
	fade_in_widget(gtk_stack_get_visible_child(GTK_STACK(window->pages)));
}


/* Close leads back to the start page from any folder, so it has nothing to do there, whichever way the page came */
static void follow_page(GtkStack *pages, GParamSpec *spec, Window *window) {
	GAction *close = g_action_map_lookup_action(G_ACTION_MAP(window->window), CLOSE_ACTION);
	g_simple_action_set_enabled(G_SIMPLE_ACTION(close), has_window_folder(window));
}


/* Toasts */

void show_toast(Window *window, const char *text) {
	AdwToast *toast = adw_toast_new(text);
	adw_toast_set_use_markup(toast, FALSE);
	adw_toast_overlay_add_toast(window->toasts, toast);
}


typedef struct {
	UndoFunction undo;
	gpointer data;
	GDestroyNotify destroy;
} UndoRequest;


static void free_undo_request(gpointer data, GClosure *closure) {
	UndoRequest *request = data;
	if (request->destroy)
		request->destroy(request->data);
	g_free(request);
}


static void press_undo(AdwToast *toast, UndoRequest *request) {
	request->undo(request->data);
}


void show_undo_toast(Window *window, const char *text, UndoFunction undo, gpointer data, GDestroyNotify destroy) {
	AdwToast *toast = adw_toast_new(text);
	adw_toast_set_use_markup(toast, FALSE);
	adw_toast_set_button_label(toast, TEXT_UNDO);
	adw_toast_set_timeout(toast, UNDO_TOAST_SECONDS);
	UndoRequest *request = g_new0(UndoRequest, 1);
	request->undo = undo;
	request->data = data;
	request->destroy = destroy;
	g_signal_connect_data(toast, "button-clicked", G_CALLBACK(press_undo), request, free_undo_request, 0);
	adw_toast_overlay_add_toast(window->toasts, toast);
}


static void show_failure_details(AdwToast *toast, Window *window) {
	const char *what = g_object_get_data(G_OBJECT(toast), "what");
	const char *output = g_object_get_data(G_OBJECT(toast), "output");
	AdwDialog *dialog = adw_alert_dialog_new(what, NULL);
	GtkWidget *label = gtk_label_new(output);
	gtk_label_set_selectable(GTK_LABEL(label), TRUE);
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_widget_add_css_class(label, "monospace");
	gtk_widget_add_css_class(label, "git-output");
	adw_alert_dialog_set_extra_child(ADW_ALERT_DIALOG(dialog), label);
	adw_alert_dialog_add_response(ADW_ALERT_DIALOG(dialog), DETAILS_RESPONSE, TEXT_CLOSE);
	adw_alert_dialog_set_prefer_wide_layout(ADW_ALERT_DIALOG(dialog), TRUE);
	adw_dialog_present(dialog, GTK_WIDGET(window->window));
}


void show_git_failure(Window *window, const char *what, const GitResult *result) {
	g_autofree char *summary = get_git_error_summary(result);
	g_autofree char *text = *summary ? g_strdup_printf(TEXT_FAILURE, what, summary) : g_strdup(what);
	g_warning("%s: %s", what, result->error);
	AdwToast *toast = adw_toast_new(text);
	adw_toast_set_use_markup(toast, FALSE);
	adw_toast_set_timeout(toast, FAILURE_TOAST_SECONDS);
	if (result->error && *result->error) {
		adw_toast_set_button_label(toast, TEXT_DETAILS);
		g_object_set_data_full(G_OBJECT(toast), "what", g_strdup(what), g_free);
		g_object_set_data_full(G_OBJECT(toast), "output", g_strstrip(g_strdup(result->error)), g_free);
		g_signal_connect(toast, "button-clicked", G_CALLBACK(show_failure_details), window);
	}
	adw_toast_overlay_add_toast(window->toasts, toast);
}


/* The main pane */

static void set_content_title(Window *window, const char *title, const char *subtitle) {
	adw_window_title_set_title(ADW_WINDOW_TITLE(window->content_title), title);
	adw_window_title_set_subtitle(ADW_WINDOW_TITLE(window->content_title), subtitle ? subtitle : "");
}


/* The header names the folder, which nothing else on screen says, while the page itself says what it shows */
static void show_folder_title(Window *window) {
	g_autofree char *folder = window->repository ? shorten_home_path(get_repository_folder(window->repository)) :
		g_strdup("");
	set_content_title(window, "", folder);
}


static void slide_to_content(Window *window) {
	adw_navigation_view_pop_to_tag(window->navigation, REPOSITORY_PAGE);
	crimson_panes_set_show_content(window->panes, TRUE);
}


static void show_now_page(Window *window) {
	gtk_stack_set_visible_child_name(GTK_STACK(window->detail_stack), NOW_PAGE);
	show_folder_title(window);
}


void show_work_changes(Window *window) {
	show_now_page(window);
	slide_to_content(window);
}


/* Fills the main pane with the commit, without sliding to it in a narrow window */
static void show_stash_or_commit(Window *window, const Commit *commit, const char *stash_name) {
	gtk_stack_set_visible_child_name(GTK_STACK(window->detail_stack), COMMIT_PAGE);
	set_commit_menu_commit(window->commit_menu_button, commit, stash_name);
	show_folder_title(window);
	/* Choosing the commit already shown does not read it again */
	if (g_strcmp0(get_commit_page_hash(window->commit_page), commit->hash) != 0)
		show_commit_page(window->commit_page, commit, stash_name);
}


void show_commit(Window *window, const Commit *commit) {
	show_stash_or_commit(window, commit, NULL);
	slide_to_content(window);
}


/* A narrow window only slides to the chosen page when the user chose it, so a reload never moves the view */
static void choose_timeline_item(TimelineItem *item, gboolean is_users_choice, gpointer data) {
	Window *window = data;
	if (item->kind == TIMELINE_NOW)
		show_now_page(window);
	else
		show_stash_or_commit(window, item->commit, item->kind == TIMELINE_STASH ? item->stash_name : NULL);
	if (is_users_choice)
		slide_to_content(window);
}


/* Clicking a row, even the one already chosen, brings its details into view from their top */
static void press_timeline_row(gpointer data) {
	Window *window = data;
	glide_changes_page_to_top(window->changes_page);
	glide_commit_page_to_top(window->commit_page);
	slide_to_content(window);
}


/* A commit outside the history shown, such as on another branch, still opens on its own */
static void read_missing_commit(GitResult *result, gpointer data) {
	Window *window = data;
	if (result->is_cancelled || !has_git_succeeded(result))
		return;
	gsize length = 0;
	const char *output = g_bytes_get_data(result->output, &length);
	Commit *commit = parse_commit_details(output ? output : "", length);
	if (commit)
		show_commit(window, commit);
	free_commit(commit);
}


static void open_missing_commit(const char *hash, gpointer data) {
	Window *window = data;
	const char *arguments[] = {"show", "-s", DECORATE_OPTION, DETAILS_FORMAT, hash, NULL};
	read_repository(window->repository, arguments, read_missing_commit, window);
}


void go_to_commit(Window *window, const char *hash) {
	if (!window->repository || !hash)
		return;
	adw_navigation_view_pop_to_tag(window->navigation, REPOSITORY_PAGE);
	select_timeline_commit(window->timeline, hash);
}


void push_window_page(Window *window, AdwNavigationPage *page) {
	adw_navigation_view_push(window->navigation, page);
	crimson_panes_set_show_content(window->panes, TRUE);
}


void reload_timeline(Window *window) {
	if (window->timeline)
		reload_timeline_history(window->timeline);
}


/* Opening a folder */

static void close_current_repository(Window *window) {
	if (!window->repository)
		return;
	set_timeline_repository(window->timeline, NULL);
	set_changes_page_repository(window->changes_page, NULL);
	set_branch_button_repository(window->branch_button, NULL);
	set_sync_button_repository(window->sync_button, NULL);
	close_repository(window->repository);
	g_clear_object(&window->repository);
}


static void show_repository(Window *window, Repository *repository) {
	close_current_repository(window);
	window->repository = g_object_ref(repository);
	add_recent_repository(get_repository_folder(repository));
	gtk_window_set_title(GTK_WINDOW(window->window), get_repository_name(repository));
	Settings *settings = get_settings();
	DiffOptions options = {settings->ignores_whitespace, settings->context_lines};
	set_diff_options(repository, &options);
	set_branch_button_repository(window->branch_button, repository);
	set_sync_button_repository(window->sync_button, repository);
	set_changes_page_repository(window->changes_page, repository);
	set_timeline_repository(window->timeline, repository);
	adw_navigation_view_pop_to_tag(window->navigation, REPOSITORY_PAGE);
	show_page(window, REPOSITORY_PAGE);
	show_now_page(window);
	crimson_panes_set_show_content(window->panes, FALSE);
	start_watching_repository(repository);
	fetch_quietly(window->sync_button);
	g_message("Repository opened: %s", get_repository_folder(repository));
}


static void show_problem(Window *window, const char *title, const char *description) {
	set_problem_page_text(window->problem_page, title, description);
	show_page(window, PROBLEM_PAGE);
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
}


static void report_found(Repository *repository, FindResult result, const char *message, gpointer data) {
	OpenRequest *request = data;
	Window *window = request->window;
	gboolean is_current = request->number == window->open_number && !window->is_closed;
	if (is_current) {
		switch (result) {
		case FIND_REPOSITORY_FOUND:
			show_repository(window, repository);
			break;
		case FIND_REPOSITORY_NONE:
			close_current_repository(window);
			set_setup_page_folder(window->setup_page, request->path);
			show_page(window, SETUP_PAGE);
			g_message("No repository in %s", request->path);
			break;
		case FIND_REPOSITORY_BARE:
			close_current_repository(window);
			show_problem(window, TEXT_BARE_TITLE, TEXT_BARE_DESCRIPTION);
			break;
		case FIND_REPOSITORY_NO_GIT:
			close_current_repository(window);
			show_problem(window, TEXT_NO_GIT_TITLE, TEXT_NO_GIT_DESCRIPTION);
			break;
		case FIND_REPOSITORY_MISSING:
			close_current_repository(window);
			forget_recent_repository(request->path);
			show_problem(window, TEXT_OPEN_FAILED_TITLE, TEXT_FOLDER_MISSING);
			break;
		case FIND_REPOSITORY_FAILED:
			close_current_repository(window);
			show_problem(window, TEXT_OPEN_FAILED_TITLE, message);
			break;
		}
	}
	release_window(window);
	g_free(request->path);
	g_free(request);
}


void open_folder(Window *window, const char *path) {
	OpenRequest *request = g_new0(OpenRequest, 1);
	request->window = keep_window(window);
	request->number = ++window->open_number;
	request->path = g_strdup(path);
	/* A window leaving its start page shows it is loading at once, and a window already showing a folder keeps it
	 * until the answer is in */
	if (!has_window_folder(window))
		gtk_stack_set_visible_child_name(GTK_STACK(window->pages), LOADING_PAGE);
	find_repository(path, report_found, request);
}


static void finish_folder_choice(GObject *source, GAsyncResult *result, gpointer data) {
	Window *window = data;
	if (window->is_closed) {
		release_window(window);
		return;
	}
	g_autoptr(GError) error = NULL;
	g_autoptr(GFile) folder = gtk_file_dialog_select_folder_finish(GTK_FILE_DIALOG(source), result, &error);
	g_autofree char *path = folder ? g_file_get_path(folder) : NULL;
	if (path)
		open_folder(window, path);
	else if (folder)
		show_toast(window, TEXT_NOT_LOCAL);
	/* The start page lists the recent folders as they are now, in case it shows behind the dialog */
	if (!path)
		update_start_page(window->start_page);
	release_window(window);
}


void ask_for_folder(Window *window) {
	g_autoptr(GtkFileDialog) dialog = gtk_file_dialog_new();
	gtk_file_dialog_set_title(dialog, TEXT_OPEN_FOLDER_TITLE);
	const char *const *recent = get_recent_repositories();
	g_autofree char *start = recent[0] ? g_path_get_dirname(recent[0]) : g_strdup(g_get_home_dir());
	g_autoptr(GFile) initial = g_file_new_for_path(start);
	gtk_file_dialog_set_initial_folder(dialog, initial);
	gtk_file_dialog_select_folder(dialog, GTK_WINDOW(window->window), NULL, finish_folder_choice, keep_window(window));
}


static void drop_folders(GFile **files, guint count, gpointer data) {
	g_autofree char *path = count > 0 ? g_file_get_path(files[0]) : NULL;
	if (path)
		open_folder(data, path);
}


/* Actions */

static void run_open(GSimpleAction *action, GVariant *parameter, gpointer data) {
	ask_for_folder(data);
}


static void run_search(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (!window->repository)
		return;
	crimson_panes_set_show_content(window->panes, FALSE);
	gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(window->search_button), TRUE);
}


static void run_branches(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (!window->repository)
		return;
	crimson_panes_set_show_content(window->panes, FALSE);
	open_branch_list(window->branch_button);
}


static void run_refresh(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->repository)
		refresh_repository(window->repository);
}


static void run_info(GSimpleAction *action, GVariant *parameter, gpointer data) {
	show_info_dialog(data);
}


/* Closing the folder leads back to the start page, and an open still on its way is forgotten */
static void run_close(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	window->open_number++;
	close_current_repository(window);
	update_start_page(window->start_page);
	show_page(window, START_PAGE);
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
}


static void run_files(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (!window->repository)
		return;
	g_autoptr(GFile) folder = g_file_new_for_path(get_repository_folder(window->repository));
	g_autoptr(GtkFileLauncher) launcher = gtk_file_launcher_new(folder);
	gtk_file_launcher_launch(launcher, GTK_WINDOW(window->window), NULL, NULL, NULL);
}


static void run_back(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (adw_navigation_view_get_visible_page(window->navigation) !=
		adw_navigation_view_find_page(window->navigation, REPOSITORY_PAGE))
		adw_navigation_view_pop(window->navigation);
	else
		crimson_panes_set_show_content(window->panes, FALSE);
}


static void run_now(GSimpleAction *action, GVariant *parameter, gpointer data) {
	Window *window = data;
	if (window->repository)
		select_timeline_now(window->timeline);
}


static void toggle_all_branches(GSimpleAction *action, GVariant *parameter, gpointer data) {
	gboolean is_on = !g_variant_get_boolean(g_action_get_state(G_ACTION(action)));
	g_simple_action_set_state(action, g_variant_new_boolean(is_on));
	get_settings()->shows_all_branches = is_on;
	save_settings();
	apply_settings();
}


static void add_window_actions(Window *window) {
	const GActionEntry ENTRIES[] = {
		{"open", run_open, NULL, NULL, NULL, {0}},
		{CLOSE_ACTION, run_close, NULL, NULL, NULL, {0}},
		{"search", run_search, NULL, NULL, NULL, {0}},
		{"branches", run_branches, NULL, NULL, NULL, {0}},
		{"refresh", run_refresh, NULL, NULL, NULL, {0}},
		{"info", run_info, NULL, NULL, NULL, {0}},
		{"files", run_files, NULL, NULL, NULL, {0}},
		{"back", run_back, NULL, NULL, NULL, {0}},
		{"now", run_now, NULL, NULL, NULL, {0}},
		{"all-branches", toggle_all_branches, NULL, "false", NULL, {0}},
	};
	g_action_map_add_action_entries(G_ACTION_MAP(window->window), ENTRIES, G_N_ELEMENTS(ENTRIES), window);
	GAction *all = g_action_map_lookup_action(G_ACTION_MAP(window->window), "all-branches");
	g_simple_action_set_state(G_SIMPLE_ACTION(all), g_variant_new_boolean(get_settings()->shows_all_branches));
}


/* Building the window */

static GMenuModel *create_main_menu(void) {
	GMenu *menu = g_menu_new();
	g_autoptr(GMenu) folder = g_menu_new();
	g_menu_append(folder, TEXT_OPEN, "win.open");
	g_menu_append(folder, TEXT_CLOSE, "win." CLOSE_ACTION);
	g_menu_append(folder, TEXT_OPEN_IN_FILES, "win.files");
	g_menu_append(folder, TEXT_REPOSITORY_DETAILS, "win.info");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(folder));
	g_autoptr(GMenu) view = g_menu_new();
	g_menu_append(view, TEXT_SHOW_ALL_BRANCHES, "win.all-branches");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(view));
	g_autoptr(GMenu) application = g_menu_new();
	g_menu_append(application, TEXT_SETTINGS, "app.settings");
	g_menu_append(application, TEXT_ABOUT, "app.about");
	g_menu_append(application, TEXT_QUIT, "app.quit");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(application));
	return G_MENU_MODEL(menu);
}


static GtkWidget *create_menu_button(void) {
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
	g_autoptr(GMenuModel) menu = create_main_menu();
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), menu);
	gtk_menu_button_set_primary(GTK_MENU_BUTTON(button), TRUE);
	gtk_widget_set_tooltip_text(button, TEXT_MENU);
	return button;
}


static GtkWidget *create_sidebar(Window *window) {
	window->timeline = create_timeline(choose_timeline_item, press_timeline_row, open_missing_commit, window);
	window->branch_button = create_branch_button(window);
	window->search_button = gtk_toggle_button_new();
	gtk_button_set_icon_name(GTK_BUTTON(window->search_button), "system-search-symbolic");
	gtk_widget_set_tooltip_text(window->search_button, TEXT_SEARCH_TOOLTIP);
	GtkWidget *search_bar = get_timeline_search_bar(window->timeline);
	g_object_bind_property(window->search_button, "active", search_bar, "search-mode-enabled",
		G_BINDING_BIDIRECTIONAL);
	gtk_search_bar_set_key_capture_widget(GTK_SEARCH_BAR(search_bar), GTK_WIDGET(window->window));
	window->sidebar_header = adw_header_bar_new();
	gtk_widget_add_css_class(window->sidebar_header, "title-bar");
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(window->sidebar_header), FALSE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(window->sidebar_header), window->branch_button);
	adw_header_bar_pack_end(ADW_HEADER_BAR(window->sidebar_header), window->search_button);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), window->sidebar_header);
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), search_bar);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), get_timeline_widget(window->timeline));
	gtk_widget_add_css_class(toolbar, "timeline-pane");
	return toolbar;
}


static GtkWidget *create_content(Window *window) {
	window->changes_page = create_changes_page(window);
	window->commit_page = create_commit_page(window);
	window->detail_stack = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(window->detail_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
	gtk_stack_add_named(GTK_STACK(window->detail_stack), get_changes_page_widget(window->changes_page), NOW_PAGE);
	gtk_stack_add_named(GTK_STACK(window->detail_stack), get_commit_page_widget(window->commit_page), COMMIT_PAGE);
	window->content_title = adw_window_title_new("", "");
	window->content_header = adw_header_bar_new();
	gtk_widget_add_css_class(window->content_header, "title-bar");
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(window->content_header), FALSE);
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(window->content_header), window->content_title);
	window->back_button = gtk_button_new_from_icon_name("go-previous-symbolic");
	gtk_widget_set_tooltip_text(window->back_button, TEXT_BACK);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(window->back_button), "win.back");
	gtk_widget_set_visible(window->back_button, FALSE);
	adw_header_bar_pack_start(ADW_HEADER_BAR(window->content_header), window->back_button);
	adw_header_bar_pack_end(ADW_HEADER_BAR(window->content_header), create_menu_button());
	window->sync_button = create_sync_button(window);
	adw_header_bar_pack_end(ADW_HEADER_BAR(window->content_header), window->sync_button);
	window->commit_menu_button = create_commit_menu_button(window);
	set_commit_page_menu(window->commit_page, window->commit_menu_button);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), window->content_header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), window->detail_stack);
	AdwNavigationPage *root = adw_navigation_page_new_with_tag(toolbar, TEXT_APPLICATION_NAME, REPOSITORY_PAGE);
	window->navigation = ADW_NAVIGATION_VIEW(adw_navigation_view_new());
	adw_navigation_view_add(window->navigation, root);
	return GTK_WIDGET(window->navigation);
}


/* A page outside a repository has its own plain header with the menu */
static GtkWidget *wrap_with_header(GtkWidget *content) {
	GtkWidget *header = adw_header_bar_new();
	gtk_widget_add_css_class(header, "title-bar");
	adw_header_bar_pack_end(ADW_HEADER_BAR(header), create_menu_button());
	adw_header_bar_set_title_widget(ADW_HEADER_BAR(header), gtk_label_new(NULL));
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), header);
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), content);
	return toolbar;
}


static void follow_collapse(CrimsonPanes *panes, GParamSpec *spec, Window *window) {
	gboolean is_narrow = crimson_panes_get_collapsed(panes);
	adw_header_bar_set_show_end_title_buttons(ADW_HEADER_BAR(window->sidebar_header), is_narrow);
	adw_header_bar_set_show_start_title_buttons(ADW_HEADER_BAR(window->content_header), is_narrow);
	gtk_widget_set_visible(window->back_button, is_narrow);
}


static void remember_sidebar_width(int width, gpointer data) {
	Window *window = data;
	window->state.sidebar_width = width;
}


/* Coming back to the window reads the folder again and looks for new commits, since other programs may have
 * changed both in the meantime */
static void follow_focus(GtkWindow *gtk_window, GParamSpec *spec, Window *window) {
	if (!gtk_window_is_active(gtk_window) || !window->repository)
		return;
	refresh_repository(window->repository);
	fetch_quietly(window->sync_button);
}


static void follow_settings(gpointer data) {
	Window *window = data;
	Settings *settings = get_settings();
	if (!window->repository)
		return;
	DiffOptions options = {settings->ignores_whitespace, settings->context_lines};
	const DiffOptions *current = get_diff_options(window->repository);
	gboolean has_diff_changed = current->ignores_whitespace != options.ignores_whitespace ||
		current->context_lines != options.context_lines;
	/* The code size follows through the stylesheet, so only a change that alters the diffs or their wrapping makes
	 * the code views again */
	if (has_diff_changed) {
		set_diff_options(window->repository, &options);
		reload_commit_page(window->commit_page);
	} else if (window->wraps_lines != settings->wraps_lines) {
		refresh_changes_page(window->changes_page);
		reload_commit_page(window->commit_page);
	}
	window->wraps_lines = settings->wraps_lines;
	if (window->shows_all_branches != settings->shows_all_branches) {
		window->shows_all_branches = settings->shows_all_branches;
		GAction *all = g_action_map_lookup_action(G_ACTION_MAP(window->window), "all-branches");
		g_simple_action_set_state(G_SIMPLE_ACTION(all), g_variant_new_boolean(settings->shows_all_branches));
		reload_timeline(window);
	}
}


/* The parts are freed once the window's widgets are gone, so no widget can reach a freed part on its way out */
static void free_window(gpointer data, GObject *gtk_window) {
	Window *window = data;
	free_timeline(window->timeline);
	free_changes_page(window->changes_page);
	free_commit_page(window->commit_page);
	g_free(window);
}


static gboolean close_window(GtkWindow *gtk_window, Window *window) {
	read_window_geometry(gtk_window, &window->state.geometry);
	save_window_state(&window->state);
	hide_closing_window(gtk_window);
	window->is_closed = TRUE;
	unwatch_settings(follow_settings, window);
	close_current_repository(window);
	g_object_weak_ref(G_OBJECT(gtk_window), free_window, window);
	return FALSE;
}


static void add_breakpoint(Window *window) {
	AdwBreakpoint *breakpoint = adw_breakpoint_new(adw_breakpoint_condition_parse(NARROW_WINDOW));
	GValue collapsed = G_VALUE_INIT;
	g_value_init(&collapsed, G_TYPE_BOOLEAN);
	g_value_set_boolean(&collapsed, TRUE);
	adw_breakpoint_add_setter(breakpoint, G_OBJECT(window->panes), "collapsed", &collapsed);
	adw_application_window_add_breakpoint(window->window, breakpoint);
}


static GtkWidget *create_loading_page(void) {
	GtkWidget *spinner = create_loading_spinner();
	gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
	return wrap_with_header(spinner);
}


Window *create_window(GtkApplication *application) {
	Window *window = g_new0(Window, 1);
	window->window = ADW_APPLICATION_WINDOW(adw_application_window_new(application));
	gtk_window_set_title(GTK_WINDOW(window->window), TEXT_APPLICATION_NAME);
	g_object_set_data(G_OBJECT(window->window), "window", window);
	load_window_state(&window->state);
	apply_window_geometry(GTK_WINDOW(window->window), &window->state.geometry);
	gtk_widget_set_size_request(GTK_WIDGET(window->window), MINIMUM_WIDTH, MINIMUM_HEIGHT);
	window->shows_all_branches = get_settings()->shows_all_branches;
	window->wraps_lines = get_settings()->wraps_lines;
	add_window_actions(window);
	window->panes = CRIMSON_PANES(crimson_panes_new(create_sidebar(window), create_content(window)));
	g_signal_connect(window->panes, "notify::collapsed", G_CALLBACK(follow_collapse), window);
	GtkWidget *repository_page = add_panes_sidebar_handle(window->panes, window->state.sidebar_width,
		SIDEBAR_MINIMUM, SIDEBAR_MAXIMUM, remember_sidebar_width, window);
	window->start_page = create_start_page(window);
	window->setup_page = create_setup_page(window);
	window->problem_page = create_problem_page(window);
	window->pages = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(window->pages), wrap_with_header(window->start_page), START_PAGE);
	gtk_stack_add_named(GTK_STACK(window->pages), create_loading_page(), LOADING_PAGE);
	gtk_stack_add_named(GTK_STACK(window->pages), wrap_with_header(window->setup_page), SETUP_PAGE);
	gtk_stack_add_named(GTK_STACK(window->pages), wrap_with_header(window->problem_page), PROBLEM_PAGE);
	gtk_stack_add_named(GTK_STACK(window->pages), repository_page, REPOSITORY_PAGE);
	/* A window without a folder shows the start page, with Open and the folders opened before */
	gtk_stack_set_visible_child_name(GTK_STACK(window->pages), START_PAGE);
	g_signal_connect(window->pages, "notify::visible-child-name", G_CALLBACK(follow_page), window);
	follow_page(GTK_STACK(window->pages), NULL, window);
	window->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
	adw_toast_overlay_set_child(window->toasts, window->pages);
	adw_application_window_set_content(window->window, GTK_WIDGET(window->toasts));
	add_breakpoint(window);
	add_file_drop(GTK_WIDGET(window->window), drop_folders, window);
	add_mouse_navigation(GTK_WIDGET(window->window), "win.back", NULL);
	g_signal_connect(window->window, "notify::is-active", G_CALLBACK(follow_focus), window);
	g_signal_connect(window->window, "close-request", G_CALLBACK(close_window), window);
	watch_settings(follow_settings, window);
	follow_collapse(window->panes, NULL, window);
	watch_window_frames(GTK_WIDGET(window->window));
	return window;
}
