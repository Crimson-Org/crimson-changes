#include <string.h>
#include <adwaita.h>

#include "view/icons.h"
#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "view/layout.h"
#include "view/page-scroller.h"
#include "common/widgets/loading.h"
#include "view/card-list.h"
#include "view/chips.h"
#include "view/file-actions.h"
#include "view/format.h"
#include "view/commit-page.h"

#define AVATAR_SIZE 40
/* A commit of one file needs no list of its files */
#define FEWEST_OVERVIEW_FILES 2
#define LOADING_PAGE "loading"
#define CONTENT_PAGE "content"
#define GOOD_SIGNATURE 'G'
#define UNTRUSTED_SIGNATURE 'U'
#define BAD_SIGNATURE 'B'
#define NO_SIGNATURE 'N'

struct CommitPage {
	Window *window;
	GtkWidget *stack, *content, *subject, *body, *avatar, *stash_mark, *author, *time, *hash_button, *chips, *summary;
	GtkWidget *stash_actions, *meta, *summary_count, *scroller;
	CardList *cards;
	Commit *commit;
	char *stash_name;
	GCancellable *loading;
	/* Both reads must be back before the page shows, so it never shows half a commit */
	Commit *details;
	GPtrArray *files;
	int waiting;
};


static void show_details(CommitPage *page) {
	const Commit *commit = page->details ? page->details : page->commit;
	/* The stash list already gave the stash's own message, without where it was made */
	gtk_label_set_text(GTK_LABEL(page->subject), page->stash_name ? page->commit->subject : commit->subject);
	gboolean has_body = commit->body && *commit->body;
	gtk_label_set_text(GTK_LABEL(page->body), has_body ? commit->body : "");
	gtk_widget_set_visible(page->body, has_body);
	gboolean is_stash = page->stash_name != NULL;
	/* A stash shows its mark where a commit shows its author, so every header card has the same shape */
	adw_avatar_set_text(ADW_AVATAR(page->avatar), commit->author_name);
	gtk_widget_set_visible(page->avatar, !is_stash);
	gtk_widget_set_visible(page->stash_mark, is_stash);
	gtk_label_set_text(GTK_LABEL(page->author), commit->author_name ? commit->author_name : "");
	/* A stash has no author worth naming, and the header already says what it is */
	gtk_widget_set_visible(page->author, !is_stash);
	gtk_widget_set_tooltip_text(page->author, commit->author_email);
	g_autofree char *ago = format_time_ago(commit->commit_time);
	g_autofree char *time = g_strdup_printf(is_stash ? TEXT_STASHED_AGO : TEXT_COMMITTED_AGO, ago);
	gtk_label_set_text(GTK_LABEL(page->time), time);
	g_autofree char *full_time = format_full_time(commit->commit_time);
	gtk_widget_set_tooltip_text(page->time, full_time);
	g_autofree char *short_hash = get_short_hash(commit->hash);
	gtk_button_set_label(GTK_BUTTON(page->hash_button), short_hash);
	gtk_widget_set_visible(page->hash_button, !is_stash);
	fill_reference_chips(page->chips, commit->references);
	/* A stash keeps the files git did not track in a second parent, which makes it no merge to the user */
	if (is_merge_commit(commit) && !is_stash) {
		gtk_box_prepend(GTK_BOX(page->chips), create_chip(TEXT_MERGE_CHIP, "merge"));
		gtk_widget_set_visible(page->chips, TRUE);
	}
	const char *signature = commit->signature == GOOD_SIGNATURE ? TEXT_SIGNED :
		commit->signature == UNTRUSTED_SIGNATURE ? TEXT_SIGNED_UNTRUSTED :
		commit->signature == BAD_SIGNATURE ? TEXT_BAD_SIGNATURE : NULL;
	if (signature) {
		GtkWidget *chip = create_chip(signature, commit->signature == BAD_SIGNATURE ? "bad" : "signed");
		gtk_widget_set_tooltip_text(chip, commit->signer);
		gtk_box_append(GTK_BOX(page->chips), chip);
		gtk_widget_set_visible(page->chips, TRUE);
	}
	gtk_widget_set_visible(page->stash_actions, is_stash);
}


static void show_summary(CommitPage *page) {
	int added = 0, removed = 0;
	for (guint index = 0; index < page->files->len; index++) {
		DiffFile *file = g_ptr_array_index(page->files, index);
		added += file->added;
		removed += file->removed;
	}
	g_autofree char *files = format_file_count(page->files->len);
	set_change_counts(page->summary_count, files, added, removed);
}


