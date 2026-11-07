#include <string.h>
#include <adwaita.h>

#include "view/icons.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "git/features.h"
#include "view/dialogs.h"
#include "view/format.h"
#include "view/branch-button.h"
#include "view/layout.h"

#define BUTTON_KEY "branch-button"
#define REFERENCE_KEY "reference"
#define CURRENT_KEY "is-current"
#define LIST_PAGE "list"
#define ACTIONS_PAGE "actions"
#define MORE_ICON "view-more-symbolic"
#define BACK_ICON "go-previous-symbolic"
#define POPOVER_WIDTH 340
#define LIST_HEIGHT 420
/* Branches following any upstream are cleaned up */
#define ANY_UPSTREAM "*"

typedef struct {
	Window *window;
	Repository *repository;
	gulong references_handler, changes_handler;
	GtkWidget *button, *name, *branch, *popover, *search, *local_list, *remote_list, *tag_list;
	GtkWidget *remote_expander, *tag_expander, *create_button, *clean_button;
	/* The list and one branch's own actions slide within the one popover, since a menu popped up from inside a
	 * popover leaves the popover unable to close on Wayland */
	GtkWidget *pages, *actions_title, *actions;
	struct BranchTarget *target;
} BranchButton;

/* A command run from the list, with what to say if it fails */
typedef struct {
	BranchButton *button;
	char *failure, *success;
} BranchCommand;


static void finish_branch_command(GitResult *result, gpointer data) {
	BranchCommand *command = data;
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(command->button->window, command->failure, result);
	else if (!result->is_cancelled && command->success)
		show_toast(command->button->window, command->success);
	release_window(command->button->window);
	g_object_unref(command->button->button);
	g_free(command->failure);
	g_free(command->success);
	g_free(command);
}


static void run_branch_command(BranchButton *button, const char *const *arguments, const char *failure,
	const char *success) {
	BranchCommand *command = g_new0(BranchCommand, 1);
	/* The button's own data lives as long as the widget, so the widget is kept too */
	command->button = button;
	g_object_ref(button->button);
	keep_window(button->window);
	command->failure = g_strdup(failure);
	command->success = g_strdup(success);
	run_repository_command(button->repository, arguments, NULL, finish_branch_command, command);
}


/* The button */

static char *format_sync_state(int ahead, int behind) {
	if (ahead && behind)
		return g_strdup_printf(TEXT_AHEAD_BEHIND, ahead, behind);
	if (ahead)
		return g_strdup_printf(TEXT_AHEAD, ahead);
	if (behind)
		return g_strdup_printf(TEXT_BEHIND, behind);
	return g_strdup("");
}


static void update_button(BranchButton *button) {
	gtk_label_set_text(GTK_LABEL(button->name), get_repository_name(button->repository));
	const WorkChanges *changes = get_work_changes(button->repository);
	const Status *status = changes ? changes->status : NULL;
	g_autofree char *branch = NULL;
	if (!status)
		branch = g_strdup("");
	else if (status->branch)
		branch = g_strdup(status->branch);
	else if (status->head_commit)
	{
		g_autofree char *short_hash = get_short_hash(status->head_commit);
		branch = g_strdup_printf(TEXT_DETACHED, short_hash);
	}
	else
		branch = g_strdup("");
	gtk_label_set_text(GTK_LABEL(button->branch), branch);
	g_autofree char *tooltip = g_strdup_printf(TEXT_BRANCH_BUTTON_TOOLTIP, get_repository_folder(button->repository));
	gtk_widget_set_tooltip_text(button->button, tooltip);
}


/* The list */

static GtkWidget *create_row_label(const char *text, const char *css_class, gboolean expands) {
	GtkWidget *label = gtk_label_new(text);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_MIDDLE);
	gtk_widget_set_hexpand(label, expands);
	if (css_class)
		gtk_widget_add_css_class(label, css_class);
	return label;
}


typedef struct BranchTarget {
	BranchButton *button;
	char *name;
} BranchTarget;


static void free_branch_target(gpointer data) {
	BranchTarget *target = data;
	g_free(target->name);
	g_free(target);
}


