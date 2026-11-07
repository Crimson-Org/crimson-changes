#include <string.h>
#include <adwaita.h>

#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "view/chips.h"
#include "view/layout.h"
#include "common/widgets/loading.h"
#include "view/diff-view.h"
#include "view/format.h"
#include "view/blame-page.h"
#include "view/file-history-page.h"
#include "view/file-card.h"

/* A diff longer than this starts folded, since it would push every other file far down */
#define LONGEST_OPEN_DIFF 400
#define CARD_KEY "file-card"
/* Pictures stay small, so a changed picture never outweighs the changes around it */
#define PICTURE_HEIGHT 140
#define ACTION_GROUP "card"
#define PICTURE_SUFFIXES_COUNT 9

static const char *const PICTURE_SUFFIXES[PICTURE_SUFFIXES_COUNT] = {
	".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".svg", ".ico", ".tga",
};

typedef struct {
	Window *window;
	DiffSide side;
	char *commit, *parent;
	GPtrArray *files;
	const DiffFile *file;
	GtkWidget *card, *revealer, *body, *chevron;
	gboolean has_body;
	guint hash;
} FileCard;


static void free_file_card(gpointer data) {
	FileCard *card = data;
	g_free(card->commit);
	g_free(card->parent);
	if (card->files)
		g_ptr_array_unref(card->files);
	g_free(card);
}


static FileCard *get_card(GtkWidget *widget) {
	return g_object_get_data(G_OBJECT(widget), CARD_KEY);
}


static int count_lines(const DiffFile *file) {
	int count = 0;
	for (guint index = 0; index < file->hunks->len; index++)
		count += (int)((DiffHunk *)g_ptr_array_index(file->hunks, index))->lines->len;
	return count;
}


static gboolean is_picture(const char *path) {
	g_autofree char *lower = g_ascii_strdown(path, -1);
	for (int index = 0; index < PICTURE_SUFFIXES_COUNT; index++)
		if (g_str_has_suffix(lower, PICTURE_SUFFIXES[index]))
			return TRUE;
	return FALSE;
}


/* Hunk actions */

typedef struct {
	FileCard *card;
	const DiffHunk *hunk;
} HunkButton;


static void press_hunk_stage(GtkButton *button, HunkButton *target) {
	if (target->card->side == SIDE_STAGED)
		unstage_hunk(target->card->window, target->card->file, target->hunk);
	else
		stage_hunk(target->card->window, target->card->file, target->hunk);
}


static void press_hunk_discard(GtkButton *button, HunkButton *target) {
	discard_hunk(target->card->window, target->card->file, target->hunk);
}


static GtkWidget *create_hunk_button(FileCard *card, const DiffHunk *hunk, const char *label, const char *tooltip,
	GCallback pressed) {
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_add_css_class(button, "hunk-action");
	HunkButton *target = g_new0(HunkButton, 1);
	target->card = card;
	target->hunk = hunk;
	g_signal_connect_data(button, "clicked", pressed, target, (GClosureNotify)(void (*)(void))g_free, 0);
	return button;
}


