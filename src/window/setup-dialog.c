#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "git/runner.h"
#include "window/setup-dialog.h"

#define DIALOG_WIDTH 520
#define DEFAULT_BRANCH "main"
#define IGNORE_FILE ".gitignore"
#define LOG_SUFFIX ".log"
#define OBJECT_SUFFIX ".o"

/* Folders and files that are made by tools rather than written by people, and what to write to leave them out */
static const struct {
	const char *name, *pattern;
} MADE_BY_TOOLS[] = {
	{"node_modules", "node_modules/"}, {"build", "build/"}, {"dist", "dist/"}, {"target", "target/"},
	{"out", "out/"}, {".venv", ".venv/"}, {"venv", "venv/"}, {"__pycache__", "__pycache__/"},
	{".cache", ".cache/"}, {".idea", ".idea/"}, {".DS_Store", ".DS_Store"}, {"Thumbs.db", "Thumbs.db"},
};

typedef struct {
	Window *window;
	char *folder;
	AdwDialog *dialog;
	GtkWidget *name_row, *email_row, *branch_row, *commit_row, *start_button;
	/* The switch rows of the patterns found, each with its pattern */
	GPtrArray *ignore_rows;
	/* The steps still to run, in order, each a NULL ended list of arguments */
	GPtrArray *steps;
} Setup;


static void free_setup(gpointer data) {
	Setup *setup = data;
	release_window(setup->window);
	g_free(setup->folder);
	g_ptr_array_unref(setup->ignore_rows);
	if (setup->steps)
		g_ptr_array_unref(setup->steps);
	g_free(setup);
}


static char *read_global_setting(const char *key) {
	const char *arguments[] = {"config", "--global", "--get", key, NULL};
	GitResult *result = run_git_now(g_get_home_dir(), arguments, NULL);
	char *value = has_git_succeeded(result) ? get_git_output_text(result) : g_strdup("");
	free_git_result(result);
	return value;
}


static GtkWidget *create_entry_row(const char *title, const char *text) {
	GtkWidget *row = adw_entry_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	gtk_editable_set_text(GTK_EDITABLE(row), text);
	return row;
}


static void add_step(Setup *setup, const char *first, ...) {
	GPtrArray *step = g_ptr_array_new_with_free_func(g_free);
	va_list list;
	va_start(list, first);
	for (const char *argument = first; argument; argument = va_arg(list, const char *))
		g_ptr_array_add(step, g_strdup(argument));
	va_end(list);
	g_ptr_array_add(step, NULL);
	g_ptr_array_add(setup->steps, step);
}


static void run_next_step(Setup *setup);


static void finish_step(GitResult *result, gpointer data) {
	Setup *setup = data;
	if (!has_git_succeeded(result)) {
		show_git_failure(setup->window, TEXT_SETUP_FAILED, result);
		gtk_widget_set_sensitive(setup->start_button, TRUE);
		g_clear_pointer(&setup->steps, g_ptr_array_unref);
		g_object_unref(setup->dialog);
		return;
	}
	g_ptr_array_remove_index(setup->steps, 0);
	run_next_step(setup);
}


/* Each step waits for the one before, so the first commit only starts once the repository is ready */
static void run_next_step(Setup *setup) {
	if (setup->steps->len == 0) {
		adw_dialog_close(setup->dialog);
		open_folder(setup->window, setup->folder);
		g_object_unref(setup->dialog);
		return;
	}
	GPtrArray *step = g_ptr_array_index(setup->steps, 0);
	run_git(setup->folder, (const char *const *)step->pdata, NULL, finish_step, setup);
}


static gboolean write_ignore_file(Setup *setup) {
	g_autoptr(GString) text = g_string_new(NULL);
	for (guint index = 0; index < setup->ignore_rows->len; index++) {
		GtkWidget *row = g_ptr_array_index(setup->ignore_rows, index);
		if (adw_switch_row_get_active(ADW_SWITCH_ROW(row)))
			g_string_append_printf(text, "%s\n", (const char *)g_object_get_data(G_OBJECT(row), "pattern"));
	}
	if (text->len == 0)
		return TRUE;
	g_autofree char *path = g_build_filename(setup->folder, IGNORE_FILE, NULL);
	g_autoptr(GError) error = NULL;
	if (!g_file_set_contents(path, text->str, (gssize)text->len, &error)) {
		g_warning("%s could not be written: %s", path, error->message);
		show_toast(setup->window, TEXT_SETUP_FAILED);
		return FALSE;
	}
	return TRUE;
}


static void press_start(GtkButton *button, Setup *setup) {
	const char *branch = gtk_editable_get_text(GTK_EDITABLE(setup->branch_row));
	if (!write_ignore_file(setup))
		return;
	gtk_widget_set_sensitive(setup->start_button, FALSE);
	g_clear_pointer(&setup->steps, g_ptr_array_unref);
	setup->steps = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
	add_step(setup, "init", "--quiet", "--initial-branch", *branch ? branch : DEFAULT_BRANCH, NULL);
	/* Who commits is stored for every repository, as git asks the first time it is used */
	if (gtk_widget_get_visible(setup->name_row)) {
		add_step(setup, "config", "--global", "user.name", gtk_editable_get_text(GTK_EDITABLE(setup->name_row)),
			NULL);
		add_step(setup, "config", "--global", "user.email", gtk_editable_get_text(GTK_EDITABLE(setup->email_row)),
			NULL);
	}
	if (adw_switch_row_get_active(ADW_SWITCH_ROW(setup->commit_row))) {
		add_step(setup, "add", "--all", NULL);
		add_step(setup, "commit", "--quiet", "--allow-empty", "-m", TEXT_FIRST_COMMIT_MESSAGE, NULL);
	}
	/* The dialog stays alive until the last step answers, even if it is closed */
	g_object_ref(setup->dialog);
	run_next_step(setup);
}