static void rename_to(const char *name, gpointer data) {
	BranchTarget *target = data;
	const char *arguments[] = {"branch", "-m", target->name, name, NULL};
	run_branch_command(target->button, arguments, TEXT_RENAME_FAILED, NULL);
}


static void rename_branch(GtkButton *widget, BranchButton *button) {
	BranchTarget *source = button->target;
	BranchTarget *target = g_new0(BranchTarget, 1);
	target->button = source->button;
	target->name = g_strdup(source->name);
	gtk_popover_popdown(GTK_POPOVER(source->button->popover));
	ask_for_text(get_window_widget(source->button->window), TEXT_RENAME_HEADING, NULL, source->name, TEXT_RENAME,
		rename_to, target, free_branch_target);
}


static void merge_branch(GtkButton *widget, BranchButton *button) {
	BranchTarget *target = button->target;
	gtk_popover_popdown(GTK_POPOVER(target->button->popover));
	const char *arguments[] = {"merge", "--no-edit", target->name, NULL};
	run_branch_command(target->button, arguments, TEXT_MERGE_FAILED, TEXT_MERGED);
}


static void delete_confirmed(gpointer data) {
	BranchTarget *target = data;
	const char *arguments[] = {"branch", "-D", target->name, NULL};
	run_branch_command(target->button, arguments, TEXT_DELETE_BRANCH_FAILED, TEXT_BRANCH_DELETED);
}


static void delete_branch(GtkButton *widget, BranchButton *button) {
	BranchTarget *source = button->target;
	BranchTarget *target = g_new0(BranchTarget, 1);
	target->button = source->button;
	target->name = g_strdup(source->name);
	gtk_popover_popdown(GTK_POPOVER(source->button->popover));
	g_autofree char *heading = g_strdup_printf(TEXT_DELETE_BRANCH_HEADING, source->name);
	ask_to_confirm(get_window_widget(source->button->window), heading, TEXT_DELETE_BRANCH_BODY, TEXT_DELETE, TRUE,
		delete_confirmed, target, free_branch_target);
}


static GtkWidget *create_action(const char *label, GCallback pressed, BranchButton *button, gboolean is_danger) {
	GtkWidget *action = gtk_button_new_with_label(label);
	gtk_widget_add_css_class(action, "flat");
	gtk_widget_add_css_class(action, "branch-action");
	if (is_danger)
		gtk_widget_add_css_class(action, "error");
	gtk_label_set_xalign(GTK_LABEL(gtk_button_get_child(GTK_BUTTON(action))), 0);
	g_signal_connect(action, "clicked", pressed, button);
	return action;
}


/* A branch's ⋮ slides the popover over to what can be done with that branch */
static void show_branch_actions(GtkButton *widget, BranchButton *button) {
	const char *name = g_object_get_data(G_OBJECT(widget), REFERENCE_KEY);
	gboolean is_current = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(widget), CURRENT_KEY));
	g_clear_pointer(&button->target, free_branch_target);
	button->target = g_new0(BranchTarget, 1);
	button->target->button = button;
	button->target->name = g_strdup(name);
	gtk_label_set_text(GTK_LABEL(button->actions_title), name);
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(button->actions)))
		gtk_box_remove(GTK_BOX(button->actions), child);
	gtk_box_append(GTK_BOX(button->actions), create_action(TEXT_RENAME, G_CALLBACK(rename_branch), button, FALSE));
	if (!is_current) {
		g_autofree char *merge = g_strdup_printf(TEXT_MERGE_INTO_CURRENT, name);
		gtk_box_append(GTK_BOX(button->actions), create_action(merge, G_CALLBACK(merge_branch), button, FALSE));
		gtk_box_append(GTK_BOX(button->actions), create_action(TEXT_DELETE_BRANCH_ACTION, G_CALLBACK(delete_branch),
			button, TRUE));
	}
	gtk_stack_set_visible_child_name(GTK_STACK(button->pages), ACTIONS_PAGE);
	gtk_widget_grab_focus(gtk_widget_get_first_child(button->actions));
}


static void show_branch_list(GtkButton *widget, BranchButton *button) {
	gtk_stack_set_visible_child_name(GTK_STACK(button->pages), LIST_PAGE);
	gtk_widget_grab_focus(button->search);
}


