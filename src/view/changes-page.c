#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "view/layout.h"
#include "view/page-scroller.h"
#include "view/card-list.h"
#include "view/commit-box.h"
#include "view/file-actions.h"
#include "view/file-card.h"
#include "view/format.h"
#include "view/changes-page.h"

#define ILLUSTRATION_CLEAN "crimson-all-committed"

/* One group of files: its heading, count, buttons and list of files near the top, and its cards further down under a
 * small heading of its own, so every changed file shows at a glance before the first diff */
typedef struct {
	GtkWidget *box, *count, *revealer, *cards_revealer;
	CardList *cards;
} Section;

/* Every file of a group shows in its list, even a single one */
#define FEWEST_OVERVIEW_FILES 1

struct ChangesPage {
	Window *window;
	Repository *repository;
	gulong changes_handler;
	GtkWidget *page, *content, *empty, *operation_revealer, *operation_title, *operation_text, *continue_button;
	GtkWidget *conflict_box, *conflict_revealer, *commit_revealer;
	Section staged, unstaged;
	CommitBox *commit_box;
	gboolean has_shown;
};


static GtkWidget *create_section_button(const char *label, const char *tooltip, GCallback pressed, Window *window) {
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "stage-button");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_signal_connect_swapped(button, "clicked", pressed, window);
	return button;
}


static GtkWidget *create_section_heading(Section *section, const char *title, const char *tooltip) {
	GtkWidget *heading = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(heading, "section-heading");
	GtkWidget *label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "section-title");
	gtk_widget_set_tooltip_text(label, tooltip);
	gtk_box_append(GTK_BOX(heading), label);
	section->count = gtk_label_new(NULL);
	gtk_widget_add_css_class(section->count, "section-count");
	gtk_widget_set_hexpand(section->count, TRUE);
	gtk_label_set_xalign(GTK_LABEL(section->count), 0);
	gtk_box_append(GTK_BOX(heading), section->count);
	return heading;
}


static GtkWidget *create_part_revealer(GtkWidget *child) {
	GtkWidget *revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), SLIDE_MILLISECONDS);
	gtk_revealer_set_child(GTK_REVEALER(revealer), child);
	return revealer;
}


static void build_section(Section *section, GtkWidget *heading, const char *title) {
	section->cards = create_card_list(FEWEST_OVERVIEW_FILES);
	section->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(section->box, "page-part");
	gtk_box_append(GTK_BOX(section->box), heading);
	gtk_box_append(GTK_BOX(section->box), get_card_list_overview(section->cards));
	section->revealer = create_part_revealer(section->box);
	GtkWidget *group = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(group, "page-part");
	/* The group's label is a divider with a line, not a second heading */
	GtkWidget *divider = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_add_css_class(divider, "group-divider");
	GtkWidget *label = gtk_label_new(title);
	gtk_widget_add_css_class(label, "group-heading");
	gtk_box_append(GTK_BOX(divider), label);
	GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
	gtk_widget_set_hexpand(line, TRUE);
	gtk_widget_set_valign(line, GTK_ALIGN_CENTER);
	gtk_box_append(GTK_BOX(divider), line);
	gtk_box_append(GTK_BOX(group), divider);
	gtk_box_append(GTK_BOX(group), get_card_list_widget(section->cards));
	section->cards_revealer = create_part_revealer(group);
}


static void create_staged_section(ChangesPage *page) {
	GtkWidget *heading = create_section_heading(&page->staged, TEXT_STAGED, TEXT_STAGED_TOOLTIP);
	gtk_box_append(GTK_BOX(heading), create_section_button(TEXT_UNSTAGE_ALL, TEXT_UNSTAGE_ALL_TOOLTIP,
		G_CALLBACK(unstage_all), page->window));
	build_section(&page->staged, heading, TEXT_STAGED);
}


