#include <string.h>
#include <glib/gstdio.h>

#include "lang/en.h"
#include "git/features.h"
#include "view/dialogs.h"
#include "view/file-actions.h"
#include "view/commit-menu.h"

#define MENU_KEY "commit-menu"
#define ACTION_GROUP "commit"
#define SOFT_RESPONSE "soft"
#define MIXED_RESPONSE "mixed"
#define HARD_RESPONSE "hard"
#define CANCEL_RESPONSE "cancel"
#define MESSAGE_FILE_PATTERN "crimson-changes-message-XXXXXX"

typedef struct {
	Window *window;
	GtkWidget *button;
	Commit *commit;
	char *stash_name;
	GSimpleActionGroup *group;
} CommitMenu;

/* A command run for the menu, with what to say if it fails or works */
typedef struct {
	Window *window;
	char *failure, *success;
	/* A file to remove once the command ended, such as a prepared message */
	char *temporary_file;
} MenuCommand;


static CommitMenu *get_menu(GtkWidget *button) {
	return g_object_get_data(G_OBJECT(button), MENU_KEY);
}


static void finish_menu_command(GitResult *result, gpointer data) {
	MenuCommand *command = data;
	if (command->temporary_file)
		g_unlink(command->temporary_file);
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(command->window, command->failure, result);
	else if (!result->is_cancelled && command->success)
		show_toast(command->window, command->success);
	release_window(command->window);
	g_free(command->failure);
	g_free(command->success);
	g_free(command->temporary_file);
	g_free(command);
}


static void run_menu_command(Window *window, const char *const *arguments, const GitOptions *options,
	const char *failure, const char *success) {
	MenuCommand *command = g_new0(MenuCommand, 1);
	command->window = keep_window(window);
	command->failure = g_strdup(failure);
	command->success = g_strdup(success);
	run_repository_command(get_window_repository(window), arguments, options, finish_menu_command, command);
}


static void copy_hash(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	copy_text(menu->window, menu->commit->hash, TEXT_HASH_COPIED);
}


/* Names typed for branches and tags */

typedef struct {
	Window *window;
	char *commit;
	gboolean is_tag;
} NameRequest;


static void free_name_request(gpointer data) {
	NameRequest *request = data;
	g_free(request->commit);
	g_free(request);
}


static void create_named(const char *name, gpointer data) {
	NameRequest *request = data;
	if (request->is_tag) {
		const char *arguments[] = {"tag", name, request->commit, NULL};
		run_menu_command(request->window, arguments, NULL, TEXT_TAG_FAILED, TEXT_TAG_MADE);
	} else {
		const char *arguments[] = {"switch", "-c", name, request->commit, NULL};
		run_menu_command(request->window, arguments, NULL, TEXT_BRANCH_FAILED, NULL);
	}
}


static void ask_for_name(CommitMenu *menu, gboolean is_tag) {
	NameRequest *request = g_new0(NameRequest, 1);
	request->window = menu->window;
	request->commit = g_strdup(menu->commit->hash);
	request->is_tag = is_tag;
	ask_for_text(get_window_widget(menu->window), is_tag ? TEXT_NEW_TAG_HEADING : TEXT_NEW_BRANCH_HEADING,
		is_tag ? TEXT_NEW_TAG_BODY : TEXT_NEW_BRANCH_HERE_BODY, NULL, is_tag ? TEXT_CREATE_TAG : TEXT_CREATE_BRANCH,
		create_named, request, free_name_request);
}


static void new_branch(GSimpleAction *action, GVariant *parameter, gpointer data) {
	ask_for_name(data, FALSE);
}


static void new_tag(GSimpleAction *action, GVariant *parameter, gpointer data) {
	ask_for_name(data, TRUE);
}


static void switch_to_commit(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	const char *arguments[] = {"switch", "--detach", menu->commit->hash, NULL};
	run_menu_command(menu->window, arguments, NULL, TEXT_SWITCH_FAILED, TEXT_SWITCHED_TO_COMMIT);
}


/* A merge is undone or copied against its first parent, the branch it was merged into */
static void revert_commit(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	const char *plain[] = {"revert", "--no-edit", menu->commit->hash, NULL};
	const char *merge[] = {"revert", "--no-edit", "-m", "1", menu->commit->hash, NULL};
	run_menu_command(menu->window, is_merge_commit(menu->commit) ? merge : plain, NULL, TEXT_REVERT_FAILED,
		TEXT_REVERTED);
}