static GtkWidget *create_branch_menu(BranchButton *button, const Reference *reference) {
	GtkWidget *more = gtk_button_new_from_icon_name(MORE_ICON);
	gtk_widget_add_css_class(more, "flat");
	gtk_widget_add_css_class(more, "branch-more");
	gtk_widget_set_valign(more, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(more, TEXT_BRANCH_MENU);
	g_object_set_data_full(G_OBJECT(more), REFERENCE_KEY, g_strdup(reference->name), g_free);
	g_object_set_data(G_OBJECT(more), CURRENT_KEY, GINT_TO_POINTER(reference->is_current));
	g_signal_connect(more, "clicked", G_CALLBACK(show_branch_actions), button);
	return more;
}


static GtkWidget *create_actions_page(BranchButton *button) {
	GtkWidget *back = gtk_button_new_from_icon_name(BACK_ICON);
	gtk_widget_add_css_class(back, "flat");
	gtk_widget_set_tooltip_text(back, TEXT_BACK_TO_BRANCHES);
	g_signal_connect(back, "clicked", G_CALLBACK(show_branch_list), button);
	button->actions_title = gtk_label_new(NULL);
	gtk_widget_add_css_class(button->actions_title, "heading");
	gtk_label_set_ellipsize(GTK_LABEL(button->actions_title), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_xalign(GTK_LABEL(button->actions_title), 0);
	gtk_widget_set_hexpand(button->actions_title, TRUE);
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_box_append(GTK_BOX(header), back);
	gtk_box_append(GTK_BOX(header), button->actions_title);
	button->actions = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_HAIRLINE);
	GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SNUG);
	gtk_widget_set_valign(page, GTK_ALIGN_START);
	gtk_box_append(GTK_BOX(page), header);
	gtk_box_append(GTK_BOX(page), button->actions);
	return page;
}


static GtkWidget *create_reference_row(BranchButton *button, const Reference *reference) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(row, "branch-row");
	GtkWidget *mark = gtk_image_new_from_icon_name(reference->kind == REFERENCE_TAG ? ICON_TAG :
		reference->is_current ? "object-select-symbolic" : ICON_BRANCH);
	if (!reference->is_current)
		gtk_widget_add_css_class(mark, "dim-label");
	gtk_box_append(GTK_BOX(row), mark);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_set_hexpand(texts, TRUE);
	gtk_box_append(GTK_BOX(texts), create_row_label(reference->name, reference->is_current ? "heading" : NULL, TRUE));
	g_autofree char *ago = format_time_ago(reference->time);
	g_autofree char *sync = format_sync_state(reference->ahead, reference->behind);
	g_autofree char *details = *sync ? g_strdup_printf(TEXT_BRANCH_DETAILS, ago, sync) : g_strdup(ago);
	gtk_box_append(GTK_BOX(texts), create_row_label(details, "row-details", TRUE));
	gtk_widget_set_tooltip_text(texts, reference->subject);
	gtk_box_append(GTK_BOX(row), texts);
	if (reference->kind == REFERENCE_BRANCH)
		gtk_box_append(GTK_BOX(row), create_branch_menu(button, reference));
	g_object_set_data_full(G_OBJECT(row), REFERENCE_KEY, g_strdup(reference->name), g_free);
	g_object_set_data_full(G_OBJECT(row), "commit", g_strdup(reference->commit), g_free);
	return row;
}


static void clear_list(GtkWidget *list) {
	gtk_list_box_remove_all(GTK_LIST_BOX(list));
}


static void fill_lists(BranchButton *button) {
	clear_list(button->local_list);
	clear_list(button->remote_list);
	clear_list(button->tag_list);
	GPtrArray *references = get_repository_references(button->repository);
	guint remote_count = 0, tag_count = 0;
	for (guint index = 0; index < references->len; index++) {
		Reference *reference = g_ptr_array_index(references, index);
		GtkWidget *list = reference->kind == REFERENCE_BRANCH ? button->local_list :
			reference->kind == REFERENCE_REMOTE ? button->remote_list : button->tag_list;
		remote_count += reference->kind == REFERENCE_REMOTE;
		tag_count += reference->kind == REFERENCE_TAG;
		gtk_list_box_append(GTK_LIST_BOX(list), create_reference_row(button, reference));
	}
	gtk_widget_set_visible(button->remote_expander, remote_count > 0);
	gtk_widget_set_visible(button->tag_expander, tag_count > 0);
	gtk_widget_set_visible(button->clean_button, get_git_features()->can_delete_merged);
}


