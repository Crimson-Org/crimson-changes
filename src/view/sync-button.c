#include <adwaita.h>

#include "view/icons.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "common/widgets/loading.h"
#include "app/settings.h"
#include "view/sync-button.h"

#define BUTTON_KEY "sync-button"
/* A quiet fetch happens at most this often, so switching windows back and forth does not hammer the remote */
#define QUIET_FETCH_SECONDS (5 * 60)
/* A quiet fetch that takes longer than this is given up */
#define QUIET_FETCH_LIMIT_SECONDS 60

typedef struct {
	Window *window;
	Repository *repository;
	gulong changes_handler;
	GtkWidget *button, *icon, *count, *popover, *pull_row, *push_row, *fetch_row;
	gint64 last_fetch;
	/* Syncs still running, and the wait before the icon spins */
	int sync_count;
	guint spin_source;
	/* The quiet fetch running, which can be stopped */
	GCancellable *quiet_fetch;
} SyncButton;

typedef struct {
	SyncButton *button;
	char *failure, *success;
} SyncCommand;


static SyncButton *get_sync(GtkWidget *widget) {
	return g_object_get_data(G_OBJECT(widget), BUTTON_KEY);
}


/* The icon only spins once a sync has taken half a second, like every other waiting sign */
static gboolean start_spinning(gpointer data) {
	SyncButton *button = data;
	button->spin_source = 0;
	gtk_widget_add_css_class(button->icon, "spinning");
	return G_SOURCE_REMOVE;
}


static void show_syncing(SyncButton *button, gboolean is_syncing) {
	button->sync_count += is_syncing ? 1 : -1;
	if (button->sync_count > 0 && !button->spin_source && !gtk_widget_has_css_class(button->icon, "spinning"))
		button->spin_source = g_timeout_add(SPINNER_DELAY_MILLISECONDS, start_spinning, button);
	if (button->sync_count == 0) {
		g_clear_handle_id(&button->spin_source, g_source_remove);
		gtk_widget_remove_css_class(button->icon, "spinning");
	}
}


static void finish_sync(GitResult *result, gpointer data) {
	SyncCommand *command = data;
	show_syncing(command->button, FALSE);
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(command->button->window, command->failure, result);
	else if (!result->is_cancelled && has_git_succeeded(result) && command->success)
		show_toast(command->button->window, command->success);
	release_window(command->button->window);
	g_object_unref(command->button->button);
	g_free(command->failure);
	g_free(command->success);
	g_free(command);
}


static void run_sync(SyncButton *button, const char *const *arguments, const char *failure, const char *success) {
	if (!button->repository)
		return;
	SyncCommand *command = g_new0(SyncCommand, 1);
	/* The button's own data lives as long as the widget, so the widget is kept too */
	command->button = button;
	g_object_ref(button->button);
	keep_window(button->window);
	command->failure = g_strdup(failure);
	command->success = g_strdup(success);
	show_syncing(button, TRUE);
	button->last_fetch = g_get_monotonic_time();
	run_repository_command(button->repository, arguments, NULL, finish_sync, command);
}


static void press_pull(GtkButton *widget, SyncButton *button) {
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	/* Pulling never makes a merge on its own, so a branch that went two ways asks the user first */
	const char *arguments[] = {"pull", "--ff-only", NULL};
	run_sync(button, arguments, TEXT_PULL_FAILED, TEXT_PULLED);
}


static void press_push(GtkButton *widget, SyncButton *button) {
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	/* A branch that was never sent goes to the remote git picks itself, the only one or origin, and follows it from
	 * then on */
	const char *arguments[] = {"-c", "push.autoSetupRemote=true", "push", NULL};
	run_sync(button, arguments, TEXT_PUSH_FAILED, TEXT_PUSHED);
}


static void press_fetch(GtkButton *widget, SyncButton *button) {
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	const char *arguments[] = {"fetch", "--all", "--prune", "--quiet", NULL};
	run_sync(button, arguments, TEXT_FETCH_FAILED, TEXT_FETCHED);
}