static void check_ready(Setup *setup) {
	gboolean has_identity = !gtk_widget_get_visible(setup->name_row) ||
		(*gtk_editable_get_text(GTK_EDITABLE(setup->name_row)) &&
		strchr(gtk_editable_get_text(GTK_EDITABLE(setup->email_row)), '@'));
	gtk_widget_set_sensitive(setup->start_button, has_identity);
}


static GtkWidget *create_identity_group(Setup *setup) {
	g_autofree char *name = read_global_setting("user.name");
	g_autofree char *email = read_global_setting("user.email");
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_SETUP_IDENTITY);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), TEXT_SETUP_IDENTITY_DETAIL);
	setup->name_row = create_entry_row(TEXT_SETUP_NAME, name);
	setup->email_row = create_entry_row(TEXT_SETUP_EMAIL, email);
	g_signal_connect_swapped(setup->name_row, "changed", G_CALLBACK(check_ready), setup);
	g_signal_connect_swapped(setup->email_row, "changed", G_CALLBACK(check_ready), setup);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), setup->name_row);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), setup->email_row);
	/* Someone who already told git who they are is not asked again */
	gboolean is_known = *name && *email;
	gtk_widget_set_visible(group, !is_known);
	gtk_widget_set_visible(setup->name_row, !is_known);
	return group;
}


static gboolean has_suffix_file(const char *folder, const char *suffix) {
	g_autoptr(GDir) directory = g_dir_open(folder, 0, NULL);
	const char *name;
	while (directory && (name = g_dir_read_name(directory)))
		if (g_str_has_suffix(name, suffix))
			return TRUE;
	return FALSE;
}


static void add_ignore_row(Setup *setup, GtkWidget *group, const char *pattern) {
	GtkWidget *row = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), pattern);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_switch_row_set_active(ADW_SWITCH_ROW(row), TRUE);
	g_object_set_data_full(G_OBJECT(row), "pattern", g_strdup(pattern), g_free);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(group), row);
	g_ptr_array_add(setup->ignore_rows, row);
}


/* Only what was found in the folder is offered, and nothing when the folder already says what to leave out */
static GtkWidget *create_ignore_group(Setup *setup) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), TEXT_SETUP_IGNORE);
	adw_preferences_group_set_description(ADW_PREFERENCES_GROUP(group), TEXT_SETUP_IGNORE_DETAIL);
	g_autofree char *existing = g_build_filename(setup->folder, IGNORE_FILE, NULL);
	if (g_file_test(existing, G_FILE_TEST_EXISTS)) {
		gtk_widget_set_visible(group, FALSE);
		return group;
	}
	for (guint index = 0; index < G_N_ELEMENTS(MADE_BY_TOOLS); index++) {
		g_autofree char *path = g_build_filename(setup->folder, MADE_BY_TOOLS[index].name, NULL);
		if (g_file_test(path, G_FILE_TEST_EXISTS))
			add_ignore_row(setup, group, MADE_BY_TOOLS[index].pattern);
	}
	if (has_suffix_file(setup->folder, LOG_SUFFIX))
		add_ignore_row(setup, group, "*" LOG_SUFFIX);
	if (has_suffix_file(setup->folder, OBJECT_SUFFIX))
		add_ignore_row(setup, group, "*" OBJECT_SUFFIX);
	gtk_widget_set_visible(group, setup->ignore_rows->len > 0);
	return group;
}


void show_setup_dialog(Window *window, const char *folder) {
	Setup *setup = g_new0(Setup, 1);
	setup->window = keep_window(window);
	setup->folder = g_strdup(folder);
	setup->ignore_rows = g_ptr_array_new();
	GtkWidget *page = adw_preferences_page_new();
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(create_identity_group(setup)));
	GtkWidget *branch_group = adw_preferences_group_new();
	setup->branch_row = create_entry_row(TEXT_SETUP_BRANCH, DEFAULT_BRANCH);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(branch_group), setup->branch_row);
	setup->commit_row = adw_switch_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(setup->commit_row), TEXT_SETUP_COMMIT);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(setup->commit_row), TEXT_SETUP_COMMIT_DETAIL);
	adw_switch_row_set_active(ADW_SWITCH_ROW(setup->commit_row), TRUE);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(branch_group), setup->commit_row);
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(branch_group));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(create_ignore_group(setup)));
	setup->start_button = gtk_button_new_with_label(TEXT_START_TRACKING);
	gtk_widget_add_css_class(setup->start_button, "suggested-action");
	gtk_widget_set_halign(setup->start_button, GTK_ALIGN_CENTER);
	gtk_widget_set_margin_top(setup->start_button, SPACING_NORMAL);
	gtk_widget_set_margin_bottom(setup->start_button, SPACING_LARGE);
	g_signal_connect(setup->start_button, "clicked", G_CALLBACK(press_start), setup);
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
	adw_toolbar_view_add_bottom_bar(ADW_TOOLBAR_VIEW(toolbar), setup->start_button);
	setup->dialog = adw_dialog_new();
	adw_dialog_set_title(setup->dialog, TEXT_SETUP_DIALOG_TITLE);
	adw_dialog_set_content_width(setup->dialog, DIALOG_WIDTH);
	adw_dialog_set_child(setup->dialog, toolbar);
	g_object_set_data_full(G_OBJECT(setup->dialog), "setup", setup, free_setup);
	check_ready(setup);
	adw_dialog_present(setup->dialog, get_window_widget(window));
}