static void switch_to(BranchButton *button, const char *name) {
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	g_autofree char *success = g_strdup_printf(TEXT_SWITCHED_TO, name);
	const char *arguments[] = {"switch", name, NULL};
	run_branch_command(button, arguments, TEXT_SWITCH_FAILED, success);
}


static void choose_local(GtkListBox *list, GtkListBoxRow *row, BranchButton *button) {
	switch_to(button, g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), REFERENCE_KEY));
}


/* A remote branch is switched to through the local branch that follows it, or a new one made to follow it, so the
 * remote it came from is never lost */
static void choose_remote(GtkListBox *list, GtkListBoxRow *row, BranchButton *button) {
	const char *name = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), REFERENCE_KEY);
	GPtrArray *references = get_repository_references(button->repository);
	for (guint index = 0; index < references->len; index++) {
		Reference *reference = g_ptr_array_index(references, index);
		if (reference->kind == REFERENCE_BRANCH && g_strcmp0(reference->upstream, name) == 0) {
			switch_to(button, reference->name);
			return;
		}
	}
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	g_autofree char *success = g_strdup_printf(TEXT_SWITCHED_TO, name);
	const char *arguments[] = {"switch", "--track", name, NULL};
	run_branch_command(button, arguments, TEXT_SWITCH_FAILED, success);
}


static void choose_tag(GtkListBox *list, GtkListBoxRow *row, BranchButton *button) {
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	go_to_commit(button->window, g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), "commit"));
}


static gboolean filter_row(GtkListBoxRow *row, gpointer data) {
	BranchButton *button = data;
	const char *search = gtk_editable_get_text(GTK_EDITABLE(button->search));
	if (!*search)
		return TRUE;
	const char *name = g_object_get_data(G_OBJECT(gtk_list_box_row_get_child(row)), REFERENCE_KEY);
	g_autofree char *lower_name = g_utf8_casefold(name, -1);
	g_autofree char *lower_search = g_utf8_casefold(search, -1);
	return strstr(lower_name, lower_search) != NULL;
}


static void change_search(BranchButton *button) {
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(button->local_list));
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(button->remote_list));
	gtk_list_box_invalidate_filter(GTK_LIST_BOX(button->tag_list));
	const char *search = gtk_editable_get_text(GTK_EDITABLE(button->search));
	g_autofree char *label = *search ? g_strdup_printf(TEXT_CREATE_NAMED_BRANCH, search) :
		g_strdup(TEXT_NEW_BRANCH);
	gtk_button_set_label(GTK_BUTTON(button->create_button), label);
	if (*search) {
		gtk_expander_set_expanded(GTK_EXPANDER(button->remote_expander), TRUE);
		gtk_expander_set_expanded(GTK_EXPANDER(button->tag_expander), TRUE);
	}
}


static void create_named(const char *name, gpointer data) {
	BranchButton *button = data;
	const char *arguments[] = {"switch", "-c", name, NULL};
	g_autofree char *success = g_strdup_printf(TEXT_SWITCHED_TO, name);
	run_branch_command(button, arguments, TEXT_BRANCH_FAILED, success);
}


static void press_create(GtkButton *widget, BranchButton *button) {
	g_autofree char *search = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(button->search))));
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	if (*search)
		create_named(search, button);
	else
		ask_for_text(get_window_widget(button->window), TEXT_NEW_BRANCH_HEADING, TEXT_NEW_BRANCH_BODY, NULL,
			TEXT_CREATE_BRANCH, create_named, button, NULL);
}


static void clean_confirmed(gpointer data) {
	BranchButton *button = data;
	const char *arguments[] = {"branch", "--delete-merged", ANY_UPSTREAM, NULL};
	run_branch_command(button, arguments, TEXT_CLEAN_FAILED, TEXT_CLEANED);
}


