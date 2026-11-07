#include <gtk/gtk.h>

#include "common/storage/key-store.h"
#include "app/settings.h"
#include "git/repository.h"

#define SETTINGS_FILE "settings.ini"
#define SETTINGS_GROUP "settings"
#define PERCENT 100

static Settings settings;
static gboolean has_loaded;
static GtkCssProvider *style;

typedef struct {
	SettingsChanged changed;
	gpointer data;
} Listener;

static GArray *listeners;


static void load_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	settings.code_size = read_key_integer(file, SETTINGS_GROUP, "code-size", DEFAULT_TEXT_SIZE, MINIMUM_TEXT_SIZE,
		MAXIMUM_TEXT_SIZE);
	settings.wraps_lines = read_key_boolean(file, SETTINGS_GROUP, "wrap-lines", FALSE);
	settings.ignores_whitespace = read_key_boolean(file, SETTINGS_GROUP, "ignore-whitespace", FALSE);
	settings.context_lines = read_key_integer(file, SETTINGS_GROUP, "context-lines", DEFAULT_CONTEXT_LINES, 0,
		MAXIMUM_CONTEXT_LINES);
	settings.shows_all_branches = read_key_boolean(file, SETTINGS_GROUP, "all-branches", FALSE);
	settings.reopens_last = read_key_boolean(file, SETTINGS_GROUP, "reopen-last", FALSE);
	settings.fetches_automatically = read_key_boolean(file, SETTINGS_GROUP, "fetch-automatically", TRUE);
}


Settings *get_settings(void) {
	if (!has_loaded) {
		has_loaded = TRUE;
		load_settings();
	}
	return &settings;
}


void save_settings(void) {
	g_autofree char *path = get_config_file_path(SETTINGS_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_integer(file, SETTINGS_GROUP, "code-size", settings.code_size);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "wrap-lines", settings.wraps_lines);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "ignore-whitespace", settings.ignores_whitespace);
	g_key_file_set_integer(file, SETTINGS_GROUP, "context-lines", settings.context_lines);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "all-branches", settings.shows_all_branches);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "reopen-last", settings.reopens_last);
	g_key_file_set_boolean(file, SETTINGS_GROUP, "fetch-automatically", settings.fetches_automatically);
	save_key_file(file, path);
}


void apply_settings(void) {
	Settings *current = get_settings();
	if (!style) {
		style = gtk_css_provider_new();
		gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(style),
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
	}
	g_autofree char *css = g_strdup_printf(".code { font-size: calc(var(--monospace-font-size) * %d / %d); }",
		current->code_size * CODE_SIZE_BASE, PERCENT * PERCENT);
	gtk_css_provider_load_from_string(style, css);
	for (guint index = 0; listeners && index < listeners->len; index++) {
		Listener *listener = &g_array_index(listeners, Listener, index);
		listener->changed(listener->data);
	}
}


void watch_settings(SettingsChanged changed, gpointer data) {
	if (!listeners)
		listeners = g_array_new(FALSE, FALSE, sizeof(Listener));
	Listener listener = {changed, data};
	g_array_append_val(listeners, listener);
}


void unwatch_settings(SettingsChanged changed, gpointer data) {
	for (guint index = 0; listeners && index < listeners->len; index++) {
		Listener *listener = &g_array_index(listeners, Listener, index);
		if (listener->changed == changed && listener->data == data) {
			g_array_remove_index(listeners, index);
			return;
		}
	}
}
