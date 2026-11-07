#include "lang/en.h"
#include "git/log.h"
#include "view/chips.h"
#include "common/widgets/spacing.h"

/* More chips than this crowd out the message, and the rest are counted in one more chip */
#define MOST_CHIPS 3
#define CHIP_WIDTH_CHARACTERS 18

static const char *const KIND_CLASSES[] = {"branch", "remote", "tag", "detached"};


GtkWidget *create_chip(const char *text, const char *kind_class) {
	GtkWidget *chip = gtk_label_new(text);
	gtk_label_set_ellipsize(GTK_LABEL(chip), PANGO_ELLIPSIZE_MIDDLE);
	gtk_label_set_max_width_chars(GTK_LABEL(chip), CHIP_WIDTH_CHARACTERS);
	gtk_widget_add_css_class(chip, "chip");
	if (kind_class)
		gtk_widget_add_css_class(chip, kind_class);
	gtk_widget_set_valign(chip, GTK_ALIGN_CENTER);
	return chip;
}


void clear_reference_chips(GtkWidget *box) {
	GtkWidget *child;
	while ((child = gtk_widget_get_first_child(box)))
		gtk_box_remove(GTK_BOX(box), child);
	gtk_widget_set_visible(box, FALSE);
}


void fill_reference_chips(GtkWidget *box, GPtrArray *references) {
	clear_reference_chips(box);
	guint shown = 0;
	g_autoptr(GString) hidden = g_string_new(NULL);
	for (guint index = 0; references && index < references->len; index++) {
		CommitReference *reference = g_ptr_array_index(references, index);
		if (reference->kind == REFERENCE_DETACHED)
			continue;
		if (shown == MOST_CHIPS) {
			g_string_append_printf(hidden, "%s%s", hidden->len ? "\n" : "", reference->name);
			continue;
		}
		GtkWidget *chip = create_chip(reference->name, KIND_CLASSES[reference->kind]);
		if (reference->is_current)
			gtk_widget_add_css_class(chip, "current");
		gtk_widget_set_tooltip_text(chip, reference->name);
		gtk_box_append(GTK_BOX(box), chip);
		shown++;
	}
	if (hidden->len > 0) {
		guint count = 1;
		for (const char *letter = hidden->str; *letter; letter++)
			count += *letter == '\n';
		g_autofree char *text = g_strdup_printf(TEXT_MORE_CHIPS, count);
		GtkWidget *more = create_chip(text, NULL);
		gtk_widget_set_tooltip_text(more, hidden->str);
		gtk_box_append(GTK_BOX(box), more);
	}
	gtk_widget_set_visible(box, shown > 0);
}


static GtkWidget *add_count_label(GtkWidget *box, const char *css_class) {
	GtkWidget *label = gtk_label_new(NULL);
	gtk_widget_add_css_class(label, css_class);
	gtk_widget_add_css_class(label, "numeric");
	gtk_box_append(GTK_BOX(box), label);
	return label;
}


GtkWidget *create_change_counts(void) {
	GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, SPACING_SMALL);
	GtkWidget *text = add_count_label(box, "count-text");
	gtk_label_set_xalign(GTK_LABEL(text), 0);
	gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
	add_count_label(box, "added-count");
	add_count_label(box, "removed-count");
	return box;
}


static void show_count(GtkWidget *label, const char *format, int count) {
	g_autofree char *text = g_strdup_printf(format, count);
	gtk_label_set_text(GTK_LABEL(label), text);
	gtk_widget_set_visible(label, count > 0);
}


void set_change_counts(GtkWidget *counts, const char *text, int added, int removed) {
	GtkWidget *label = gtk_widget_get_first_child(counts);
	gtk_label_set_text(GTK_LABEL(label), text ? text : "");
	gtk_widget_set_visible(label, text && *text);
	label = gtk_widget_get_next_sibling(label);
	show_count(label, TEXT_ADDED_COUNT, added);
	show_count(gtk_widget_get_next_sibling(label), TEXT_REMOVED_COUNT, removed);
}