static void run_clean(GtkButton *widget, BranchButton *button) {
	gtk_popover_popdown(GTK_POPOVER(button->popover));
	ask_to_confirm(get_window_widget(button->window), TEXT_CLEAN_HEADING, TEXT_CLEAN_BODY, TEXT_CLEAN, TRUE,
		clean_confirmed, button, NULL);
}


static GtkWidget *create_list(BranchButton *button, GCallback activated) {
	GtkWidget *list = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
	gtk_list_box_set_activate_on_single_click(GTK_LIST_BOX(list), TRUE);
	gtk_list_box_set_filter_func(GTK_LIST_BOX(list), filter_row, button, NULL);
	gtk_widget_add_css_class(list, "navigation-sidebar");
	g_signal_connect(list, "row-activated", activated, button);
	return list;
}


static GtkWidget *create_expander(const char *title, GtkWidget *list) {
	GtkWidget *expander = gtk_expander_new(title);
	gtk_expander_set_child(GTK_EXPANDER(expander), list);
	gtk_widget_add_css_class(expander, "branch-expander");
	return expander;
}


/* Typing in the list's search field and pressing Enter makes a branch of that name when none matches */
static void activate_search(GtkSearchEntry *entry, BranchButton *button) {
	press_create(NULL, button);
}


static void open_popover(GtkPopover *popover, BranchButton *button) {
	gtk_stack_set_visible_child_full(GTK_STACK(button->pages), LIST_PAGE, GTK_STACK_TRANSITION_TYPE_NONE);
	gtk_editable_set_text(GTK_EDITABLE(button->search), "");
	fill_lists(button);
	gtk_widget_grab_focus(button->search);
}


static GtkWidget *create_popover(BranchButton *button) {
	button->search = gtk_search_entry_new();
	gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(button->search), TEXT_BRANCH_SEARCH);
	g_signal_connect_swapped(button->search, "search-changed", G_CALLBACK(change_search), button);
	g_signal_connect(button->search, "activate", G_CALLBACK(activate_search), button);
	button->local_list = create_list(button, G_CALLBACK(choose_local));
	button->remote_list = create_list(button, G_CALLBACK(choose_remote));
	button->tag_list = create_list(button, G_CALLBACK(choose_tag));
	button->remote_expander = create_expander(TEXT_REMOTE_BRANCHES, button->remote_list);
	button->tag_expander = create_expander(TEXT_TAGS, button->tag_list);
	GtkWidget *lists = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SMALL);
	gtk_box_append(GTK_BOX(lists), button->local_list);
	gtk_box_append(GTK_BOX(lists), button->remote_expander);
	gtk_box_append(GTK_BOX(lists), button->tag_expander);
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
	gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroller), TRUE);
	gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(scroller), LIST_HEIGHT);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), lists);
	button->create_button = gtk_button_new_with_label(TEXT_NEW_BRANCH);
	gtk_widget_set_tooltip_text(button->create_button, TEXT_NEW_BRANCH_TOOLTIP);
	gtk_widget_add_css_class(button->create_button, "suggested-action");
	gtk_widget_set_hexpand(button->create_button, TRUE);
	g_signal_connect(button->create_button, "clicked", G_CALLBACK(press_create), button);
	/* Cleaning up asks before it deletes anything, so a plain button is enough */
	button->clean_button = gtk_button_new_with_label(TEXT_CLEAN);
	gtk_widget_set_tooltip_text(button->clean_button, TEXT_CLEAN_TOOLTIP);
	g_signal_connect(button->clean_button, "clicked", G_CALLBACK(run_clean), button);
	GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_box_append(GTK_BOX(footer), button->create_button);
	gtk_box_append(GTK_BOX(footer), button->clean_button);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SNUG);
	gtk_widget_add_css_class(box, "branch-popover");
	gtk_widget_set_size_request(box, POPOVER_WIDTH, -1);
	gtk_box_append(GTK_BOX(box), button->search);
	gtk_box_append(GTK_BOX(box), scroller);
	gtk_box_append(GTK_BOX(box), footer);
	button->pages = gtk_stack_new();
	gtk_stack_set_transition_type(GTK_STACK(button->pages), GTK_STACK_TRANSITION_TYPE_SLIDE_LEFT_RIGHT);
	gtk_stack_set_transition_duration(GTK_STACK(button->pages), SLIDE_MILLISECONDS);
	gtk_stack_set_vhomogeneous(GTK_STACK(button->pages), FALSE);
	gtk_stack_set_interpolate_size(GTK_STACK(button->pages), TRUE);
	gtk_stack_add_named(GTK_STACK(button->pages), box, LIST_PAGE);
	gtk_stack_add_named(GTK_STACK(button->pages), create_actions_page(button), ACTIONS_PAGE);
	button->popover = gtk_popover_new();
	gtk_popover_set_child(GTK_POPOVER(button->popover), button->pages);
	g_signal_connect(button->popover, "show", G_CALLBACK(open_popover), button);
	return button->popover;
}


