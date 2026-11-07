#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "view/commit-box.h"

#define MESSAGE_LINES_HEIGHT 76
#define NEW_MODE "new"
#define UPDATE_MODE "update"
/* What the branch's history of moves says about Undo */
#define UNDO_REASON "undo commit"

struct CommitBox {
	Window *window;
	GtkWidget *box, *message, *placeholder, *button, *modes;
	guint staged_count, changed_count;
	gboolean is_amending;
	/* Each way keeps its own text, so switching back and forth never loses what was written */
	char *new_text, *update_text;
};

/* What Undo needs to take a commit back while keeping its changes */
typedef struct {
	CommitBox *box;
	/* The repository the commit went into, which stays the one Undo works on even if the window moves on */
	Repository *repository;
	/* The commit checked out before, or NULL when this was the first commit */
	char *previous;
	/* The commit made, which Undo only takes back while it is still the one checked out */
	char *made;
	char *message;
} CommitUndo;


static char *get_message(CommitBox *box) {
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(box->message));
	GtkTextIter start, end;
	gtk_text_buffer_get_bounds(buffer, &start, &end);
	g_autofree char *text = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
	return g_strstrip(g_steal_pointer(&text));
}


static void set_message(CommitBox *box, const char *text) {
	gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(box->message)), text, -1);
}


static void update_button(CommitBox *box) {
	g_autofree char *message = get_message(box);
	gboolean has_changes = box->staged_count + box->changed_count > 0 || box->is_amending;
	gtk_widget_set_sensitive(box->button, *message && has_changes);
	gtk_button_set_label(GTK_BUTTON(box->button), box->is_amending ? TEXT_UPDATE_COMMIT :
		box->staged_count > 0 ? TEXT_COMMIT : TEXT_COMMIT_ALL);
	gtk_widget_set_tooltip_text(box->button, box->is_amending ? TEXT_UPDATE_COMMIT_TOOLTIP :
		box->staged_count > 0 ? TEXT_COMMIT_TOOLTIP : TEXT_COMMIT_ALL_TOOLTIP);
	gtk_widget_set_visible(box->placeholder, !*message);
}


static void start_new_commit(CommitBox *box);


static void free_undo(gpointer data) {
	CommitUndo *undo = data;
	release_window(undo->box->window);
	g_clear_object(&undo->repository);
	g_free(undo->made);
	g_free(undo->previous);
	g_free(undo->message);
	g_free(undo);
}


static void finish_undo(GitResult *result, gpointer data) {
	CommitUndo *undo = data;
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(undo->box->window, TEXT_UNDO_COMMIT_FAILED, result);
	free_undo(undo);
}


/* The commit goes away and its changes stay staged, with the message back in the field to try again. Git only
 * moves the branch while it still points at the commit made, so a commit or pull since then is never undone */
static void undo_commit(gpointer data) {
	CommitUndo *source = data;
	CommitUndo *undo = g_new0(CommitUndo, 1);
	undo->box = source->box;
	keep_window(undo->box->window);
	if (get_window_repository(undo->box->window) == source->repository)
		set_message(undo->box, source->message);
	const char *move[] = {"update-ref", "-m", UNDO_REASON, "HEAD", source->previous ? source->previous : "",
		source->made, NULL};
	const char *remove[] = {"update-ref", "-m", UNDO_REASON, "-d", "HEAD", source->made, NULL};
	run_repository_command(source->repository, source->previous ? move : remove, NULL, finish_undo, undo);
}


static void read_made_commit(GitResult *result, gpointer data) {
	CommitUndo *undo = data;
	if (result->is_cancelled || !has_git_succeeded(result)) {
		free_undo(undo);
		return;
	}
	undo->made = get_git_output_text(result);
	show_undo_toast(undo->box->window, TEXT_COMMITTED, undo_commit, undo, free_undo);
}


static void finish_commit(GitResult *result, gpointer data) {
	CommitUndo *undo = data;
	if (result->is_cancelled) {
		free_undo(undo);
		return;
	}
	if (!has_git_succeeded(result)) {
		show_git_failure(undo->box->window, TEXT_COMMIT_FAILED, result);
		set_message(undo->box, undo->message);
		free_undo(undo);
		return;
	}
	start_new_commit(undo->box);
	const char *arguments[] = {"rev-parse", "HEAD", NULL};
	read_repository(undo->repository, arguments, read_made_commit, undo);
}