static void create_unstaged_section(ChangesPage *page) {
	GtkWidget *heading = create_section_heading(&page->unstaged, TEXT_UNSTAGED, TEXT_UNSTAGED_TOOLTIP);
	gtk_box_append(GTK_BOX(heading), create_section_button(TEXT_STASH_PUSH, TEXT_STASH_PUSH_TOOLTIP,
		G_CALLBACK(stash_all), page->window));
	/* Discarding loses work, so its button wears the colour of danger */
	GtkWidget *discard = create_section_button(TEXT_DISCARD_ALL, TEXT_DISCARD_ALL_TOOLTIP, G_CALLBACK(discard_all),
		page->window);
	gtk_widget_add_css_class(discard, "destructive-action");
	gtk_box_append(GTK_BOX(heading), discard);
	gtk_box_append(GTK_BOX(heading), create_section_button(TEXT_STAGE_ALL, TEXT_STAGE_ALL_TOOLTIP,
		G_CALLBACK(stage_all), page->window));
	build_section(&page->unstaged, heading, TEXT_UNSTAGED);
}


/* Work git stopped in the middle of */

static const char *const OPERATION_COMMANDS[] = {NULL, "merge", "rebase", "cherry-pick", "revert", "bisect", "am"};


static void finish_operation_command(GitResult *result, gpointer data) {
	ChangesPage *page = data;
	if (!result->is_cancelled && !has_git_succeeded(result))
		show_git_failure(page->window, TEXT_OPERATION_FAILED, result);
	release_window(page->window);
}


static void run_operation(ChangesPage *page, const char *step) {
	Operation operation = get_repository_operation(page->repository);
	if (operation == OPERATION_NONE)
		return;
	/* A bisect ends with reset rather than abort, and has no step to continue */
	const char *word = operation == OPERATION_BISECT ? "reset" : step;
	g_autofree char *option = operation == OPERATION_BISECT ? g_strdup(word) : g_strdup_printf("--%s", word);
	const char *arguments[] = {OPERATION_COMMANDS[operation], option, NULL};
	keep_window(page->window);
	run_repository_command(page->repository, arguments, NULL, finish_operation_command, page);
}


static void press_continue(GtkButton *button, ChangesPage *page) {
	run_operation(page, "continue");
}


static void press_abort(GtkButton *button, ChangesPage *page) {
	run_operation(page, "abort");
}


static GtkWidget *create_operation_banner(ChangesPage *page) {
	GtkWidget *banner = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_add_css_class(banner, "operation-banner");
	gtk_widget_add_css_class(banner, "page-part");
	GtkWidget *icon = gtk_image_new_from_icon_name("dialog-warning-symbolic");
	gtk_image_set_icon_size(GTK_IMAGE(icon), GTK_ICON_SIZE_LARGE);
	gtk_box_append(GTK_BOX(banner), icon);
	GtkWidget *texts = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_HAIRLINE);
	gtk_widget_set_hexpand(texts, TRUE);
	page->operation_title = gtk_label_new(NULL);
	gtk_widget_add_css_class(page->operation_title, "heading");
	gtk_label_set_xalign(GTK_LABEL(page->operation_title), 0);
	page->operation_text = gtk_label_new(NULL);
	gtk_label_set_wrap(GTK_LABEL(page->operation_text), TRUE);
	gtk_label_set_xalign(GTK_LABEL(page->operation_text), 0);
	gtk_box_append(GTK_BOX(texts), page->operation_title);
	gtk_box_append(GTK_BOX(texts), page->operation_text);
	gtk_box_append(GTK_BOX(banner), texts);
	GtkWidget *abort = gtk_button_new_with_label(TEXT_ABORT);
	gtk_widget_set_tooltip_text(abort, TEXT_ABORT_TOOLTIP);
	gtk_widget_set_valign(abort, GTK_ALIGN_CENTER);
	g_signal_connect(abort, "clicked", G_CALLBACK(press_abort), page);
	gtk_box_append(GTK_BOX(banner), abort);
	page->continue_button = gtk_button_new_with_label(TEXT_CONTINUE);
	gtk_widget_set_tooltip_text(page->continue_button, TEXT_CONTINUE_TOOLTIP);
	gtk_widget_set_valign(page->continue_button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(page->continue_button, "suggested-action");
	g_signal_connect(page->continue_button, "clicked", G_CALLBACK(press_continue), page);
	gtk_box_append(GTK_BOX(banner), page->continue_button);
	page->operation_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(page->operation_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_child(GTK_REVEALER(page->operation_revealer), banner);
	return page->operation_revealer;
}


/* Parts slide in and out as the folder changes, while the first look at a repository appears at once */
static void reveal_part(ChangesPage *page, GtkWidget *revealer, gboolean is_revealed) {
	gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), page->has_shown ? SLIDE_MILLISECONDS : 0);
	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), is_revealed);
}


