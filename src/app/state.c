#include <string.h>

#include "common/storage/key-store.h"
#include "app/state.h"

#define STATE_FILE "window.ini"
#define RECENT_FILE "recent.ini"
#define STATE_GROUP "window"
#define RECENT_GROUP "recent"
#define RECENT_KEY "repositories"
#define DEFAULT_WIDTH 1280
#define DEFAULT_HEIGHT 820
/* Enough to find the few projects people switch between, without a list to scroll */
#define MAXIMUM_RECENT 8

static GStrv recent;


void load_window_state(WindowState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	load_window_geometry(file, &state->geometry, DEFAULT_WIDTH, DEFAULT_HEIGHT);
	state->sidebar_width = read_key_integer(file, STATE_GROUP, "sidebar-width", SIDEBAR_WIDTH, SIDEBAR_MINIMUM,
		SIDEBAR_MAXIMUM);
}


void save_window_state(const WindowState *state) {
	g_autofree char *path = get_state_file_path(STATE_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	store_window_geometry(file, &state->geometry);
	g_key_file_set_integer(file, STATE_GROUP, "sidebar-width", state->sidebar_width);
	save_key_file(file, path);
}


static void load_recent(void) {
	if (recent)
		return;
	g_autofree char *path = get_state_file_path(RECENT_FILE);
	g_autoptr(GKeyFile) file = load_key_file(path, G_KEY_FILE_NONE);
	recent = g_key_file_get_string_list(file, RECENT_GROUP, RECENT_KEY, NULL, NULL);
	if (!recent)
		recent = g_new0(char *, 1);
}


static void save_recent(void) {
	g_autofree char *path = get_state_file_path(RECENT_FILE);
	g_autoptr(GKeyFile) file = g_key_file_new();
	g_key_file_set_string_list(file, RECENT_GROUP, RECENT_KEY, (const char *const *)recent, g_strv_length(recent));
	save_key_file(file, path);
}


const char *const *get_recent_repositories(void) {
	load_recent();
	return (const char *const *)recent;
}


/* The list without the folder, so it can be put back at the front or left out */
static GPtrArray *copy_without(const char *folder) {
	GPtrArray *kept = g_ptr_array_new();
	for (char **item = recent; *item; item++)
		if (strcmp(*item, folder) != 0)
			g_ptr_array_add(kept, g_strdup(*item));
	return kept;
}


static void replace_recent(GPtrArray *items) {
	while (items->len > MAXIMUM_RECENT)
		g_free(g_ptr_array_steal_index(items, items->len - 1));
	g_ptr_array_add(items, NULL);
	g_strfreev(recent);
	recent = (GStrv)g_ptr_array_free(items, FALSE);
	save_recent();
}


void add_recent_repository(const char *folder) {
	load_recent();
	GPtrArray *items = copy_without(folder);
	g_ptr_array_insert(items, 0, g_strdup(folder));
	replace_recent(items);
}


void forget_recent_repository(const char *folder) {
	load_recent();
	replace_recent(copy_without(folder));
}
