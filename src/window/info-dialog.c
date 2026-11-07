#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "common/text/numbers.h"
#include "git/features.h"
#include "view/format.h"
#include "view/file-actions.h"
#include "window/info-dialog.h"

#define DIALOG_WIDTH 560
#define DIALOG_HEIGHT 640
#define WORKTREE_MARK "worktree "
#define FETCH_MARK " (fetch)"

typedef struct {
	Window *window;
	AdwDialog *dialog;
	GtkWidget *remotes, *sizes, *worktrees, *submodules;
} InfoDialog;


static GtkWidget *create_value_row(const char *title, const char *value) {
	GtkWidget *row = adw_action_row_new();
	adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
	adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(row), FALSE);
	adw_action_row_set_subtitle(ADW_ACTION_ROW(row), value);
	adw_action_row_set_subtitle_selectable(ADW_ACTION_ROW(row), TRUE);
	gtk_widget_add_css_class(row, "property");
	return row;
}


static GtkWidget *create_group(const char *title) {
	GtkWidget *group = adw_preferences_group_new();
	adw_preferences_group_set_title(ADW_PREFERENCES_GROUP(group), title);
	return group;
}


/* Each "name url (fetch)" line names one remote */
static void read_remotes_now(GitResult *result, InfoDialog *info) {
	if (result->is_cancelled)
		return;
	g_autofree char *text = get_git_output_text(result);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	guint count = 0;
	for (char **line = lines; *line; line++) {
		if (!g_str_has_suffix(*line, FETCH_MARK))
			continue;
		(*line)[strlen(*line) - strlen(FETCH_MARK)] = 0;
		g_auto(GStrv) parts = g_strsplit_set(*line, " \t", 2);
		if (g_strv_length(parts) == 2) {
			adw_preferences_group_add(ADW_PREFERENCES_GROUP(info->remotes), create_value_row(parts[0], parts[1]));
			count++;
		}
	}
	gtk_widget_set_visible(info->remotes, count > 0);
}


static guint64 read_value(GHashTable *values, const char *key) {
	const char *value = g_hash_table_lookup(values, key);
	return value ? g_ascii_strtoull(value, NULL, 10) : 0;
}


static void add_count_row(InfoDialog *info, const char *title, guint64 count) {
	g_autofree char *text = format_count(count);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(info->sizes), create_value_row(title, text));
}


/* Lines such as "objects.commits.count=88" */
static void read_structure_now(GitResult *result, InfoDialog *info) {
	if (result->is_cancelled || !has_git_succeeded(result))
		return;
	g_autofree char *text = get_git_output_text(result);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	g_autoptr(GHashTable) values = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
	for (char **line = lines; *line; line++) {
		char *separator = strchr(*line, '=');
		if (separator)
			g_hash_table_insert(values, g_strndup(*line, (gsize)(separator - *line)), g_strdup(separator + 1));
	}
	add_count_row(info, TEXT_INFO_COMMITS, read_value(values, "objects.commits.count"));
	add_count_row(info, TEXT_INFO_BRANCHES, read_value(values, "references.branches.count"));
	add_count_row(info, TEXT_INFO_TAGS, read_value(values, "references.tags.count"));
	guint64 disk = read_value(values, "objects.commits.disk_size") + read_value(values, "objects.trees.disk_size") +
		read_value(values, "objects.blobs.disk_size") + read_value(values, "objects.tags.disk_size");
	g_autofree char *size = format_size(disk);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(info->sizes), create_value_row(TEXT_INFO_SIZE, size));
	gtk_widget_set_visible(info->sizes, TRUE);
}


static void read_count_now(GitResult *result, InfoDialog *info) {
	if (result->is_cancelled || !has_git_succeeded(result))
		return;
	g_autofree char *text = get_git_output_text(result);
	add_count_row(info, TEXT_INFO_COMMITS, g_ascii_strtoull(text, NULL, 10));
	gtk_widget_set_visible(info->sizes, TRUE);
}


static void read_worktrees_now(GitResult *result, InfoDialog *info) {
	if (result->is_cancelled || !has_git_succeeded(result))
		return;
	g_autofree char *text = get_git_output_text(result);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	guint count = 0;
	for (char **line = lines; *line; line++)
		if (g_str_has_prefix(*line, WORKTREE_MARK)) {
			g_autofree char *name = g_path_get_basename(*line + strlen(WORKTREE_MARK));
			adw_preferences_group_add(ADW_PREFERENCES_GROUP(info->worktrees),
				create_value_row(name, *line + strlen(WORKTREE_MARK)));
			count++;
		}
	/* One work tree is the folder itself, which the first group already shows */
	gtk_widget_set_visible(info->worktrees, count > 1);
}


/* Each read holds the dialog, so closing it early never leaves an answer without a place to go */
typedef void (*InfoReader)(GitResult *result, InfoDialog *info);