static void finish_part(CommitPage *page) {
	if (--page->waiting > 0)
		return;
	/* A diff that could not be read shows the commit without files, never the last commit's files */
	if (!page->files)
		page->files = g_ptr_array_new_with_free_func(free_diff_file);
	show_details(page);
	show_summary(page);
	const char *parent = page->commit->parents && page->commit->parents[0] ? page->commit->parents[0] : NULL;
	CardSource source = {SIDE_COMMIT, page->commit->hash, parent};
	update_card_list(page->cards, page->window, &source, page->files, page->files, FALSE);
	gtk_stack_set_visible_child_name(GTK_STACK(page->stack), CONTENT_PAGE);
	fade_in_widget(page->content);
}


static void read_details(GitResult *result, gpointer data) {
	if (result->is_cancelled)
		return;
	CommitPage *page = data;
	if (has_git_succeeded(result)) {
		gsize length = 0;
		const char *output = g_bytes_get_data(result->output, &length);
		free_commit(page->details);
		page->details = parse_commit_details(output ? output : "", length);
	}
	finish_part(page);
}


static void read_files(GitResult *result, gpointer data) {
	if (result->is_cancelled)
		return;
	CommitPage *page = data;
	g_clear_pointer(&page->files, g_ptr_array_unref);
	if (has_git_succeeded(result)) {
		gsize length = 0;
		const char *output = g_bytes_get_data(result->output, &length);
		page->files = parse_diff(output ? output : "", length);
	} else
		show_git_failure(page->window, TEXT_COMMIT_READ_FAILED, result);
	finish_part(page);
}


static void load_commit(CommitPage *page) {
	Repository *repository = get_window_repository(page->window);
	if (page->loading)
		g_cancellable_cancel(page->loading);
	g_clear_object(&page->loading);
	page->loading = g_cancellable_new();
	page->waiting = 2;
	GitOptions options = {.cancellable = page->loading};
	const char *details[] = {"show", "-s", DECORATE_OPTION, DETAILS_FORMAT, page->commit->hash, NULL};
	run_git(get_repository_folder(repository), details, &options, read_details, page);
	g_autoptr(GPtrArray) arguments = g_ptr_array_new_with_free_func(g_free);
	g_ptr_array_add(arguments, g_strdup("show"));
	g_ptr_array_add(arguments, g_strdup("--format="));
	g_ptr_array_add(arguments, g_strdup("--patch"));
	/* A merge shows what it brought into the branch, and a stash what it put aside */
	g_ptr_array_add(arguments, g_strdup("--diff-merges=first-parent"));
	add_diff_arguments(repository, arguments);
	g_ptr_array_add(arguments, g_strdup(page->commit->hash));
	g_ptr_array_add(arguments, g_strdup("--"));
	g_ptr_array_add(arguments, NULL);
	run_git(get_repository_folder(repository), (const char *const *)arguments->pdata, &options, read_files, page);
}


void show_commit_page(CommitPage *page, const Commit *commit, const char *stash_name) {
	free_commit(page->commit);
	page->commit = copy_commit(commit);
	g_set_str(&page->stash_name, stash_name);
	g_clear_pointer(&page->details, free_commit);
	gtk_stack_set_visible_child_name(GTK_STACK(page->stack), LOADING_PAGE);
	load_commit(page);
}


void reload_commit_page(CommitPage *page) {
	if (page->commit)
		load_commit(page);
}


const char *get_commit_page_hash(CommitPage *page) {
	return page->commit ? page->commit->hash : NULL;
}


static void copy_hash(GtkButton *button, CommitPage *page) {
	copy_text(page->window, page->commit->hash, TEXT_HASH_COPIED);
}


/* The page's button runs the same action as the stash's menu in the header, which also holds Delete */
static void press_pop(GtkButton *button, CommitPage *page) {
	gtk_widget_activate_action(GTK_WIDGET(button), "commit.stash-pop", NULL);
}


static GtkWidget *create_stash_actions(CommitPage *page) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	GtkWidget *pop = gtk_button_new_with_label(TEXT_STASH_POP);
	gtk_widget_add_css_class(pop, "suggested-action");
	gtk_widget_set_tooltip_text(pop, TEXT_STASH_POP_TOOLTIP);
	g_signal_connect(pop, "clicked", G_CALLBACK(press_pop), page);
	gtk_box_append(GTK_BOX(box), pop);
	gtk_widget_set_halign(box, GTK_ALIGN_START);
	return box;
}


static GtkWidget *create_label(const char *css_class, gboolean wraps) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_label_set_xalign(GTK_LABEL(label), 0);
	gtk_label_set_selectable(GTK_LABEL(label), wraps);
	gtk_label_set_wrap(GTK_LABEL(label), wraps);
	gtk_label_set_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
	if (css_class)
		gtk_widget_add_css_class(label, css_class);
	return label;
}