/* The line range and the function git found, with the hunk's own actions, which show on hover */
static GtkWidget *create_hunk_header(FileCard *card, const DiffHunk *hunk) {
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	gtk_widget_add_css_class(header, "hunk-header");
	int first = hunk->new_count > 0 ? hunk->new_start : hunk->old_start;
	int count = MAX(hunk->new_count, hunk->old_count);
	g_autofree char *range = count > 1 ? g_strdup_printf(TEXT_LINE_RANGE, first, first + count - 1) :
		g_strdup_printf(TEXT_LINE_SINGLE, first);
	GtkWidget *lines = gtk_label_new(range);
	gtk_widget_add_css_class(lines, "hunk-range");
	gtk_box_append(GTK_BOX(header), lines);
	/* The function git found above the change shows on hover, so the header stays one short line */
	gtk_widget_set_hexpand(lines, TRUE);
	gtk_label_set_xalign(GTK_LABEL(lines), 0);
	if (*hunk->section)
		gtk_widget_set_tooltip_text(lines, hunk->section);
	/* A diff that leaves out whitespace changes is not what is in the file, so its hunks cannot be staged alone */
	gboolean ignores_whitespace = get_diff_options(get_window_repository(card->window))->ignores_whitespace;
	gboolean has_actions = (card->side == SIDE_UNSTAGED || card->side == SIDE_STAGED) &&
		card->file->kind != CHANGE_UNTRACKED && !card->file->is_binary && !ignores_whitespace;
	if (!has_actions)
		return header;
	if (card->side == SIDE_UNSTAGED)
		gtk_box_append(GTK_BOX(header), create_hunk_button(card, hunk, TEXT_DISCARD, TEXT_DISCARD_HUNK_TOOLTIP,
			G_CALLBACK(press_hunk_discard)));
	gboolean is_staged = card->side == SIDE_STAGED;
	gtk_box_append(GTK_BOX(header), create_hunk_button(card, hunk, is_staged ? TEXT_UNSTAGE : TEXT_STAGE,
		is_staged ? TEXT_UNSTAGE_HUNK_TOOLTIP : TEXT_STAGE_HUNK_TOOLTIP, G_CALLBACK(press_hunk_stage)));
	return header;
}


/* Pictures */

/* A picture is read and decoded away from the window, since a large one takes a while */
typedef struct {
	GtkWidget *picture;
	GtkWidget *frame;
	GBytes *bytes;
	char *path;
} PictureTarget;


static void free_picture_target(gpointer data) {
	PictureTarget *target = data;
	g_object_unref(target->frame);
	if (target->bytes)
		g_bytes_unref(target->bytes);
	g_free(target->path);
	g_free(target);
}


static void decode_picture(GTask *task, gpointer source, gpointer data, GCancellable *cancellable) {
	PictureTarget *target = data;
	GError *error = NULL;
	GdkTexture *texture = target->bytes ? gdk_texture_new_from_bytes(target->bytes, &error) :
		gdk_texture_new_from_filename(target->path, &error);
	if (texture)
		g_task_return_pointer(task, texture, g_object_unref);
	else
		g_task_return_error(task, error);
}


static void show_picture(GObject *source, GAsyncResult *result, gpointer data) {
	PictureTarget *target = g_task_get_task_data(G_TASK(result));
	g_autoptr(GdkTexture) texture = g_task_propagate_pointer(G_TASK(result), NULL);
	if (texture) {
		gtk_picture_set_paintable(GTK_PICTURE(target->picture), GDK_PAINTABLE(texture));
		fade_in_widget(target->picture);
	} else
		gtk_widget_set_visible(target->frame, FALSE);
}


static void start_decoding(PictureTarget *target) {
	g_autoptr(GTask) task = g_task_new(target->frame, NULL, show_picture, NULL);
	g_task_set_task_data(task, target, free_picture_target);
	g_task_run_in_thread(task, decode_picture);
}


static void read_picture(GitResult *result, gpointer data) {
	PictureTarget *target = data;
	if (result->is_cancelled || !has_git_succeeded(result)) {
		if (!result->is_cancelled)
			gtk_widget_set_visible(target->frame, FALSE);
		free_picture_target(target);
		return;
	}
	target->bytes = g_bytes_ref(result->output);
	start_decoding(target);
}