static void cherry_pick_commit(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	const char *plain[] = {"cherry-pick", menu->commit->hash, NULL};
	const char *merge[] = {"cherry-pick", "-m", "1", menu->commit->hash, NULL};
	run_menu_command(menu->window, is_merge_commit(menu->commit) ? merge : plain, NULL, TEXT_CHERRY_PICK_FAILED,
		TEXT_CHERRY_PICKED);
}


/* Resetting */

typedef struct {
	Window *window;
	char *commit;
} CommitRequest;


static void free_commit_request(gpointer data) {
	CommitRequest *request = data;
	g_free(request->commit);
	g_free(request);
}


static void choose_reset(AdwAlertDialog *dialog, const char *response, CommitRequest *request) {
	if (strcmp(response, CANCEL_RESPONSE) == 0)
		return;
	g_autofree char *mode = g_strdup_printf("--%s", response);
	const char *arguments[] = {"reset", mode, request->commit, NULL};
	run_menu_command(request->window, arguments, NULL, TEXT_RESET_FAILED, TEXT_RESET_DONE);
}


static void reset_branch(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	AdwDialog *dialog = adw_alert_dialog_new(TEXT_RESET_HEADING, TEXT_RESET_BODY);
	adw_alert_dialog_add_responses(ADW_ALERT_DIALOG(dialog), CANCEL_RESPONSE, TEXT_CANCEL, HARD_RESPONSE,
		TEXT_RESET_HARD, MIXED_RESPONSE, TEXT_RESET_MIXED, SOFT_RESPONSE, TEXT_RESET_SOFT, NULL);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), HARD_RESPONSE, ADW_RESPONSE_DESTRUCTIVE);
	adw_alert_dialog_set_response_appearance(ADW_ALERT_DIALOG(dialog), SOFT_RESPONSE, ADW_RESPONSE_SUGGESTED);
	adw_alert_dialog_set_close_response(ADW_ALERT_DIALOG(dialog), CANCEL_RESPONSE);
	adw_alert_dialog_set_prefer_wide_layout(ADW_ALERT_DIALOG(dialog), TRUE);
	CommitRequest *request = g_new0(CommitRequest, 1);
	request->window = menu->window;
	request->commit = g_strdup(menu->commit->hash);
	g_object_set_data_full(G_OBJECT(dialog), "request", request, free_commit_request);
	g_signal_connect(dialog, "response", G_CALLBACK(choose_reset), request);
	adw_dialog_present(dialog, get_window_widget(menu->window));
}


/* Editing history, which only newer git can do without touching the work folder */

typedef struct {
	Window *window;
	char *commit;
	char *message;
	gboolean is_drop;
} HistoryEdit;


static void free_history_edit(gpointer data) {
	HistoryEdit *edit = data;
	release_window(edit->window);
	g_free(edit->commit);
	g_free(edit->message);
	g_free(edit);
}


static void reword_with(const char *message, gpointer data) {
	HistoryEdit *edit = data;
	g_autofree char *path = NULL;
	g_autoptr(GError) error = NULL;
	int handle = g_file_open_tmp(MESSAGE_FILE_PATTERN, &path, &error);
	if (handle >= 0)
		g_close(handle, NULL);
	if (handle < 0 || !g_file_set_contents(path, message, -1, &error)) {
		g_warning("The new message could not be written: %s", error->message);
		if (path)
			g_unlink(path);
		show_toast(edit->window, TEXT_MESSAGE_FAILED);
		return;
	}
	/* Git opens its editor on the message file, and this editor copies the new message over it */
	g_autofree char *editor = g_strdup_printf("cp '%s'", path);
	GitOptions options = {.editor = editor};
	const char *arguments[] = {"history", "reword", edit->commit, NULL};
	MenuCommand *command = g_new0(MenuCommand, 1);
	command->window = keep_window(edit->window);
	command->failure = g_strdup(TEXT_REWORD_FAILED);
	command->success = g_strdup(TEXT_REWORDED);
	command->temporary_file = g_steal_pointer(&path);
	run_repository_command(get_window_repository(edit->window), arguments, &options, finish_menu_command, command);
}


static void drop_confirmed(gpointer data) {
	HistoryEdit *edit = data;
	const char *arguments[] = {"history", "drop", edit->commit, NULL};
	run_menu_command(edit->window, arguments, NULL, TEXT_DROP_FAILED, TEXT_DROPPED);
}