/* A fetch nobody asked for runs beside the queue of commands, so a remote that does not answer never holds up a
 * commit, and it gives up after a while */
typedef struct {
	SyncButton *button;
	Repository *repository;
	GCancellable *cancellable;
	guint limit_source;
} QuietFetch;


static gboolean stop_quiet_fetch(gpointer data) {
	QuietFetch *fetch = data;
	fetch->limit_source = 0;
	g_cancellable_cancel(fetch->cancellable);
	return G_SOURCE_REMOVE;
}


static void finish_quiet_fetch(GitResult *result, gpointer data) {
	QuietFetch *fetch = data;
	g_clear_handle_id(&fetch->limit_source, g_source_remove);
	show_syncing(fetch->button, FALSE);
	if (fetch->button->quiet_fetch == fetch->cancellable)
		g_clear_object(&fetch->button->quiet_fetch);
	if (has_git_succeeded(result))
		refresh_repository(fetch->repository);
	release_window(fetch->button->window);
	g_object_unref(fetch->button->button);
	g_object_unref(fetch->repository);
	g_object_unref(fetch->cancellable);
	g_free(fetch);
}


void fetch_quietly(GtkWidget *widget) {
	SyncButton *button = get_sync(widget);
	if (!button->repository || !get_settings()->fetches_automatically || !has_repository_remote(button->repository))
		return;
	gint64 now = g_get_monotonic_time();
	if (button->last_fetch && now - button->last_fetch < QUIET_FETCH_SECONDS * G_USEC_PER_SEC)
		return;
	if (button->quiet_fetch)
		return;
	button->last_fetch = now;
	QuietFetch *fetch = g_new0(QuietFetch, 1);
	fetch->button = button;
	fetch->repository = g_object_ref(button->repository);
	fetch->cancellable = g_cancellable_new();
	button->quiet_fetch = g_object_ref(fetch->cancellable);
	keep_window(button->window);
	g_object_ref(button->button);
	fetch->limit_source = g_timeout_add_seconds(QUIET_FETCH_LIMIT_SECONDS, stop_quiet_fetch, fetch);
	show_syncing(button, TRUE);
	const char *arguments[] = {"fetch", "--all", "--prune", "--quiet", NULL};
	GitOptions options = {.cancellable = fetch->cancellable, .is_quiet = TRUE};
	run_git(get_repository_folder(fetch->repository), arguments, &options, finish_quiet_fetch, fetch);
}


static GtkWidget *create_row(const char *title, const char *icon, GCallback pressed, SyncButton *button) {
	GtkWidget *row = adw_button_content_new();
	adw_button_content_set_label(ADW_BUTTON_CONTENT(row), title);
	adw_button_content_set_icon_name(ADW_BUTTON_CONTENT(row), icon);
	gtk_widget_set_halign(row, GTK_ALIGN_START);
	GtkWidget *widget = gtk_button_new();
	gtk_button_set_child(GTK_BUTTON(widget), row);
	gtk_widget_add_css_class(widget, "flat");
	g_signal_connect(widget, "clicked", pressed, button);
	return widget;
}


