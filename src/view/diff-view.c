#include <string.h>
#include <adwaita.h>
#include <gtksourceview/gtksource.h>

#include "lang/en.h"
#include "app/settings.h"
#include "git/word-diff.h"
#include "view/diff-view.h"
#include "view/colours.h"
#include "common/app/identity.h"

/* Adwaita's colours with line numbers on the card's own colour, from the program's resources */
#define LIGHT_SCHEME "crimson-changes-light"
#define DARK_SCHEME "crimson-changes-dark"
#define SCHEMES_RESOURCE "schemes"
#define NUMBER_PADDING 6
#define SIGN_PADDING 8
/* Numbers line up on their right, and a sign sits in the middle of the space before the code */
#define NUMBER_ALIGN 1.0f
#define SIGN_ALIGN 0.5f
/* Most projects indent by four, and eight pushes deep code off the card */
#define TAB_WIDTH 4
#define CODE_EDGE_GAP 6
#define NUMBER_CLASS "line-number"
#define SIGN_CLASS "sign"
#define LINE_INFO_KEY "line-info"
#define ADDED_LINE_TAG "added-line"
#define REMOVED_LINE_TAG "removed-line"
#define ADDED_WORD_TAG "added-word"
#define REMOVED_WORD_TAG "removed-word"
#define NOTE_TAG "note"
#define ADDED_SIGN "+"
#define REMOVED_SIGN "−"
/* Green and red tint a line softly and a changed word more, the same on light and dark backgrounds */
#define LINE_TINT 0.18
#define WORD_TINT 0.34
/* The note about a missing line break is grey on any background */
#define NOTE_COLOUR "#808080"
#define NOTE_ALPHA 0.9
#define TAG_PRIORITY_HIGHEST -1

/* What the gutter shows beside each line of the buffer */
typedef struct {
	GArray *old_numbers, *new_numbers;
	GArray *kinds;
} LineInfo;


static void free_line_info(gpointer data) {
	LineInfo *info = data;
	g_array_unref(info->old_numbers);
	g_array_unref(info->new_numbers);
	g_array_unref(info->kinds);
	g_free(info);
}


static LineInfo *create_line_info(void) {
	LineInfo *info = g_new0(LineInfo, 1);
	info->old_numbers = g_array_new(FALSE, FALSE, sizeof(int));
	info->new_numbers = g_array_new(FALSE, FALSE, sizeof(int));
	info->kinds = g_array_new(FALSE, FALSE, sizeof(char));
	return info;
}


static void add_line_info(LineInfo *info, int old_number, int new_number, char kind) {
	g_array_append_val(info->old_numbers, old_number);
	g_array_append_val(info->new_numbers, new_number);
	g_array_append_val(info->kinds, kind);
}


static GtkSourceStyleSchemeManager *get_scheme_manager(void) {
	GtkSourceStyleSchemeManager *manager = gtk_source_style_scheme_manager_get_default();
	static gboolean has_added_schemes;
	if (!has_added_schemes) {
		has_added_schemes = TRUE;
		g_autofree char *resource = get_resource_path(SCHEMES_RESOURCE);
		g_autofree char *path = g_strconcat("resource://", resource, NULL);
		gtk_source_style_scheme_manager_append_search_path(manager, path);
	}
	return manager;
}


static void use_style_scheme(GtkSourceBuffer *buffer) {
	gboolean is_dark = adw_style_manager_get_dark(adw_style_manager_get_default());
	GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(get_scheme_manager(),
		is_dark ? DARK_SCHEME : LIGHT_SCHEME);
	if (!scheme)
		g_warning("The code colours %s were not found", is_dark ? DARK_SCHEME : LIGHT_SCHEME);
	gtk_source_buffer_set_style_scheme(buffer, scheme);
}


static void follow_style(AdwStyleManager *manager, GParamSpec *spec, GtkSourceBuffer *buffer) {
	use_style_scheme(buffer);
}