static GtkWidget *create_picture_side(FileCard *card, const char *caption, const char *revision, const char *path) {
	GtkWidget *picture = gtk_picture_new();
	gtk_picture_set_content_fit(GTK_PICTURE(picture), GTK_CONTENT_FIT_CONTAIN);
	gtk_picture_set_can_shrink(GTK_PICTURE(picture), TRUE);
	gtk_widget_set_size_request(picture, -1, PICTURE_HEIGHT);
	gtk_widget_add_css_class(picture, "picture-side");
	GtkWidget *label = gtk_label_new(caption);
	gtk_widget_add_css_class(label, "dim-label");
	gtk_widget_add_css_class(label, "caption");
	GtkWidget *frame = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_SMALL);
	gtk_widget_set_hexpand(frame, TRUE);
	gtk_box_append(GTK_BOX(frame), picture);
	gtk_box_append(GTK_BOX(frame), label);
	PictureTarget *target = g_new0(PictureTarget, 1);
	target->picture = picture;
	target->frame = g_object_ref(frame);
	Repository *repository = get_window_repository(card->window);
	if (!revision) {
		target->path = g_build_filename(get_repository_folder(repository), path, NULL);
		start_decoding(target);
	} else {
		g_autofree char *object = g_strdup_printf("%s:%s", revision, path);
		const char *arguments[] = {"show", object, NULL};
		read_repository(repository, arguments, read_picture, target);
	}
	return frame;
}


/* A changed picture shows before and after side by side, read from git and the work folder */
static GtkWidget *create_picture_comparison(FileCard *card) {
	const DiffFile *file = card->file;
	const char *old_path = file->old_path ? file->old_path : file->path;
	const char *old_revision = card->side == SIDE_UNSTAGED ? "" : card->side == SIDE_STAGED ? "HEAD" : card->parent;
	const char *new_revision = card->side == SIDE_UNSTAGED ? NULL : card->side == SIDE_STAGED ? "" : card->commit;
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_NORMAL);
	gtk_widget_add_css_class(box, "picture-comparison");
	gboolean has_old = file->kind != CHANGE_ADDED && file->kind != CHANGE_UNTRACKED && old_revision;
	if (has_old)
		gtk_box_append(GTK_BOX(box), create_picture_side(card, TEXT_BEFORE, old_revision, old_path));
	if (file->kind != CHANGE_DELETED)
		gtk_box_append(GTK_BOX(box), create_picture_side(card, has_old ? TEXT_AFTER : TEXT_NEW_PICTURE,
			new_revision, file->path));
	return box;
}


/* The body */

static GtkWidget *create_note(const char *text) {
	GtkWidget *label = gtk_label_new(text);
	gtk_widget_add_css_class(label, "card-note");
	gtk_label_set_wrap(GTK_LABEL(label), TRUE);
	return label;
}


static void fill_body(FileCard *card) {
	const DiffFile *file = card->file;
	card->has_body = TRUE;
	if (file->is_binary && is_picture(file->path)) {
		gtk_box_append(GTK_BOX(card->body), create_picture_comparison(card));
		return;
	}
	if (file->is_binary) {
		gtk_box_append(GTK_BOX(card->body), create_note(TEXT_BINARY_FILE));
		return;
	}
	if (file->is_large) {
		gtk_box_append(GTK_BOX(card->body), create_note(TEXT_LARGE_FILE));
		return;
	}
	if (file->hunks->len == 0) {
		gtk_box_append(GTK_BOX(card->body), create_note(file->is_mode_change ? TEXT_MODE_CHANGE :
			file->kind == CHANGE_RENAMED ? TEXT_ONLY_RENAMED : TEXT_EMPTY_FILE));
		return;
	}
	for (guint index = 0; index < file->hunks->len; index++) {
		const DiffHunk *hunk = g_ptr_array_index(file->hunks, index);
		GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
		gtk_widget_add_css_class(box, "hunk");
		gtk_box_append(GTK_BOX(box), create_hunk_header(card, hunk));
		gtk_box_append(GTK_BOX(box), create_hunk_view(file, hunk));
		gtk_box_append(GTK_BOX(card->body), box);
	}
}


void set_file_card_expanded(GtkWidget *widget, gboolean is_expanded) {
	FileCard *card = get_card(widget);
	if (is_expanded && !card->has_body)
		fill_body(card);
	gtk_revealer_set_reveal_child(GTK_REVEALER(card->revealer), is_expanded);
	gtk_image_set_from_icon_name(GTK_IMAGE(card->chevron), is_expanded ? "pan-down-symbolic" : "pan-end-symbolic");
	if (is_expanded)
		gtk_widget_add_css_class(card->card, "expanded");
	else
		gtk_widget_remove_css_class(card->card, "expanded");
}