static void run_commit(CommitUndo *undo) {
	Repository *repository = undo->repository;
	const char *plain[] = {"commit", "--file=-", "--cleanup=whitespace", NULL};
	const char *amend[] = {"commit", "--file=-", "--cleanup=whitespace", "--amend", NULL};
	g_autoptr(GBytes) input = g_bytes_new(undo->message, strlen(undo->message));
	GitOptions options = {.input = input};
	run_repository_command(repository, undo->box->is_amending ? amend : plain, &options, finish_commit, undo);
}


/* Commit All stages everything first, new files too, and commits only once that is done */
static void commit_after_staging(GitResult *result, gpointer data) {
	CommitUndo *undo = data;
	if (result->is_cancelled || !has_git_succeeded(result)) {
		finish_commit(result, undo);
		return;
	}
	run_commit(undo);
}


static void commit(CommitBox *box) {
	if (!gtk_widget_get_sensitive(box->button))
		return;
	g_autofree char *message = get_message(box);
	Repository *repository = get_window_repository(box->window);
	const WorkChanges *changes = get_work_changes(repository);
	CommitUndo *undo = g_new0(CommitUndo, 1);
	undo->box = box;
	keep_window(box->window);
	undo->repository = g_object_ref(repository);
	undo->previous = g_strdup(changes ? changes->status->head_commit : NULL);
	undo->message = g_strdup(message);
	set_message(box, "");
	if (!box->is_amending && box->staged_count == 0) {
		const char *stage[] = {"add", "-A", NULL};
		run_repository_command(repository, stage, NULL, commit_after_staging, undo);
	} else
		run_commit(undo);
}


static gboolean press_key(GtkEventControllerKey *controller, guint key, guint code, GdkModifierType state,
	CommitBox *box) {
	if ((key == GDK_KEY_Return || key == GDK_KEY_KP_Enter) && (state & GDK_CONTROL_MASK)) {
		commit(box);
		return TRUE;
	}
	return FALSE;
}


static void read_last_message(GitResult *result, gpointer data) {
	CommitBox *box = data;
	/* The answer can come after the user went back to a new commit */
	if (!has_git_succeeded(result) || !box->is_amending)
		return;
	g_autofree char *message = get_git_output_text(result);
	g_autofree char *current = get_message(box);
	if (!*current)
		set_message(box, g_strchomp(message));
}


/* Switching keeps the text written for the way left, and brings back the text of the way chosen. Updating the latest
 * commit starts from its message, which is what people usually want to change */
static void choose_mode(AdwToggleGroup *modes, GParamSpec *spec, CommitBox *box) {
	gboolean is_amending = g_strcmp0(adw_toggle_group_get_active_name(modes), UPDATE_MODE) == 0;
	if (is_amending == box->is_amending)
		return;
	char **left = box->is_amending ? &box->update_text : &box->new_text;
	g_free(*left);
	*left = get_message(box);
	box->is_amending = is_amending;
	const char *kept = is_amending ? box->update_text : box->new_text;
	set_message(box, kept ? kept : "");
	gtk_label_set_text(GTK_LABEL(box->placeholder), is_amending ? TEXT_UPDATE_PLACEHOLDER : TEXT_COMMIT_PLACEHOLDER);
	if (is_amending && !kept) {
		const char *arguments[] = {"log", "-1", "--format=%B", NULL};
		read_repository(get_window_repository(box->window), arguments, read_last_message, box);
	}
	update_button(box);
}


/* After a commit, and in another repository, the box starts over as an empty new commit */
static void start_new_commit(CommitBox *box) {
	g_clear_pointer(&box->new_text, g_free);
	g_clear_pointer(&box->update_text, g_free);
	adw_toggle_group_set_active_name(ADW_TOGGLE_GROUP(box->modes), NEW_MODE);
	set_message(box, "");
}


