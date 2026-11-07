#pragma once

#include <glib.h>

#define MINIMUM_TEXT_SIZE 50
#define MAXIMUM_TEXT_SIZE 300
#define DEFAULT_TEXT_SIZE 100
/* The share of the system monospace font that a code size of 100% stands for, since the full size reads large */
#define CODE_SIZE_BASE 85
#define MAXIMUM_CONTEXT_LINES 20

/* Choices made in Settings, kept between launches */
typedef struct {
	/* The size of the code in diffs and blame, as a percentage of CODE_SIZE_BASE */
	int code_size;
	gboolean wraps_lines;
	gboolean ignores_whitespace;
	/* Unchanged lines shown around each change */
	int context_lines;
	/* The timeline shows every branch instead of only the one checked out */
	gboolean shows_all_branches;
	/* The window opens the last repository instead of asking for a folder */
	gboolean reopens_last;
	/* New commits on the remote are looked for when a repository opens and when the window comes back into focus */
	gboolean fetches_automatically;
} Settings;

typedef void (*SettingsChanged)(gpointer data);

Settings *get_settings(void);
void save_settings(void);
/* Applies the sizes and tells every listener, such as each window, which reads the diffs again */
void apply_settings(void);
void watch_settings(SettingsChanged changed, gpointer data);
void unwatch_settings(SettingsChanged changed, gpointer data);