void refresh_file_card(GtkWidget *widget) {
	FileCard *card = get_card(widget);
	if (!card || !card->has_body)
		return;
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(card->body)))
		gtk_box_remove(GTK_BOX(card->body), child);
	card->has_body = FALSE;
	if (gtk_revealer_get_reveal_child(GTK_REVEALER(card->revealer)))
		fill_body(card);
}


static void toggle_card(GtkGestureClick *gesture, int presses, double x, double y, FileCard *card) {
	set_file_card_expanded(card->card, !gtk_revealer_get_reveal_child(GTK_REVEALER(card->revealer)));
}


/* The header */

static const char *get_kind_text(const DiffFile *file) {
	switch (file->kind) {
	case CHANGE_ADDED:
	case CHANGE_UNTRACKED:
		return TEXT_KIND_NEW;
	case CHANGE_DELETED:
		return TEXT_KIND_REMOVED;
	case CHANGE_RENAMED:
		return TEXT_KIND_RENAMED;
	case CHANGE_COPIED:
		return TEXT_KIND_COPIED;
	default:
		return NULL;
	}
}


static const char *get_kind_class(const DiffFile *file) {
	switch (file->kind) {
	case CHANGE_ADDED:
	case CHANGE_UNTRACKED:
		return "new";
	case CHANGE_DELETED:
		return "removed";
	default:
		return "moved";
	}
}


static GtkWidget *create_name(const DiffFile *file) {
	g_autofree char *folder = get_path_folder(file->path);
	g_autofree char *markup = g_markup_printf_escaped("<span alpha=\"60%%\">%s</span><b>%s</b>", folder,
		get_path_name(file->path));
	GtkWidget *name = gtk_label_new(NULL);
	gtk_label_set_markup(GTK_LABEL(name), markup);
	gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_START);
	gtk_label_set_xalign(GTK_LABEL(name), 0);
	gtk_widget_set_tooltip_text(name, file->path);
	return name;
}


static GtkWidget *create_counts(const DiffFile *file) {
	GtkWidget *counts = create_change_counts();
	set_change_counts(counts, NULL, file->added, file->removed);
	gtk_widget_add_css_class(counts, "line-counts");
	gtk_widget_set_valign(counts, GTK_ALIGN_CENTER);
	gtk_widget_set_visible(counts, file->added + file->removed > 0);
	return counts;
}


static void press_stage(GtkButton *button, FileCard *card) {
	if (card->side == SIDE_STAGED)
		unstage_file(card->window, card->file);
	else
		stage_file(card->window, card->file);
}


static void press_discard(GtkButton *button, FileCard *card) {
	discard_file(card->window, card->file);
}


static GtkWidget *create_icon_button(const char *icon, const char *tooltip, GCallback pressed, FileCard *card) {
	GtkWidget *button = gtk_button_new_from_icon_name(icon);
	gtk_widget_set_tooltip_text(button, tooltip);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	g_signal_connect(button, "clicked", pressed, card);
	return button;
}


static void run_blame(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	const char *revision = card->side == SIDE_COMMIT ? card->commit : NULL;
	show_blame_page(card->window, card->file->path, revision);
}


static void run_history(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	show_file_history_page(card->window, card->file->path);
}


static void run_open(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	open_work_file(card->window, card->file->path);
}


static void run_folder(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	open_work_file_folder(card->window, card->file->path);
}


static void run_copy_path(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	copy_text(card->window, card->file->path, TEXT_PATH_COPIED);
}


static void run_restore(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	restore_file_version(card->window, card->commit, card->file);
}


static void run_discard(GSimpleAction *action, GVariant *parameter, FileCard *card) {
	discard_file(card->window, card->file);
}