static void show_operation(ChangesPage *page, const WorkChanges *changes) {
	Operation operation = get_repository_operation(page->repository);
	reveal_part(page, page->operation_revealer, operation != OPERATION_NONE);
	if (operation == OPERATION_NONE)
		return;
	gboolean has_conflicts = count_conflicted_files(changes->status) > 0;
	gtk_label_set_text(GTK_LABEL(page->operation_title), get_operation_title(operation));
	gtk_label_set_text(GTK_LABEL(page->operation_text), operation == OPERATION_BISECT ? TEXT_BISECT_TEXT :
		has_conflicts ? TEXT_OPERATION_CONFLICTS : TEXT_OPERATION_READY);
	gtk_widget_set_visible(page->continue_button, operation != OPERATION_BISECT);
	gtk_widget_set_sensitive(page->continue_button, !has_conflicts);
}


static void show_conflicts(ChangesPage *page, const WorkChanges *changes) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(page->conflict_box)))
		gtk_box_remove(GTK_BOX(page->conflict_box), child);
	guint count = 0;
	for (guint index = 0; index < changes->status->files->len; index++) {
		StatusFile *file = g_ptr_array_index(changes->status->files, index);
		if (!file->is_conflicted)
			continue;
		gtk_box_append(GTK_BOX(page->conflict_box), create_conflict_card(page->window, file->path));
		count++;
	}
	reveal_part(page, page->conflict_revealer, count > 0);
}


/* Conflicted files show only as conflicts, not again as changes */
static GPtrArray *filter_conflicts(GPtrArray *files, const Status *status) {
	g_autoptr(GHashTable) conflicted = g_hash_table_new(g_str_hash, g_str_equal);
	for (guint index = 0; index < status->files->len; index++) {
		StatusFile *status_file = g_ptr_array_index(status->files, index);
		if (status_file->is_conflicted)
			g_hash_table_add(conflicted, status_file->path);
	}
	GPtrArray *kept = g_ptr_array_new_full(files->len, NULL);
	for (guint index = 0; index < files->len; index++) {
		DiffFile *file = g_ptr_array_index(files, index);
		if (!g_hash_table_contains(conflicted, file->path))
			g_ptr_array_add(kept, file);
	}
	return kept;
}


static void show_section(ChangesPage *page, Section *section, GPtrArray *files, DiffSide side) {
	g_autoptr(GPtrArray) shown = filter_conflicts(files, get_work_changes(page->repository)->status);
	CardSource source = {side, NULL, NULL};
	/* The cards keep the full list alive, while showing only the files that are not conflicts */
	update_card_list(section->cards, page->window, &source, files, shown, page->has_shown);
	g_autofree char *count = format_file_count(shown->len);
	gtk_label_set_text(GTK_LABEL(section->count), count);
	reveal_part(page, section->revealer, shown->len > 0);
	reveal_part(page, section->cards_revealer, shown->len > 0);
}