static GtkSourceBuffer *create_buffer(const char *path) {
	GtkSourceBuffer *buffer = gtk_source_buffer_new(NULL);
	GtkSourceLanguage *language = gtk_source_language_manager_guess_language(
		gtk_source_language_manager_get_default(), path, NULL);
	gtk_source_buffer_set_language(buffer, language);
	gtk_source_buffer_set_highlight_matching_brackets(buffer, FALSE);
	use_style_scheme(buffer);
	g_signal_connect_object(adw_style_manager_get_default(), "notify::dark", G_CALLBACK(follow_style), buffer, 0);
	GtkTextTagTable *table = gtk_text_buffer_get_tag_table(GTK_TEXT_BUFFER(buffer));
	const struct {
		const char *name, *colour;
		double alpha;
		gboolean is_paragraph;
	} TAGS[] = {
		{ADDED_LINE_TAG, ADDED_COLOUR, LINE_TINT, TRUE}, {REMOVED_LINE_TAG, REMOVED_COLOUR, LINE_TINT, TRUE},
		{ADDED_WORD_TAG, ADDED_COLOUR, WORD_TINT, FALSE}, {REMOVED_WORD_TAG, REMOVED_COLOUR, WORD_TINT, FALSE},
	};
	for (guint index = 0; index < G_N_ELEMENTS(TAGS); index++) {
		GtkTextTag *tag = gtk_text_tag_new(TAGS[index].name);
		GdkRGBA colour = get_colour_with_alpha(TAGS[index].colour, TAGS[index].alpha);
		g_object_set(tag, TAGS[index].is_paragraph ? "paragraph-background-rgba" : "background-rgba", &colour, NULL);
		gtk_text_tag_table_add(table, tag);
		g_object_unref(tag);
	}
	GtkTextTag *note = gtk_text_tag_new(NOTE_TAG);
	GdkRGBA note_colour = get_colour_with_alpha(NOTE_COLOUR, NOTE_ALPHA);
	g_object_set(note, "style", PANGO_STYLE_ITALIC, "foreground-rgba", &note_colour, NULL);
	gtk_text_tag_table_add(table, note);
	g_object_unref(note);
	return buffer;
}


static void show_number(GtkSourceGutterRenderer *renderer, GObject *lines, guint line, gpointer data) {
	LineInfo *info = g_object_get_data(G_OBJECT(gtk_source_gutter_renderer_get_view(renderer)), LINE_INFO_KEY);
	GArray *numbers = data ? info->new_numbers : info->old_numbers;
	int number = line < numbers->len ? g_array_index(numbers, int, line) : 0;
	char text[G_ASCII_DTOSTR_BUF_SIZE] = "";
	if (number > 0)
		g_snprintf(text, sizeof(text), "%d", number);
	gtk_source_gutter_renderer_text_set_text(GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), text, -1);
}


static void show_sign(GtkSourceGutterRenderer *renderer, GObject *lines, guint line, gpointer data) {
	LineInfo *info = g_object_get_data(G_OBJECT(gtk_source_gutter_renderer_get_view(renderer)), LINE_INFO_KEY);
	char kind = line < info->kinds->len ? g_array_index(info->kinds, char, line) : ' ';
	const char *markup = kind == DIFF_LINE_ADDED ? "<span foreground=\"" ADDED_COLOUR "\">" ADDED_SIGN "</span>" :
		kind == DIFF_LINE_REMOVED ? "<span foreground=\"" REMOVED_COLOUR "\">" REMOVED_SIGN "</span>" : "";
	gtk_source_gutter_renderer_text_set_markup(GTK_SOURCE_GUTTER_RENDERER_TEXT(renderer), markup, -1);
}


/* A gutter column that tints an added or removed line the same as the code beside it, so a line reads as one row */
G_DECLARE_FINAL_TYPE(TintedGutter, tinted_gutter, CRIMSON, TINTED_GUTTER, GtkSourceGutterRendererText)