static GtkWidget *create_menu_button(FileCard *card) {
	gboolean is_new = card->file->kind == CHANGE_UNTRACKED || card->file->kind == CHANGE_ADDED;
	gboolean exists_now = card->side != SIDE_COMMIT || card->file->kind != CHANGE_DELETED;
	const GActionEntry ENTRIES[] = {
		{"blame", (void (*)(GSimpleAction *, GVariant *, gpointer))run_blame, NULL, NULL, NULL, {0}},
		{"history", (void (*)(GSimpleAction *, GVariant *, gpointer))run_history, NULL, NULL, NULL, {0}},
		{"open", (void (*)(GSimpleAction *, GVariant *, gpointer))run_open, NULL, NULL, NULL, {0}},
		{"folder", (void (*)(GSimpleAction *, GVariant *, gpointer))run_folder, NULL, NULL, NULL, {0}},
		{"copy-path", (void (*)(GSimpleAction *, GVariant *, gpointer))run_copy_path, NULL, NULL, NULL, {0}},
		{"restore", (void (*)(GSimpleAction *, GVariant *, gpointer))run_restore, NULL, NULL, NULL, {0}},
		{"discard", (void (*)(GSimpleAction *, GVariant *, gpointer))run_discard, NULL, NULL, NULL, {0}},
	};
	g_autoptr(GSimpleActionGroup) group = g_simple_action_group_new();
	g_action_map_add_action_entries(G_ACTION_MAP(group), ENTRIES, G_N_ELEMENTS(ENTRIES), card);
	gtk_widget_insert_action_group(card->card, ACTION_GROUP, G_ACTION_GROUP(group));
	g_autoptr(GMenu) menu = g_menu_new();
	g_autoptr(GMenu) looks = g_menu_new();
	gboolean has_lines = card->side == SIDE_COMMIT ? card->file->kind != CHANGE_DELETED : !is_new;
	if (has_lines)
		g_menu_append(looks, TEXT_BLAME, ACTION_GROUP ".blame");
	g_menu_append(looks, TEXT_FILE_HISTORY, ACTION_GROUP ".history");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(looks));
	g_autoptr(GMenu) files = g_menu_new();
	if (exists_now) {
		g_menu_append(files, TEXT_OPEN_FILE, ACTION_GROUP ".open");
		g_menu_append(files, TEXT_OPEN_FOLDER_OF_FILE, ACTION_GROUP ".folder");
	}
	g_menu_append(files, TEXT_COPY_PATH, ACTION_GROUP ".copy-path");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(files));
	g_autoptr(GMenu) changes = g_menu_new();
	if (card->side == SIDE_COMMIT)
		g_menu_append(changes, TEXT_RESTORE_VERSION, ACTION_GROUP ".restore");
	if (card->side == SIDE_UNSTAGED)
		g_menu_append(changes, TEXT_DISCARD_CHANGES, ACTION_GROUP ".discard");
	g_menu_append_section(menu, NULL, G_MENU_MODEL(changes));
	GtkWidget *button = gtk_menu_button_new();
	gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "view-more-symbolic");
	gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
	gtk_widget_set_tooltip_text(button, TEXT_FILE_MENU);
	gtk_widget_add_css_class(button, "flat");
	gtk_widget_set_valign(button, GTK_ALIGN_CENTER);
	return button;
}