/* Whether the commit is already on a remote decides how strongly the dialog warns */
static void ask_history_edit(GitResult *result, gpointer data) {
	HistoryEdit *edit = data;
	if (result->is_cancelled) {
		free_history_edit(edit);
		return;
	}
	g_autofree char *remotes = get_git_output_text(result);
	gboolean is_pushed = *remotes != 0;
	GtkWidget *parent = get_window_widget(edit->window);
	if (edit->is_drop)
		ask_to_confirm(parent, TEXT_DROP_HEADING, is_pushed ? TEXT_DROP_PUSHED_BODY : TEXT_DROP_BODY, TEXT_DROP,
			TRUE, drop_confirmed, edit, free_history_edit);
	else
		ask_for_message(parent, TEXT_REWORD_HEADING, is_pushed ? TEXT_REWORD_PUSHED_BODY : NULL, edit->message,
			TEXT_REWORD, reword_with, edit, free_history_edit);
}


/* The whole message is read first, since the list of commits only holds each one's first line */
static void read_full_message(GitResult *result, gpointer data) {
	HistoryEdit *edit = data;
	if (result->is_cancelled || !has_git_succeeded(result)) {
		if (!result->is_cancelled)
			show_git_failure(edit->window, TEXT_COMMIT_READ_FAILED, result);
		free_history_edit(edit);
		return;
	}
	edit->message = get_git_output_text(result);
	const char *arguments[] = {"branch", "-r", "--contains", edit->commit, NULL};
	read_repository(get_window_repository(edit->window), arguments, ask_history_edit, edit);
}


static void start_history_edit(CommitMenu *menu, gboolean is_drop) {
	HistoryEdit *edit = g_new0(HistoryEdit, 1);
	edit->window = keep_window(menu->window);
	edit->commit = g_strdup(menu->commit->hash);
	edit->is_drop = is_drop;
	const char *arguments[] = {"show", "-s", "--format=%B", menu->commit->hash, NULL};
	read_repository(get_window_repository(menu->window), arguments, read_full_message, edit);
}


static void reword_commit(GSimpleAction *action, GVariant *parameter, gpointer data) {
	start_history_edit(data, FALSE);
}


static void drop_commit(GSimpleAction *action, GVariant *parameter, gpointer data) {
	start_history_edit(data, TRUE);
}


/* Stashes */

/* A stash's place in the list moves when another is put aside, so its name is looked up by its commit when used */
static char *find_stash_name(CommitMenu *menu) {
	GPtrArray *stashes = get_repository_stashes(get_window_repository(menu->window));
	for (guint index = 0; index < stashes->len; index++) {
		Stash *stash = g_ptr_array_index(stashes, index);
		if (g_strcmp0(stash->hash, menu->commit->hash) == 0)
			return g_strdup(stash->name);
	}
	show_toast(menu->window, TEXT_STASH_GONE);
	return NULL;
}

static void apply_stash(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	g_autofree char *name = find_stash_name(menu);
	if (!name)
		return;
	const char *arguments[] = {"stash", "apply", name, NULL};
	run_menu_command(menu->window, arguments, NULL, TEXT_STASH_APPLY_FAILED, TEXT_STASH_APPLIED);
}


static void pop_stash(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	g_autofree char *name = find_stash_name(menu);
	if (!name)
		return;
	const char *arguments[] = {"stash", "pop", name, NULL};
	run_menu_command(menu->window, arguments, NULL, TEXT_STASH_APPLY_FAILED, TEXT_STASH_POPPED);
}


/* A dropped stash is still a commit for a while, so Undo stores it again under its old message */
typedef struct {
	Window *window;
	char *hash, *message;
} DroppedStash;


static void free_dropped_stash(gpointer data) {
	DroppedStash *stash = data;
	release_window(stash->window);
	g_free(stash->hash);
	g_free(stash->message);
	g_free(stash);
}


static void restore_stash(gpointer data) {
	DroppedStash *stash = data;
	const char *arguments[] = {"stash", "store", "-m", stash->message, stash->hash, NULL};
	run_menu_command(stash->window, arguments, NULL, TEXT_STASH_DROP_FAILED, NULL);
}


static void finish_stash_drop(GitResult *result, gpointer data) {
	DroppedStash *stash = data;
	if (result->is_cancelled || !has_git_succeeded(result)) {
		if (!result->is_cancelled)
			show_git_failure(stash->window, TEXT_STASH_DROP_FAILED, result);
		free_dropped_stash(stash);
		return;
	}
	show_undo_toast(stash->window, TEXT_STASH_DROPPED, restore_stash, stash, free_dropped_stash);
}