static void update_rows(SyncButton *button) {
	const WorkChanges *changes = get_work_changes(button->repository);
	int ahead = changes ? changes->status->ahead : 0, behind = changes ? changes->status->behind : 0;
	gboolean has_upstream = changes && changes->status->has_upstream;
	g_autofree char *pull = behind ? g_strdup_printf(TEXT_PULL_COUNT, behind) : g_strdup(TEXT_PULL);
	g_autofree char *push = ahead ? g_strdup_printf(TEXT_PUSH_COUNT, ahead) : g_strdup(has_upstream ? TEXT_PUSH :
		TEXT_PUBLISH);
	adw_button_content_set_label(ADW_BUTTON_CONTENT(gtk_button_get_child(GTK_BUTTON(button->pull_row))), pull);
	adw_button_content_set_label(ADW_BUTTON_CONTENT(gtk_button_get_child(GTK_BUTTON(button->push_row))), push);
	gtk_widget_set_sensitive(button->pull_row, has_upstream);
	gtk_widget_set_tooltip_text(button->pull_row, TEXT_PULL_TOOLTIP);
	gtk_widget_set_tooltip_text(button->push_row, has_upstream ? TEXT_PUSH_TOOLTIP : TEXT_PUBLISH_TOOLTIP);
	/* The button says what it would do: commits to get first, else commits to send, else only its icon */
	gtk_image_set_from_icon_name(GTK_IMAGE(button->icon), behind ? ICON_PULL : ahead ?
		ICON_PUSH : ICON_SYNC);
	g_autofree char *count = behind ? g_strdup_printf(TEXT_SYNC_PULL_LABEL, behind) : ahead ?
		g_strdup_printf(TEXT_SYNC_PUSH_LABEL, ahead) : g_strdup("");
	gtk_label_set_text(GTK_LABEL(button->count), count);
	gtk_widget_set_visible(button->count, *count != 0);
	gtk_widget_set_visible(button->button, has_repository_remote(button->repository));
}


static void follow_changes(Repository *repository, SyncButton *button) {
	update_rows(button);
}


static void free_sync_button(gpointer data) {
	SyncButton *button = data;
	g_clear_handle_id(&button->spin_source, g_source_remove);
	if (button->repository) {
		g_clear_signal_handler(&button->changes_handler, button->repository);
		g_clear_object(&button->repository);
	}
	g_free(button);
}


GtkWidget *create_sync_button(Window *window) {
	SyncButton *button = g_new0(SyncButton, 1);
	button->window = window;
	button->pull_row = create_row(TEXT_PULL, ICON_PULL, G_CALLBACK(press_pull), button);
	button->push_row = create_row(TEXT_PUSH, ICON_PUSH, G_CALLBACK(press_push), button);
	button->fetch_row = create_row(TEXT_FETCH, "view-refresh-symbolic", G_CALLBACK(press_fetch), button);
	gtk_widget_set_tooltip_text(button->fetch_row, TEXT_FETCH_TOOLTIP);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_HAIRLINE);
	gtk_box_append(GTK_BOX(box), button->pull_row);
	gtk_box_append(GTK_BOX(box), button->push_row);
	gtk_box_append(GTK_BOX(box), button->fetch_row);
	button->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(button->popover), box);
	gtk_widget_add_css_class(button->popover, "menu");
	button->icon = gtk_image_new_from_icon_name(ICON_SYNC);
	button->count = gtk_label_new(NULL);
	gtk_widget_add_css_class(button->count, "sync-count");
	GtkWidget *content = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	gtk_box_append(GTK_BOX(content), button->icon);
	gtk_box_append(GTK_BOX(content), button->count);
	button->button = gtk_menu_button_new();
	gtk_menu_button_set_child(GTK_MENU_BUTTON(button->button), content);
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button->button), button->popover);
	gtk_widget_set_tooltip_text(button->button, TEXT_SYNC_TOOLTIP);
	gtk_widget_set_visible(button->button, FALSE);
	g_object_set_data_full(G_OBJECT(button->button), BUTTON_KEY, button, free_sync_button);
	return button->button;
}


void set_sync_button_repository(GtkWidget *widget, Repository *repository) {
	SyncButton *button = get_sync(widget);
	if (button->repository) {
		g_clear_signal_handler(&button->changes_handler, button->repository);
		g_clear_object(&button->repository);
	}
	button->last_fetch = 0;
	if (button->quiet_fetch)
		g_cancellable_cancel(button->quiet_fetch);
	gtk_widget_set_visible(button->button, FALSE);
	if (!repository)
		return;
	button->repository = g_object_ref(repository);
	button->changes_handler = g_signal_connect(repository, "changes-loaded", G_CALLBACK(follow_changes), button);
}