static GtkWidget *create_header(FileCard *card) {
	const DiffFile *file = card->file;
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(header, "card-header");
	/* The fold arrow leads the header like every fold in the app, pointing right when folded and down when open */
	card->chevron = gtk_image_new_from_icon_name("pan-end-symbolic");
	gtk_widget_add_css_class(card->chevron, "dim-label");
	gtk_box_append(GTK_BOX(header), card->chevron);
	g_autoptr(GIcon) icon = get_file_icon(file->path);
	GtkWidget *image = gtk_image_new_from_gicon(icon);
	gtk_box_append(GTK_BOX(header), image);
	/* The kind and the counts follow the name, and the buttons that show on hover keep to the far end */
	GtkWidget *name = create_name(file);
	gtk_box_append(GTK_BOX(header), name);
	const char *kind = get_kind_text(file);
	if (kind) {
		GtkWidget *badge = gtk_label_new(kind);
		gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(badge, "kind-badge");
		gtk_widget_add_css_class(badge, get_kind_class(file));
		if (file->old_path) {
			g_autofree char *tooltip = g_strdup_printf(TEXT_RENAMED_FROM, file->old_path);
			gtk_widget_set_tooltip_text(badge, tooltip);
		}
		gtk_box_append(GTK_BOX(header), badge);
	}
	gtk_box_append(GTK_BOX(header), create_counts(file));
	GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
	gtk_widget_set_hexpand(spacer, TRUE);
	gtk_box_append(GTK_BOX(header), spacer);
	if (card->side == SIDE_UNSTAGED) {
		GtkWidget *discard = create_icon_button("edit-undo-symbolic", TEXT_DISCARD_FILE_TOOLTIP,
			G_CALLBACK(press_discard), card);
		gtk_widget_add_css_class(discard, "card-action");
		gtk_box_append(GTK_BOX(header), discard);
	}
	/* Staging is the main thing done with a card, so its button says what it does */
	if (card->side == SIDE_UNSTAGED || card->side == SIDE_STAGED) {
		gboolean is_staged = card->side == SIDE_STAGED;
		GtkWidget *stage = gtk_button_new_with_label(is_staged ? TEXT_UNSTAGE : TEXT_STAGE);
		gtk_widget_set_tooltip_text(stage, is_staged ? TEXT_UNSTAGE_FILE_TOOLTIP : TEXT_STAGE_FILE_TOOLTIP);
		gtk_widget_add_css_class(stage, "stage-button");
		gtk_widget_add_css_class(stage, "card-action");
		gtk_widget_add_css_class(stage, is_staged ? "staged" : "unstaged");
		gtk_widget_set_valign(stage, GTK_ALIGN_CENTER);
		g_signal_connect(stage, "clicked", G_CALLBACK(press_stage), card);
		gtk_box_append(GTK_BOX(header), stage);
	}
	GtkWidget *menu = create_menu_button(card);
	gtk_widget_add_css_class(menu, "card-action");
	gtk_box_append(GTK_BOX(header), menu);
	GtkGesture *click = gtk_gesture_click_new();
	g_signal_connect(click, "released", G_CALLBACK(toggle_card), card);
	gtk_widget_add_controller(header, GTK_EVENT_CONTROLLER(click));
	gtk_widget_set_cursor_from_name(header, "pointer");
	return header;
}


GtkWidget *create_file_overview_row(const DiffFile *file) {
	GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(row, "overview-row");
	g_autoptr(GIcon) icon = get_file_icon(file->path);
	gtk_box_append(GTK_BOX(row), gtk_image_new_from_gicon(icon));
	/* The kind and counts follow the name, the same as in the card's header */
	gtk_box_append(GTK_BOX(row), create_name(file));
	const char *kind = get_kind_text(file);
	if (kind) {
		GtkWidget *badge = gtk_label_new(kind);
		gtk_widget_set_valign(badge, GTK_ALIGN_CENTER);
		gtk_widget_add_css_class(badge, "kind-badge");
		gtk_widget_add_css_class(badge, get_kind_class(file));
		gtk_box_append(GTK_BOX(row), badge);
	}
	gtk_box_append(GTK_BOX(row), create_counts(file));
	return row;
}