static void drop_stash(GSimpleAction *action, GVariant *parameter, gpointer data) {
	CommitMenu *menu = data;
	g_autofree char *name = find_stash_name(menu);
	if (!name)
		return;
	DroppedStash *stash = g_new0(DroppedStash, 1);
	stash->window = keep_window(menu->window);
	stash->hash = g_strdup(menu->commit->hash);
	stash->message = g_strdup(menu->commit->body && *menu->commit->body ? menu->commit->body : menu->commit->subject);
	const char *arguments[] = {"stash", "drop", name, NULL};
	run_repository_command(get_window_repository(menu->window), arguments, NULL, finish_stash_drop, stash);
}


static GMenuModel *build_commit_menu(void) {
	GMenu *menu = g_menu_new();
	g_autoptr(GMenu) basics = g_menu_new();
	g_menu_append(basics, TEXT_COPY_HASH, ACTION_GROUP ".copy-hash");
	g_menu_append(basics, TEXT_NEW_BRANCH_HERE, ACTION_GROUP ".new-branch");
	g_menu_append(basics, TEXT_NEW_TAG, ACTION_GROUP ".new-tag");
	g_menu_append(basics, TEXT_SWITCH_TO_COMMIT, ACTION_GROUP ".switch");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(basics));
	g_autoptr(GMenu) changes = g_menu_new();
	g_menu_append(changes, TEXT_REVERT, ACTION_GROUP ".revert");
	g_menu_append(changes, TEXT_CHERRY_PICK, ACTION_GROUP ".cherry-pick");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(changes));
	g_autoptr(GMenu) history = g_menu_new();
	const GitFeatures *features = get_git_features();
	if (features->can_reword)
		g_menu_append(history, TEXT_REWORD_MENU, ACTION_GROUP ".reword");
	if (features->can_drop)
		g_menu_append(history, TEXT_DROP_MENU, ACTION_GROUP ".drop");
	g_menu_append(history, TEXT_RESET_MENU, ACTION_GROUP ".reset");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(history));
	return G_MENU_MODEL(menu);
}


static GMenuModel *build_stash_menu(void) {
	GMenu *menu = g_menu_new();
	g_menu_append(menu, TEXT_STASH_POP, ACTION_GROUP ".stash-pop");
	g_menu_append(menu, TEXT_STASH_APPLY, ACTION_GROUP ".stash-apply");
	g_menu_append(menu, TEXT_STASH_DROP, ACTION_GROUP ".stash-drop");
	return G_MENU_MODEL(menu);
}


static void free_commit_menu(gpointer data) {
	CommitMenu *menu = data;
	free_commit(menu->commit);
	g_free(menu->stash_name);
	g_object_unref(menu->group);
	g_free(menu);
}


GtkWidget *create_commit_menu_button(Window *window) {
	CommitMenu *menu = g_new0(CommitMenu, 1);
	menu->window = window;
	menu->button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(menu->button), "view-more-symbolic");
	gtk_widget_set_tooltip_text(menu->button, TEXT_COMMIT_MENU);
	g_object_set_data_full(G_OBJECT(menu->button), MENU_KEY, menu, free_commit_menu);
	const GActionEntry ENTRIES[] = {
		{"copy-hash", copy_hash, NULL, NULL, NULL, {0}},
		{"new-branch", new_branch, NULL, NULL, NULL, {0}},
		{"new-tag", new_tag, NULL, NULL, NULL, {0}},
		{"switch", switch_to_commit, NULL, NULL, NULL, {0}},
		{"revert", revert_commit, NULL, NULL, NULL, {0}},
		{"cherry-pick", cherry_pick_commit, NULL, NULL, NULL, {0}},
		{"reset", reset_branch, NULL, NULL, NULL, {0}},
		{"reword", reword_commit, NULL, NULL, NULL, {0}},
		{"drop", drop_commit, NULL, NULL, NULL, {0}},
		{"stash-apply", apply_stash, NULL, NULL, NULL, {0}},
		{"stash-pop", pop_stash, NULL, NULL, NULL, {0}},
		{"stash-drop", drop_stash, NULL, NULL, NULL, {0}},
	};
	menu->group = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(menu->group), ENTRIES, G_N_ELEMENTS(ENTRIES), menu);
	gtk_widget_insert_action_group(get_window_widget(window), ACTION_GROUP, G_ACTION_GROUP(menu->group));
	return menu->button;
}


void set_commit_menu_commit(GtkWidget *button, const Commit *commit, const char *stash_name) {
	CommitMenu *menu = get_menu(button);
	free_commit(menu->commit);
	menu->commit = copy_commit(commit);
	g_set_str(&menu->stash_name, stash_name);
	g_autoptr(GMenuModel) model = stash_name ? build_stash_menu() : build_commit_menu();
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), model);
	gtk_widget_set_tooltip_text(button, stash_name ? TEXT_STASH_MENU : TEXT_COMMIT_MENU);
}
