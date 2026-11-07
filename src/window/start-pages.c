#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "app/state.h"
#include "view/format.h"
#include "window/setup-dialog.h"
#include "common/widgets/start-view.h"
#include "window/start-pages.h"

#define OPEN_ACTION "win.open"
#define OPEN_ICON "folder-open-symbolic"
#define RECENT_ICON "folder-symbolic"
#define ILLUSTRATION_SETUP "crimson-start-tracking"
#define ILLUSTRATION_PROBLEM "crimson-problem"
#define WINDOW_KEY "window"
#define FOLDER_KEY "folder"
#define STATUS_KEY "status"


static Window *get_page_window(GtkWidget *page) {
	return g_object_get_data(G_OBJECT(page), WINDOW_KEY);
}


static GtkWidget *create_open_button(const char *label, gboolean is_suggested) {
	GtkWidget *button = gtk_button_new_with_label(label);
	if (is_suggested)
		gtk_widget_add_css_class(button, "suggested-action");
	gtk_widget_set_halign(button, GTK_ALIGN_CENTER);
	gtk_actionable_set_action_name(GTK_ACTIONABLE(button), OPEN_ACTION);
	return button;
}


static GtkWidget *create_status_page(const char *icon, const char *title, const char *description) {
	GtkWidget *status = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(status), icon);
	adw_status_page_set_title(ADW_STATUS_PAGE(status), title);
	adw_status_page_set_description(ADW_STATUS_PAGE(status), description);
	gtk_widget_add_css_class(status, "illustrated");
	gtk_widget_set_vexpand(status, TRUE);
	return status;
}


/* The start page, the same as every Crimson app's: Open, and the folders opened lately */

static const StartChoice START_CHOICES[] = {{OPEN_ICON, TEXT_OPEN, OPEN_ACTION}};


static void open_recent(const char *folder, gpointer window) {
	open_folder(window, folder);
}


static void forget_recent(const char *folder, gpointer window) {
	forget_recent_repository(folder);
}


void update_start_page(GtkWidget *page) {
	g_autoptr(GArray) items = g_array_new(FALSE, TRUE, sizeof(StartRecent));
	g_autoptr(GPtrArray) owned = g_ptr_array_new_with_free_func(g_free);
	g_autoptr(GIcon) icon = g_themed_icon_new(RECENT_ICON);
	for (const char *const *folder = get_recent_repositories(); *folder; folder++) {
		if (!g_file_test(*folder, G_FILE_TEST_IS_DIR))
			continue;
		char *name = g_path_get_basename(*folder);
		g_autofree char *parent = g_path_get_dirname(*folder);
		char *place = shorten_home_path(parent);
		g_ptr_array_add(owned, name);
		g_ptr_array_add(owned, place);
		StartRecent item = {icon, name, place, *folder, *folder};
		g_array_append_val(items, item);
	}
	set_start_view_recent(page, (StartRecent *)(void *)items->data, items->len);
}


GtkWidget *create_start_page(Window *window) {
	const StartViewSetup setup = {START_CHOICES, G_N_ELEMENTS(START_CHOICES), NULL, TEXT_RECENT, TEXT_RECENT_REMOVE,
		open_recent, forget_recent, window};
	GtkWidget *page = create_start_view(&setup);
	update_start_page(page);
	return page;
}


/* A folder without a repository */

static void press_start(GtkButton *button, GtkWidget *page) {
	show_setup_dialog(get_page_window(page), g_object_get_data(G_OBJECT(page), FOLDER_KEY));
}


GtkWidget *create_setup_page(Window *window) {
	GtkWidget *start = gtk_button_new_with_label(TEXT_START_TRACKING);
	gtk_widget_add_css_class(start, "suggested-action");
	gtk_widget_set_tooltip_text(start, TEXT_START_TRACKING_TOOLTIP);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "setup-actions");
	gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(box), start);
	gtk_box_append(GTK_BOX(box), create_open_button(TEXT_OPEN_OTHER_FOLDER, FALSE));
	GtkWidget *page = create_status_page(ILLUSTRATION_SETUP, "", TEXT_SETUP_DESCRIPTION);
	adw_status_page_set_child(ADW_STATUS_PAGE(page), box);
	g_object_set_data(G_OBJECT(page), WINDOW_KEY, window);
	g_signal_connect(start, "clicked", G_CALLBACK(press_start), page);
	return page;
}


void set_setup_page_folder(GtkWidget *page, const char *folder) {
	g_object_set_data_full(G_OBJECT(page), FOLDER_KEY, g_strdup(folder), g_free);
	g_autofree char *name = g_path_get_basename(folder);
	g_autofree char *title = g_strdup_printf(TEXT_SETUP_TITLE, name);
	adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
}


/* A folder that cannot be shown */

GtkWidget *create_problem_page(Window *window) {
	GtkWidget *page = create_status_page(ILLUSTRATION_PROBLEM, "", "");
	adw_status_page_set_child(ADW_STATUS_PAGE(page), create_open_button(TEXT_OPEN_OTHER_FOLDER, TRUE));
	return page;
}


/* A status page reads its description as markup, so git's own words are escaped */
void set_problem_page_text(GtkWidget *page, const char *title, const char *description) {
	g_autofree char *escaped = g_markup_escape_text(description ? description : "", -1);
	adw_status_page_set_title(ADW_STATUS_PAGE(page), title);
	adw_status_page_set_description(ADW_STATUS_PAGE(page), escaped);
}