GtkWidget *create_file_card(Window *window, const CardSource *source, GPtrArray *files, const DiffFile *file,
	gboolean is_expanded) {
	FileCard *card = g_new0(FileCard, 1);
	card->window = window;
	card->side = source->side;
	card->commit = g_strdup(source->commit);
	card->parent = g_strdup(source->parent);
	card->files = g_ptr_array_ref(files);
	card->file = file;
	card->hash = hash_diff_file(file) + (guint)source->side;
	card->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(card->card, "file-card");
	g_object_set_data_full(G_OBJECT(card->card), CARD_KEY, card, free_file_card);
	gtk_box_append(GTK_BOX(card->card), create_header(card));
	card->body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(card->body, "card-body");
	card->revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(card->revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(GTK_REVEALER(card->revealer), 0);
	gtk_revealer_set_child(GTK_REVEALER(card->revealer), card->body);
	gtk_box_append(GTK_BOX(card->card), card->revealer);
	/* A new card starts as it should look, and only the user's own folding slides */
	/* A file with nothing to show line by line, such as one only moved, starts folded */
	gboolean has_lines = file->hunks->len > 0 || file->is_binary;
	set_file_card_expanded(card->card, is_expanded && has_lines && count_lines(file) <= LONGEST_OPEN_DIFF);
	gtk_revealer_set_transition_duration(GTK_REVEALER(card->revealer), SLIDE_MILLISECONDS);
	return card->card;
}


guint get_file_card_hash(GtkWidget *widget) {
	FileCard *card = get_card(widget);
	return card ? card->hash : 0;
}


/* Conflicts */

typedef struct {
	Window *window;
	char *path;
} ConflictCard;


static void free_conflict_card(gpointer data) {
	ConflictCard *card = data;
	g_free(card->path);
	g_free(card);
}


static void press_ours(GtkButton *button, ConflictCard *card) {
	keep_our_version(card->window, card->path);
}


static void press_theirs(GtkButton *button, ConflictCard *card) {
	keep_their_version(card->window, card->path);
}


static void press_resolved(GtkButton *button, ConflictCard *card) {
	mark_resolved(card->window, card->path);
}


static void press_open(GtkButton *button, ConflictCard *card) {
	open_work_file(card->window, card->path);
}


static GtkWidget *create_conflict_button(ConflictCard *card, const char *label, const char *tooltip,
	GCallback pressed) {
	GtkWidget *button = gtk_button_new_with_label(label);
	gtk_widget_set_tooltip_text(button, tooltip);
	g_signal_connect(button, "clicked", pressed, card);
	return button;
}


GtkWidget *create_conflict_card(Window *window, const char *path) {
	ConflictCard *card = g_new0(ConflictCard, 1);
	card->window = window;
	card->path = g_strdup(path);
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
	gtk_widget_add_css_class(box, "file-card");
	gtk_widget_add_css_class(box, "conflict");
	g_object_set_data_full(G_OBJECT(box), "conflict-card", card, free_conflict_card);
	GtkWidget *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(header, "card-header");
	gtk_box_append(GTK_BOX(header), gtk_image_new_from_icon_name("dialog-warning-symbolic"));
	DiffFile named = {.path = (char *)path};
	GtkWidget *name = create_name(&named);
	gtk_widget_set_hexpand(name, TRUE);
	gtk_box_append(GTK_BOX(header), name);
	GtkWidget *badge = gtk_label_new(TEXT_KIND_CONFLICT);
	gtk_widget_add_css_class(badge, "kind-badge");
	gtk_widget_add_css_class(badge, "conflict");
	gtk_box_append(GTK_BOX(header), badge);
	gtk_box_append(GTK_BOX(box), header);
	GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SNUG);
	gtk_widget_add_css_class(actions, "conflict-actions");
	gtk_box_append(GTK_BOX(actions), create_conflict_button(card, TEXT_OPEN_FILE, TEXT_OPEN_CONFLICT_TOOLTIP,
		G_CALLBACK(press_open)));
	gtk_box_append(GTK_BOX(actions), create_conflict_button(card, TEXT_KEEP_OURS, TEXT_KEEP_OURS_TOOLTIP,
		G_CALLBACK(press_ours)));
	gtk_box_append(GTK_BOX(actions), create_conflict_button(card, TEXT_KEEP_THEIRS, TEXT_KEEP_THEIRS_TOOLTIP,
		G_CALLBACK(press_theirs)));
	GtkWidget *resolved = create_conflict_button(card, TEXT_MARK_RESOLVED, TEXT_MARK_RESOLVED_TOOLTIP,
		G_CALLBACK(press_resolved));
	gtk_widget_add_css_class(resolved, "suggested-action");
	gtk_box_append(GTK_BOX(actions), resolved);
	gtk_box_append(GTK_BOX(box), actions);
	return box;
}