struct _TintedGutter {
	GtkSourceGutterRendererText parent;
};

G_DEFINE_FINAL_TYPE(TintedGutter, tinted_gutter, GTK_SOURCE_TYPE_GUTTER_RENDERER_TEXT)


static void tint_line(GtkSourceGutterRenderer *renderer, GtkSnapshot *snapshot, GtkSourceGutterLines *lines,
	guint line) {
	LineInfo *info = g_object_get_data(G_OBJECT(gtk_source_gutter_renderer_get_view(renderer)), LINE_INFO_KEY);
	char kind = line < info->kinds->len ? g_array_index(info->kinds, char, line) : ' ';
	if (kind == DIFF_LINE_ADDED || kind == DIFF_LINE_REMOVED) {
		int y = 0, height = 0;
		/* The newer get_line_extent needs GtkSourceView 5.16, and the app still runs on 5.12 */
		G_GNUC_BEGIN_IGNORE_DEPRECATIONS
		gtk_source_gutter_lines_get_line_yrange(lines, line, GTK_SOURCE_GUTTER_RENDERER_ALIGNMENT_MODE_CELL, &y,
			&height);
		G_GNUC_END_IGNORE_DEPRECATIONS
		GdkRGBA colour = get_colour_with_alpha(kind == DIFF_LINE_ADDED ? ADDED_COLOUR : REMOVED_COLOUR, LINE_TINT);
		gtk_snapshot_append_color(snapshot, &colour, &GRAPHENE_RECT_INIT(0, y,
			gtk_widget_get_width(GTK_WIDGET(renderer)), height));
	}
	GTK_SOURCE_GUTTER_RENDERER_CLASS(tinted_gutter_parent_class)->snapshot_line(renderer, snapshot, lines, line);
}


static void tinted_gutter_class_init(TintedGutterClass *class) {
	GTK_SOURCE_GUTTER_RENDERER_CLASS(class)->snapshot_line = tint_line;
}


static void tinted_gutter_init(TintedGutter *gutter) {
}


/* The stylesheet gives each column its width in the code's own font size, which follows the settings */
static GtkSourceGutterRenderer *create_renderer(const char *css_class, int padding, float xalign, GCallback query,
	gpointer data) {
	GtkSourceGutterRenderer *renderer = g_object_new(tinted_gutter_get_type(), NULL);
	gtk_source_gutter_renderer_set_xalign(renderer, xalign);
	gtk_source_gutter_renderer_set_xpad(renderer, padding);
	gtk_widget_add_css_class(GTK_WIDGET(renderer), css_class);
	g_signal_connect(renderer, "query-data", query, data);
	return renderer;
}


void apply_code_view_settings(GtkWidget *view) {
	gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(view), get_settings()->wraps_lines ? GTK_WRAP_WORD_CHAR :
		GTK_WRAP_NONE);
}


