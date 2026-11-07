#include <adwaita.h>

#include "lang/en.h"
#include "app/application.h"
#include "app/settings.h"
#include "app/settings-dialog.h"
#include "app/state.h"
#include "git/features.h"
#include "view/colours.h"
#include "window/window.h"


/* A window with nothing open yet takes the folder, and each other folder gets a window of its own */
static Window *find_empty_window(GtkApplication *application) {
	for (GList *item = gtk_application_get_windows(application); item; item = item->next) {
		Window *window = g_object_get_data(G_OBJECT(item->data), "window");
		if (window && !has_window_folder(window))
			return window;
	}
	return NULL;
}


static void activate(GApplication *application) {
	GtkWindow *active = gtk_application_get_active_window(GTK_APPLICATION(application));
	if (active) {
		gtk_window_present(active);
		return;
	}
	Window *window = create_window(GTK_APPLICATION(application));
	gtk_window_present(get_gtk_window(window));
	const char *const *recent = get_recent_repositories();
	/* Like the other Crimson apps, a window opens on its start page, and the folder dialog only opens when asked */
	if (get_settings()->reopens_last && recent[0] && g_file_test(recent[0], G_FILE_TEST_IS_DIR))
		open_folder(window, recent[0]);
}


static void open_files(GApplication *application, GFile **files, int count, const char *hint) {
	for (int index = 0; index < count; index++) {
		g_autofree char *path = g_file_get_path(files[index]);
		if (!path)
			continue;
		Window *window = find_empty_window(GTK_APPLICATION(application));
		if (!window)
			window = create_window(GTK_APPLICATION(application));
		gtk_window_present(get_gtk_window(window));
		open_folder(window, path);
	}
}


static void start_up(GApplication *application) {
	load_colour_variables();
	apply_settings();
	/* The installed git's version decides what menus offer, so it is read once before any window opens */
	get_git_features();
}


static const Shortcut SHORTCUTS[] = {
	{"win.open", {"<Control>o", NULL}},
	{"win.search", {"<Control>f", NULL}},
	{"win.branches", {"<Control>b", NULL}},
	{"win.refresh", {"F5", "<Control>r", NULL}},
	{"win.info", {"<Control>i", NULL}},
	{"win.now", {"<Alt>Home", NULL}},
	{"win.back", {"<Alt>Left", NULL}},
	{"win.close-folder", {"<Control>w", NULL}},
};

const ApplicationSetup APPLICATION_SETUP = {
	.identity = {"io.github.CrimsonOrg.CrimsonChanges", TEXT_APPLICATION_NAME, "crimson-changes",
		APPLICATION_VERSION},
	.start_up = start_up,
	.activate = activate,
	.open = open_files,
	.parameter_summary = TEXT_FOLDER_OPTION,
	.show_settings = show_settings,
	.shortcuts = SHORTCUTS,
	.shortcut_count = G_N_ELEMENTS(SHORTCUTS),
};
