#include <adwaita.h>

#include "lang/en.h"
#include "app/settings.h"
#include "app/settings-dialog.h"

#define SIZE_STEP 10
#define DIALOG_WIDTH 560
#define PERCENT_SIGN '%'


static void store_settings(void) {
	save_settings();
	apply_settings();
}


static void change_number(AdwSpinRow *row, GParamSpec *property, int *value) {
	*value = (int)adw_spin_row_get_value(row);
	store_settings();
}


static gboolean show_percent(AdwSpinRow *row, gpointer data) {
	g_autofree char *text = g_strdup_printf(TEXT_PERCENT, (int)adw_spin_row_get_value(row));
	gtk_editable_set_text(GTK_EDITABLE(row), text);
	return TRUE;
}


/* A typed size may end with its percent sign */
static int read_percent(AdwSpinRow *row, double *value, gpointer data) {
	const char *text = gtk_editable_get_text(GTK_EDITABLE(row));
	char *end = NULL;
	*value = g_ascii_strtod(text, &end);
	while (g_ascii_isspace(*end))
		end++;
	if (*end == PERCENT_SIGN)
		end++;
	return end != text && *end == 0 ? TRUE : GTK_INPUT_ERROR;
}


static GtkWidget *create_spin_row(const char *title, const char *subtitle, int minimum, int maximum, int step,
	int *value) {
	GtkWidget *row = adw_spin_row_new_with_range(minimum, maximum, step);
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	adw_spin_row_set_value(ADW_SPIN_ROW(row), *value);
	g_signal_connect(row, "notify::value", G_CALLBACK(change_number), value);
	return row;
}


static GtkWidget *create_size_row(int *size) {
	GtkWidget *row = create_spin_row(TEXT_CODE_SIZE, TEXT_CODE_SIZE_DETAIL, MINIMUM_TEXT_SIZE, MAXIMUM_TEXT_SIZE,
		SIZE_STEP, size);
	adw_spin_row_set_numeric(ADW_SPIN_ROW(row), FALSE);
	g_signal_connect(row, "output", G_CALLBACK(show_percent), NULL);
	g_signal_connect(row, "input", G_CALLBACK(read_percent), NULL);
	show_percent(ADW_SPIN_ROW(row), NULL);
	return row;
}


static void change_switch(AdwSwitchRow *row, GParamSpec *property, gboolean *value) {
	*value = adw_switch_row_get_active(row);
	store_settings();
}


static GtkWidget *create_switch_row(const char *title, const char *subtitle, gboolean *value) {
	GtkWidget *row = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	if (subtitle)
		adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
	adw_switch_row_set_active(ADW_SWITCH_ROW(row), *value);
	g_signal_connect(row, "notify::active", G_CALLBACK(change_switch), value);
	return row;
}


static GtkWidget *create_group(const char *title) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	return group;
}


void show_settings(GtkWidget *parent) {
	Settings *settings = get_settings();
	GtkWidget *diffs = create_group(TEXT_SETTINGS_DIFFS);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(diffs), create_size_row(&settings->code_size));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(diffs), create_switch_row(TEXT_WRAP_LINES, NULL,
		&settings->wraps_lines));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(diffs), create_switch_row(TEXT_IGNORE_WHITESPACE,
		TEXT_IGNORE_WHITESPACE_DETAIL, &settings->ignores_whitespace));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(diffs), create_spin_row(TEXT_CONTEXT_LINES,
		TEXT_CONTEXT_LINES_DETAIL, 0, MAXIMUM_CONTEXT_LINES, 1, &settings->context_lines));
	GtkWidget *history = create_group(TEXT_SETTINGS_HISTORY);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(history), create_switch_row(TEXT_ALL_BRANCHES,
		TEXT_ALL_BRANCHES_DETAIL, &settings->shows_all_branches));
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(history), create_switch_row(TEXT_FETCH_AUTOMATICALLY,
		TEXT_FETCH_AUTOMATICALLY_DETAIL, &settings->fetches_automatically));
	GtkWidget *starting = create_group(TEXT_SETTINGS_STARTING);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(starting), create_switch_row(TEXT_REOPEN_LAST,
		TEXT_REOPEN_LAST_DETAIL, &settings->reopens_last));
	GtkWidget *page = adw_preferences_page_new();
	gtk_widget_set_size_request(page, DIALOG_WIDTH, -1);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(diffs));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(history));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(starting));
	AdwDialog *dialog = adw_preferences_dialog_new();
	adw_dialog_set_title(dialog, TEXT_SETTINGS);
	adw_dialog_set_follows_content_size(dialog, TRUE);
	adw_preferences_dialog_add(ADW_PREFERENCES_DIALOG(dialog), ADW_PREFERENCES_PAGE(page));
	adw_dialog_present(dialog, parent);
}