static GtkWidget *create_view(GtkSourceBuffer *buffer, LineInfo *info, gboolean shows_signs) {
	GtkWidget *view = gtk_source_view_new_with_buffer(buffer);
	gtk_text_view_set_editable(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(view), FALSE);
	gtk_text_view_set_monospace(GTK_TEXT_VIEW(view), TRUE);
	gtk_source_view_set_tab_width(GTK_SOURCE_VIEW(view), TAB_WIDTH);
	/* The code starts right at the gutter, so a changed line's tint runs unbroken from its numbers into its code,
	 * and a wrapped line's missing number tells it apart from a line of its own */
	gtk_text_view_set_left_margin(GTK_TEXT_VIEW(view), 0);
	/* The first and last lines keep clear of the card's edges */
	gtk_text_view_set_top_margin(GTK_TEXT_VIEW(view), CODE_EDGE_GAP);
	gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(view), CODE_EDGE_GAP);
	gtk_widget_add_css_class(view, "code");
	gtk_widget_add_css_class(view, "diff-view");
	g_object_set_data_full(G_OBJECT(view), LINE_INFO_KEY, info, free_line_info);
	GtkSourceGutter *gutter = gtk_source_view_get_gutter(GTK_SOURCE_VIEW(view), GTK_TEXT_WINDOW_LEFT);
	int position = 0;
	gtk_source_gutter_insert(gutter, create_renderer(NUMBER_CLASS, NUMBER_PADDING, NUMBER_ALIGN,
		G_CALLBACK(show_number), NULL), position++);
	if (shows_signs) {
		gtk_source_gutter_insert(gutter, create_renderer(NUMBER_CLASS, NUMBER_PADDING, NUMBER_ALIGN,
			G_CALLBACK(show_number), GINT_TO_POINTER(TRUE)), position++);
		gtk_source_gutter_insert(gutter, create_renderer(SIGN_CLASS, SIGN_PADDING, SIGN_ALIGN, G_CALLBACK(show_sign),
			NULL), position++);
	}
	apply_code_view_settings(view);
	return view;
}


/* A line as it can show: other encodings than UTF-8 get replacement marks, and a lone carriage return, which a
 * text view would take for a line break, shows as a space of the same length */
static char *make_displayable(const char *text) {
	char *shown = g_utf8_make_valid(text, -1);
	g_strdelimit(shown, "\r", ' ');
	return shown;
}


static void tag_line(GtkTextBuffer *buffer, int line, const char *tag) {
	GtkTextIter start, end;
	gtk_text_buffer_get_iter_at_line(buffer, &start, line);
	end = start;
	gtk_text_iter_forward_to_line_end(&end);
	/* The line break is tinted too, so the tint reaches the right edge of an empty line */
	gtk_text_iter_forward_char(&end);
	gtk_text_buffer_apply_tag_by_name(buffer, tag, &start, &end);
}


static void tag_words(GtkTextBuffer *buffer, int line, GArray *ranges, const char *tag) {
	for (guint index = 0; index < ranges->len; index++) {
		ByteRange *range = &g_array_index(ranges, ByteRange, index);
		GtkTextIter start, end;
		gtk_text_buffer_get_iter_at_line_index(buffer, &start, line, range->start);
		gtk_text_buffer_get_iter_at_line_index(buffer, &end, line, range->end);
		gtk_text_buffer_apply_tag_by_name(buffer, tag, &start, &end);
	}
}


/* Marks the words inside each replaced line, comparing it with the line that took its place */
static void mark_changed_words(GtkTextBuffer *buffer, const DiffHunk *hunk, GArray *buffer_lines) {
	g_autoptr(GArray) partners = pair_replaced_lines(hunk);
	g_autoptr(GArray) old_ranges = g_array_new(FALSE, FALSE, sizeof(ByteRange));
	g_autoptr(GArray) new_ranges = g_array_new(FALSE, FALSE, sizeof(ByteRange));
	for (guint index = 0; index < hunk->lines->len; index++) {
		int partner = g_array_index(partners, int, index);
		DiffLine *line = g_ptr_array_index(hunk->lines, index);
		if (partner < 0 || line->kind != DIFF_LINE_REMOVED)
			continue;
		DiffLine *added = g_ptr_array_index(hunk->lines, partner);
		/* Word positions are counted in the file's bytes, which only match the shown text in UTF-8 */
		if (!g_utf8_validate(line->text, -1, NULL) || !g_utf8_validate(added->text, -1, NULL))
			continue;
		g_array_set_size(old_ranges, 0);
		g_array_set_size(new_ranges, 0);
		if (!find_changed_words(line->text, added->text, old_ranges, new_ranges))
			continue;
		tag_words(buffer, g_array_index(buffer_lines, int, index), old_ranges, REMOVED_WORD_TAG);
		tag_words(buffer, g_array_index(buffer_lines, int, partner), new_ranges, ADDED_WORD_TAG);
	}
}