static void free_branch_button(gpointer data) {
	BranchButton *button = data;
	if (button->repository) {
		g_clear_signal_handler(&button->references_handler, button->repository);
		g_clear_signal_handler(&button->changes_handler, button->repository);
		g_clear_object(&button->repository);
	}
	g_clear_pointer(&button->target, free_branch_target);
	g_free(button);
}


GtkWidget *create_branch_button(Window *window) {
	BranchButton *button = g_new0(BranchButton, 1);
	button->window = window;
	button->name = gtk_label_new(NULL);
	gtk_widget_add_css_class(button->name, "repository-name");
	gtk_label_set_ellipsize(GTK_LABEL(button->name), PANGO_ELLIPSIZE_END);
	button->branch = gtk_label_new(NULL);
	gtk_label_set_ellipsize(GTK_LABEL(button->branch), PANGO_ELLIPSIZE_MIDDLE);
	GtkWidget *line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_TIGHT);
	gtk_widget_add_css_class(line, "branch-line");
	gtk_widget_set_halign(line, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(line), gtk_image_new_from_icon_name(ICON_BRANCH));
	gtk_box_append(GTK_BOX(line), button->branch);
	/* A small arrow says the name opens the list of branches */
	GtkWidget *arrow = gtk_image_new_from_icon_name("pan-down-symbolic");
	gtk_widget_add_css_class(arrow, "branch-arrow");
	gtk_box_append(GTK_BOX(line), arrow);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_box_append(GTK_BOX(texts), button->name);
	gtk_box_append(GTK_BOX(texts), line);
	button->button = gtk_menu_button_new();
	gtk_menu_button_set_child(GTK_MENU_BUTTON(button->button), texts);
	gtk_menu_button_set_always_show_arrow(GTK_MENU_BUTTON(button->button), FALSE);
	gtk_widget_add_css_class(button->button, "flat");
	gtk_widget_add_css_class(button->button, "branch-button");
	gtk_menu_button_set_popover(GTK_MENU_BUTTON(button->button), create_popover(button));
	g_object_set_data_full(G_OBJECT(button->button), BUTTON_KEY, button, free_branch_button);
	return button->button;
}


static void follow_changes(Repository *repository, BranchButton *button) {
	update_button(button);
}


/* An open list is only filled again when the branches changed, not on every change to a file */
static void follow_references(Repository *repository, BranchButton *button) {
	update_button(button);
	if (gtk_widget_get_visible(button->popover))
		fill_lists(button);
}


void set_branch_button_repository(GtkWidget *widget, Repository *repository) {
	BranchButton *button = g_object_get_data(G_OBJECT(widget), BUTTON_KEY);
	if (button->repository) {
		g_clear_signal_handler(&button->references_handler, button->repository);
		g_clear_signal_handler(&button->changes_handler, button->repository);
		g_clear_object(&button->repository);
	}
	if (!repository)
		return;
	button->repository = g_object_ref(repository);
	button->references_handler = g_signal_connect(repository, "references-loaded", G_CALLBACK(follow_references),
		button);
	button->changes_handler = g_signal_connect(repository, "changes-loaded", G_CALLBACK(follow_changes), button);
	update_button(button);
}


void open_branch_list(GtkWidget *widget) {
	gtk_menu_button_popup(GTK_MENU_BUTTON(widget));
}