static void show_changes(Repository *repository, ChangesPage *page) {
	const WorkChanges *changes = get_work_changes(repository);
	if (!changes)
		return;
	gboolean is_clean = is_status_clean(changes->status);
	gtk_widget_set_visible(page->empty, is_clean);
	show_operation(page, changes);
	show_conflicts(page, changes);
	show_section(page, &page->staged, changes->staged, SIDE_STAGED);
	show_section(page, &page->unstaged, changes->unstaged, SIDE_UNSTAGED);
	update_commit_box(page->commit_box, count_staged_files(changes->status), count_unstaged_files(changes->status));
	/* While git is stopped in the middle of something, Continue in the banner makes the commit */
	reveal_part(page, page->commit_revealer, !is_clean && get_repository_operation(repository) == OPERATION_NONE);
	page->has_shown = TRUE;
}


static GtkWidget *create_empty_state(void) {
	GtkWidget *empty = adw_status_page_new();
	adw_status_page_set_icon_name(ADW_STATUS_PAGE(empty), ILLUSTRATION_CLEAN);
	adw_status_page_set_title(ADW_STATUS_PAGE(empty), TEXT_NOTHING_CHANGED);
	adw_status_page_set_description(ADW_STATUS_PAGE(empty), TEXT_NOTHING_CHANGED_TEXT);
	gtk_widget_add_css_class(empty, "illustrated");
	gtk_widget_set_vexpand(empty, TRUE);
	gtk_widget_set_visible(empty, FALSE);
	return empty;
}


ChangesPage *create_changes_page(Window *window) {
	ChangesPage *page = g_new0(ChangesPage, 1);
	page->window = window;
	/* Each part keeps its own gap below it inside its revealer, so a hidden part leaves no gap behind */
	page->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(page->content, "page-content");
	gtk_box_append(GTK_BOX(page->content), create_operation_banner(page));
	page->conflict_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(page->conflict_box, "page-part");
	page->conflict_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(page->conflict_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_child(GTK_REVEALER(page->conflict_revealer), page->conflict_box);
	gtk_box_append(GTK_BOX(page->content), page->conflict_revealer);
	/* The commit box leads the page as a card, the way a commit's own page starts with its message */
	page->commit_box = create_commit_box(window);
	GtkWidget *commit_card = get_commit_box_widget(page->commit_box);
	gtk_widget_add_css_class(commit_card, "page-part");
	page->commit_revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(page->commit_revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_child(GTK_REVEALER(page->commit_revealer), commit_card);
	gtk_box_append(GTK_BOX(page->content), page->commit_revealer);
	create_staged_section(page);
	create_unstaged_section(page);
	gtk_box_append(GTK_BOX(page->content), page->staged.revealer);
	gtk_box_append(GTK_BOX(page->content), page->unstaged.revealer);
	gtk_box_append(GTK_BOX(page->content), page->staged.cards_revealer);
	gtk_box_append(GTK_BOX(page->content), page->unstaged.cards_revealer);
	page->empty = create_empty_state();
	gtk_box_append(GTK_BOX(page->content), page->empty);
	page->page = create_page_scroller(page->content);
	return page;
}


void glide_changes_page_to_top(ChangesPage *page) {
	glide_page_to_top(page->page);
}


GtkWidget *get_changes_page_widget(ChangesPage *page) {
	return page->page;
}


static void let_go_of_repository(ChangesPage *page) {
	if (page->repository) {
		g_clear_signal_handler(&page->changes_handler, page->repository);
		g_clear_object(&page->repository);
	}
}


void set_changes_page_repository(ChangesPage *page, Repository *repository) {
	let_go_of_repository(page);
	page->has_shown = FALSE;
	reset_commit_box(page->commit_box);
	if (!repository)
		return;
	page->repository = g_object_ref(repository);
	page->changes_handler = g_signal_connect(repository, "changes-loaded", G_CALLBACK(show_changes), page);
	show_changes(repository, page);
}


void refresh_changes_page(ChangesPage *page) {
	refresh_card_list(page->staged.cards);
	refresh_card_list(page->unstaged.cards);
}


/* The page's widgets are gone by now, so only the repository is let go, without resetting the commit box */
void free_changes_page(ChangesPage *page) {
	let_go_of_repository(page);
	free_card_list(page->staged.cards);
	free_card_list(page->unstaged.cards);
	free_commit_box(page->commit_box);
	g_free(page);
}