static void fit_to_text(GtkAdjustment *height, GtkScrolledWindow *scroller) {
	int text_height = (int)gtk_adjustment_get_upper(height);
	if (text_height > gtk_scrolled_window_get_min_content_height(scroller))
		gtk_scrolled_window_set_min_content_height(scroller, text_height);
}


GtkWidget *create_hunk_view(const DiffFile *file, const DiffHunk *hunk) {
	GtkSourceBuffer *buffer = create_buffer(file->path);
	LineInfo *info = create_line_info();
	g_autoptr(GString) text = g_string_new(NULL);
	/* Where each hunk line landed in the buffer, since the note about a missing line break is its own line */
	g_autoptr(GArray) buffer_lines = g_array_new(FALSE, FALSE, sizeof(int));
	for (guint index = 0; index < hunk->lines->len; index++) {
		DiffLine *line = g_ptr_array_index(hunk->lines, index);
		int buffer_line = (int)info->kinds->len;
		g_array_append_val(buffer_lines, buffer_line);
		/* Each line after the first starts on a new line, even when the lines before it were empty */
		if (index > 0)
			g_string_append_c(text, '\n');
		g_autofree char *shown = make_displayable(line->kind == DIFF_LINE_NO_NEWLINE ? TEXT_NO_LINE_BREAK : line->text);
		g_string_append(text, shown);
		add_line_info(info, line->old_number, line->new_number, (char)line->kind);
	}
	GtkTextBuffer *text_buffer = GTK_TEXT_BUFFER(buffer);
	gtk_text_buffer_set_text(text_buffer, text->str, (int)text->len);
	for (guint index = 0; index < hunk->lines->len; index++) {
		DiffLine *line = g_ptr_array_index(hunk->lines, index);
		int buffer_line = g_array_index(buffer_lines, int, index);
		if (line->kind == DIFF_LINE_ADDED)
			tag_line(text_buffer, buffer_line, ADDED_LINE_TAG);
		else if (line->kind == DIFF_LINE_REMOVED)
			tag_line(text_buffer, buffer_line, REMOVED_LINE_TAG);
		else if (line->kind == DIFF_LINE_NO_NEWLINE)
			tag_line(text_buffer, buffer_line, NOTE_TAG);
	}
	mark_changed_words(text_buffer, hunk, buffer_lines);
	GtkWidget *view = create_view(buffer, info, TRUE);
	g_object_unref(buffer);
	/* A text view guesses its height before it knows its width, so wrapped lines can leave the last line out of
	 * sight. The scroller grows to the height the laid out text really has, and long lines scroll sideways when not
	 * wrapped */
	GtkWidget *scroller = gtk_scrolled_window_new();
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), get_settings()->wraps_lines ? GTK_POLICY_NEVER :
		GTK_POLICY_AUTOMATIC, GTK_POLICY_EXTERNAL);
	gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), view);
	GtkAdjustment *height = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroller));
	g_signal_connect_object(height, "changed", G_CALLBACK(fit_to_text), scroller, 0);
	return scroller;
}


GtkWidget *create_code_view(const char *path, const char *text, int first_line) {
	GtkSourceBuffer *buffer = create_buffer(path);
	g_autofree char *shown = make_displayable(text);
	text = shown;
	gsize length = strlen(text);
	/* The last line break ends the last line rather than starting an empty one */
	if (length > 0 && text[length - 1] == '\n')
		length--;
	gtk_text_buffer_set_text(GTK_TEXT_BUFFER(buffer), text, (int)length);
	LineInfo *info = create_line_info();
	int count = gtk_text_buffer_get_line_count(GTK_TEXT_BUFFER(buffer));
	for (int line = 0; line < count; line++)
		add_line_info(info, first_line + line, 0, DIFF_LINE_CONTEXT);
	GtkWidget *view = create_view(buffer, info, FALSE);
	g_object_unref(buffer);
	return view;
}