static GtkWidget *create_header(CommitPage *page) {
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(header, "commit-header");
	page->subject = create_label("commit-subject", TRUE);
	page->body = create_label("commit-body", TRUE);
	page->avatar = adw_avatar_new(AVATAR_SIZE, NULL, TRUE);
	/* The same grey box a stash has in the list, in a circle the size of an author's picture */
	page->stash_mark = gtk_image_new_from_icon_name(ICON_STASH);
	gtk_widget_set_size_request(page->stash_mark, AVATAR_SIZE, AVATAR_SIZE);
	gtk_widget_add_css_class(page->stash_mark, "stash-mark");
	page->author = create_label("heading", FALSE);
	page->time = create_label("dim-label", FALSE);
	GtkWidget *who = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_HAIRLINE);
	gtk_widget_set_valign(who, GTK_ALIGN_CENTER);
	gtk_widget_set_hexpand(who, TRUE);
	gtk_box_append(GTK_BOX(who), page->author);
	gtk_box_append(GTK_BOX(who), page->time);
	page->hash_button = gtk_button_new();
	gtk_widget_add_css_class(page->hash_button, "hash-button");
	gtk_widget_add_css_class(page->hash_button, "monospace");
	gtk_widget_set_valign(page->hash_button, GTK_ALIGN_CENTER);
	gtk_widget_set_tooltip_text(page->hash_button, TEXT_COPY_HASH_TOOLTIP);
	g_signal_connect(page->hash_button, "clicked", G_CALLBACK(copy_hash), page);
	page->meta = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_box_append(GTK_BOX(page->meta), page->avatar);
	gtk_box_append(GTK_BOX(page->meta), page->stash_mark);
	gtk_box_append(GTK_BOX(page->meta), who);
	gtk_box_append(GTK_BOX(page->meta), page->hash_button);
	page->chips = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	page->stash_actions = create_stash_actions(page);
	gtk_box_append(GTK_BOX(header), page->meta);
	gtk_box_append(GTK_BOX(header), page->subject);
	gtk_box_append(GTK_BOX(header), page->body);
	gtk_box_append(GTK_BOX(header), page->chips);
	gtk_box_append(GTK_BOX(header), page->stash_actions);
	return header;
}


CommitPage *create_commit_page(Window *window) {
	CommitPage *page = g_new0(CommitPage, 1);
	page->window = window;
	page->cards = create_card_list(FEWEST_OVERVIEW_FILES);
	/* The files heading looks like the headings over the changes not committed yet */
	page->summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(page->summary, "section-heading");
	GtkWidget *summary_title = create_label("section-title", FALSE);
	gtk_label_set_text(GTK_LABEL(summary_title), TEXT_CHANGED_FILES);
	page->summary_count = create_change_counts();
	gtk_widget_add_css_class(page->summary_count, "section-count");
	gtk_box_append(GTK_BOX(page->summary), summary_title);
	gtk_box_append(GTK_BOX(page->summary), page->summary_count);
	/* Parts of the page sit apart by the large gap, and a heading sits on its list by the normal one */
	page->content = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_LARGE);
	gtk_widget_add_css_class(page->content, "page-content");
	gtk_box_append(GTK_BOX(page->content), create_header(page));
	/* The files heading sits on its list by the same gap as the headings over the changes not committed yet */
	GtkWidget *files = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_box_append(GTK_BOX(files), page->summary);
	gtk_box_append(GTK_BOX(files), get_card_list_overview(page->cards));
	gtk_box_append(GTK_BOX(page->content), files);
	gtk_box_append(GTK_BOX(page->content), get_card_list_widget(page->cards));
	page->scroller = create_page_scroller(page->content);
	GtkWidget *spinner = create_loading_spinner();
	gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
	gtk_widget_set_valign(spinner, GTK_ALIGN_CENTER);
	page->stack = gtk_stack_new();
	gtk_stack_add_named(GTK_STACK(page->stack), spinner, LOADING_PAGE);
	gtk_stack_add_named(GTK_STACK(page->stack), page->scroller, CONTENT_PAGE);
	return page;
}


void set_commit_page_menu(CommitPage *page, GtkWidget *menu_button) {
	gtk_widget_set_valign(menu_button, GTK_ALIGN_CENTER);
	gtk_widget_add_css_class(menu_button, "flat");
	gtk_box_append(GTK_BOX(page->meta), menu_button);
}


void glide_commit_page_to_top(CommitPage *page) {
	glide_page_to_top(page->scroller);
}


GtkWidget *get_commit_page_widget(CommitPage *page) {
	return page->stack;
}


void free_commit_page(CommitPage *page) {
	if (page->loading)
		g_cancellable_cancel(page->loading);
	g_clear_object(&page->loading);
	free_card_list(page->cards);
	free_commit(page->commit);
	free_commit(page->details);
	g_clear_pointer(&page->files, g_ptr_array_unref);
	g_free(page->stash_name);
	g_free(page);
}