typedef struct {
	AdwDialog *dialog;
	InfoReader reader;
} InfoRead;


static void deliver(GitResult *result, gpointer data) {
	InfoRead *read = data;
	if (!result->is_cancelled)
		read->reader(result, g_object_get_data(G_OBJECT(read->dialog), "info"));
	g_object_unref(read->dialog);
	g_free(read);
}


static void read_into(InfoDialog *info, const char *const *arguments, InfoReader reader) {
	InfoRead *read = g_new0(InfoRead, 1);
	read->dialog = g_object_ref(info->dialog);
	read->reader = reader;
	read_repository(get_window_repository(info->window), arguments, deliver, read);
}


/* Lines such as " 1a2b3c4 libs/engine (v1.2)", with a mark first when the folder holds another commit */
static void read_submodules_now(GitResult *result, InfoDialog *info) {
	if (result->is_cancelled || !has_git_succeeded(result))
		return;
	g_autofree char *text = get_git_output_text(result);
	g_auto(GStrv) lines = g_strsplit(text, "\n", -1);
	guint count = 0;
	for (char **line = lines; *line; line++) {
		g_auto(GStrv) parts = g_strsplit(g_strchug(*line + (**line ? 1 : 0)), " ", 3);
		if (g_strv_length(parts) < 2)
			continue;
		g_autofree char *hash = get_short_hash(parts[0]);
		adw_preferences_group_add(ADW_PREFERENCES_GROUP(info->submodules), create_value_row(parts[1], hash));
		count++;
	}
	gtk_widget_set_visible(info->submodules, count > 0);
}


static void open_folder_row(AdwActionRow *row, InfoDialog *info) {
	gtk_widget_activate_action(get_window_widget(info->window), "win.files", NULL);
}


void show_info_dialog(Window *window) {
	Repository *repository = get_window_repository(window);
	if (!repository)
		return;
	InfoDialog *info = g_new0(InfoDialog, 1);
	info->window = window;
	GtkWidget *place = create_group(TEXT_INFO_PLACE);
	GtkWidget *folder = create_value_row(TEXT_INFO_FOLDER, get_repository_folder(repository));
	GtkWidget *open = gtk_button_new_from_icon_name("folder-open-symbolic");
	gtk_widget_set_tooltip_text(open, TEXT_OPEN_IN_FILES);
	gtk_widget_set_valign(open, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(open, "flat");
	g_signal_connect_swapped(open, "clicked", G_CALLBACK(open_folder_row), info);
	adw_action_row_add_suffix(ADW_ACTION_ROW(folder), open);
	adw_preferences_group_add(ADW_PREFERENCES_GROUP(place), folder);
	info->remotes = create_group(TEXT_INFO_REMOTES);
	gtk_widget_set_visible(info->remotes, FALSE);
	info->sizes = create_group(TEXT_INFO_SIZES);
	gtk_widget_set_visible(info->sizes, FALSE);
	info->worktrees = create_group(TEXT_INFO_WORKTREES);
	gtk_widget_set_visible(info->worktrees, FALSE);
	info->submodules = create_group(TEXT_INFO_SUBMODULES);
	gtk_widget_set_visible(info->submodules, FALSE);
	GtkWidget *page = adw_preferences_page_new();
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(place));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(info->remotes));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(info->sizes));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(info->worktrees));
	adw_preferences_page_add(ADW_PREFERENCES_PAGE(page), ADW_PREFERENCES_GROUP(info->submodules));
	GtkWidget *toolbar = adw_toolbar_view_new();
	adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), adw_header_bar_new());
	adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), page);
	info->dialog = adw_dialog_new();
	adw_dialog_set_title(info->dialog, get_repository_name(repository));
	adw_dialog_set_content_width(info->dialog, DIALOG_WIDTH);
	adw_dialog_set_content_height(info->dialog, DIALOG_HEIGHT);
	adw_dialog_set_child(info->dialog, toolbar);
	g_object_set_data_full(G_OBJECT(info->dialog), "info", info, g_free);
	const char *remotes[] = {"remote", "-v", NULL};
	read_into(info, remotes, read_remotes_now);
	if (get_git_features()->can_describe_structure) {
		const char *structure[] = {"repo", "structure", "--format=lines", NULL};
		read_into(info, structure, read_structure_now);
	} else if (has_repository_commits(repository)) {
		const char *count[] = {"rev-list", "--count", "HEAD", NULL};
		read_into(info, count, read_count_now);
	}
	const char *worktrees[] = {"worktree", "list", "--porcelain", NULL};
	read_into(info, worktrees, read_worktrees_now);
	const char *submodules[] = {"submodule", "status", NULL};
	read_into(info, submodules, read_submodules_now);
	adw_dialog_present(info->dialog, get_window_widget(window));
}