static GtkWidget *create_modes(CommitBox *box) {
	const struct {
		const char *name, *label, *tooltip;
	} MODES[] = {
		{NEW_MODE, TEXT_NEW_COMMIT, TEXT_NEW_COMMIT_TOOLTIP},
		{UPDATE_MODE, TEXT_UPDATE_LATEST, TEXT_UPDATE_LATEST_TOOLTIP},
	};
	box->modes = adw_toggle_group_new();
	for (guint index = 0; index < G_N_ELEMENTS(MODES); index++) {
		AdwToggle *toggle = adw_toggle_new();
		adw_toggle_set_name(toggle, MODES[index].name);
		adw_toggle_set_label(toggle, MODES[index].label);
		adw_toggle_set_tooltip(toggle, MODES[index].tooltip);
		adw_toggle_group_add(ADW_TOGGLE_GROUP(box->modes), toggle);
	}
	adw_toggle_group_set_active_name(ADW_TOGGLE_GROUP(box->modes), NEW_MODE);
	gtk_widget_set_halign(box->modes, GTK_ALIGN_START);
	g_signal_connect(box->modes, "notify::active-name", G_CALLBACK(choose_mode), box);
	return box->modes;
}


static GtkWidget *create_message_field(CommitBox *box) {
	box->message = gtk_text_view_new();
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(box->message), GTK_WRAP_WORD_CHAR);
	gtk_text_view_set_accepts_tab(GTK_TEXT_VIEW(box->message), FALSE);
	gtk_widget_add_css_class(box->message, "commit-message");
	g_signal_connect_swapped(gtk_text_view_get_buffer(GTK_TEXT_VIEW(box->message)), "changed",
		G_CALLBACK(update_button), box);
	GtkEventController *keys = gtk_event_controller_key_new();
	g_signal_connect(keys, "key-pressed", G_CALLBACK(press_key), box);
	gtk_widget_add_controller(box->message, keys);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), box->message);
	gtk_widget_set_size_request(scroller, -1, MESSAGE_LINES_HEIGHT);
	box->placeholder = gtk_label_new(TEXT_COMMIT_PLACEHOLDER);
	gtk_widget_add_css_class(box->placeholder, "commit-placeholder");
	gtk_widget_set_halign(box->placeholder, GTK_ALIGN_START);
	gtk_widget_set_valign(box->placeholder, GTK_ALIGN_START);
	gtk_widget_set_can_target(box->placeholder, FALSE);
	GtkWidget *overlay = gtk_overlay_new();
	gtk_overlay_set_child(GTK_OVERLAY(overlay), scroller);
	gtk_overlay_add_overlay(GTK_OVERLAY(overlay), box->placeholder);
	gtk_widget_add_css_class(overlay, "commit-field");
	return overlay;
}


CommitBox *create_commit_box(Window *window) {
	CommitBox *box = g_new0(CommitBox, 1);
	box->window = window;
	/* The box reads in the order it is used: what to make, the message, then the button that makes it */
	box->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box->box, "commit-box");
	gtk_box_append(GTK_BOX(box->box), create_modes(box));
	gtk_box_append(GTK_BOX(box->box), create_message_field(box));
	box->button = gtk_button_new_with_label(TEXT_COMMIT);
	gtk_widget_add_css_class(box->button, "suggested-action");
	gtk_widget_set_halign(box->button, GTK_ALIGN_END);
	g_signal_connect_swapped(box->button, "clicked", G_CALLBACK(commit), box);
	gtk_box_append(GTK_BOX(box->box), box->button);
	update_button(box);
	return box;
}


GtkWidget *get_commit_box_widget(CommitBox *box) {
	return box->box;
}


void update_commit_box(CommitBox *box, guint staged_count, guint changed_count) {
	box->staged_count = staged_count;
	box->changed_count = changed_count;
	/* A repository without commits has no latest commit to update */
	gtk_widget_set_visible(box->modes, has_repository_commits(get_window_repository(box->window)));
	update_button(box);
}


/* Another repository starts with an empty field and a new commit */
void reset_commit_box(CommitBox *box) {
	start_new_commit(box);
}


void free_commit_box(CommitBox *box) {
	g_free(box->new_text);
	g_free(box->update_text);
	g_free(box);
}
