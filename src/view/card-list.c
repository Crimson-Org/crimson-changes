#include "lang/en.h"
#include "common/widgets/spacing.h"
#include "view/layout.h"
#include "common/widgets/smooth-scroll.h"
#include "view/card-list.h"

/* Past this many files the rest start folded, so a commit that touched hundreds of files still opens at once */
#define MOST_OPEN_CARDS 30
/* Past this many files the rest are only counted, so a commit of thousands of files still opens */
#define MOST_CARDS 400
#define KEY_KEY "card-key"


static char *make_key(const DiffFile *file) {
	return g_strdup_printf("%c%s", (char)file->kind, file->path);
}

/* A file chosen in the list lands this far below the top of the view */
#define LANDING_GAP 12
#define GLIDE_MILLISECONDS 360

struct CardList {
	/* The list of changed files above the cards, which jumps to a card, and the cards themselves */
	GtkWidget *overview, *box, *more;
	guint fewest_overview_files;
	/* Each card's revealer by the card's key */
	GHashTable *revealers;
};


/* Choosing a file in the list glides the view to its card and opens the card */
static void jump_to_card(GtkListBox *overview, GtkListBoxRow *row, CardList *list) {
	const char *key = g_object_get_data(G_OBJECT(row), KEY_KEY);
	GtkWidget *revealer = key ? g_hash_table_lookup(list->revealers, key) : NULL;
	GtkWidget *scroller = gtk_widget_get_ancestor(list->box, GTK_TYPE_SCROLLED_WINDOW);
	if (!revealer || !scroller)
		return;
	set_file_card_expanded(gtk_revealer_get_child(GTK_REVEALER(revealer)), TRUE);
	graphene_point_t point = GRAPHENE_POINT_INIT(0, 0), target;
	if (gtk_widget_compute_point(revealer, gtk_scrolled_window_get_child(GTK_SCROLLED_WINDOW(scroller)), &point,
		&target))
		glide_scroller(GTK_SCROLLED_WINDOW(scroller), MAX(target.y - LANDING_GAP, 0), GLIDE_MILLISECONDS);
}


static void fill_overview(CardList *list, GPtrArray *shown, guint count) {
	gtk_list_box_remove_all(GTK_LIST_BOX(list->overview));
	for (guint index = 0; index < count; index++) {
		const DiffFile *file = g_ptr_array_index(shown, index);
		GtkWidget *row = gtk_list_box_row_new();
		gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), create_file_overview_row(file));
		g_object_set_data_full(G_OBJECT(row), KEY_KEY, make_key(file), g_free);
		gtk_list_box_append(GTK_LIST_BOX(list->overview), row);
	}
	gtk_widget_set_visible(list->overview, count >= list->fewest_overview_files);
}


CardList *create_card_list(guint fewest_overview_files) {
	CardList *list = g_new0(CardList, 1);
	list->fewest_overview_files = fewest_overview_files;
	list->overview = gtk_list_box_new();
	gtk_list_box_set_selection_mode(GTK_LIST_BOX(list->overview), GTK_SELECTION_NONE);
	gtk_widget_add_css_class(list->overview, "file-overview");
	gtk_widget_set_visible(list->overview, FALSE);
	g_signal_connect(list->overview, "row-activated", G_CALLBACK(jump_to_card), list);
	list->box = gtk_box_new(GTK_ORIENTATION_VERTICAL, SPACING_NORMAL);
	gtk_widget_add_css_class(list->box, "card-list");
	list->revealers = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	/* The list holds the label itself, since the label leaves the column and comes back below the last card */
	list->more = g_object_ref_sink(gtk_label_new(NULL));
	gtk_widget_add_css_class(list->more, "card-note");
	gtk_widget_set_visible(list->more, FALSE);
	return list;
}


GtkWidget *get_card_list_overview(CardList *list) {
	return list->overview;
}


GtkWidget *get_card_list_widget(CardList *list) {
	return list->box;
}




/* A card that slid out leaves the column once it is out of sight */
static void remove_hidden(GtkRevealer *revealer, GParamSpec *spec, CardList *list) {
	if (gtk_revealer_get_child_revealed(revealer) || gtk_revealer_get_reveal_child(revealer))
		return;
	GtkWidget *parent = gtk_widget_get_parent(GTK_WIDGET(revealer));
	if (parent)
		gtk_box_remove(GTK_BOX(parent), GTK_WIDGET(revealer));
}


static GtkWidget *create_revealer(CardList *list, GtkWidget *card, const char *key, gboolean animates) {
	GtkWidget *revealer = gtk_revealer_new();
	gtk_revealer_set_transition_type(GTK_REVEALER(revealer), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
	gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), animates ? SLIDE_MILLISECONDS : 0);
	gtk_revealer_set_child(GTK_REVEALER(revealer), card);
	g_object_set_data_full(G_OBJECT(revealer), KEY_KEY, g_strdup(key), g_free);
	g_signal_connect(revealer, "notify::child-revealed", G_CALLBACK(remove_hidden), list);
	return revealer;
}


/* Without animation the card leaves at once, and otherwise it slides away and leaves once hidden */
static void hide_card(CardList *list, GtkWidget *revealer, gboolean animates) {
	if (!animates) {
		gtk_box_remove(GTK_BOX(list->box), revealer);
		return;
	}
	gtk_revealer_set_transition_duration(GTK_REVEALER(revealer), SLIDE_MILLISECONDS);
	gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), FALSE);
}


void update_card_list(CardList *list, Window *window, const CardSource *source, GPtrArray *owner, GPtrArray *shown,
	gboolean animates) {
	g_autoptr(GHashTable) kept = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	GtkWidget *previous = NULL;
	guint count = MIN(shown->len, MOST_CARDS);
	for (guint index = 0; index < count; index++) {
		const DiffFile *file = g_ptr_array_index(shown, index);
		g_autofree char *key = make_key(file);
		GtkWidget *revealer = animates ? g_hash_table_lookup(list->revealers, key) : NULL;
		guint hash = hash_diff_file(file) + (guint)source->side;
		if (revealer) {
			GtkWidget *card = gtk_revealer_get_child(GTK_REVEALER(revealer));
			/* A card whose diff changed is made again in place, keeping whether it was open */
			if (get_file_card_hash(card) != hash) {
				gboolean was_open = gtk_widget_has_css_class(card, "expanded");
				gtk_revealer_set_child(GTK_REVEALER(revealer), create_file_card(window, source, owner, file, was_open));
			}
			gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
		} else {
			GtkWidget *card = create_file_card(window, source, owner, file, index < MOST_OPEN_CARDS);
			revealer = create_revealer(list, card, key, animates);
			gtk_box_append(GTK_BOX(list->box), revealer);
			gtk_revealer_set_reveal_child(GTK_REVEALER(revealer), TRUE);
		}
		gtk_box_reorder_child_after(GTK_BOX(list->box), revealer, previous);
		previous = revealer;
		g_hash_table_insert(kept, g_strdup(key), revealer);
	}
	GHashTableIter iterator;
	gpointer key, revealer;
	g_hash_table_iter_init(&iterator, list->revealers);
	while (g_hash_table_iter_next(&iterator, &key, &revealer))
		if (g_hash_table_lookup(kept, key) != revealer)
			hide_card(list, revealer, animates);
	g_hash_table_remove_all(list->revealers);
	fill_overview(list, shown, count);
	/* The count of files left out stays below the last card */
	if (gtk_widget_get_parent(list->more))
		gtk_box_remove(GTK_BOX(list->box), list->more);
	g_autofree char *more = g_strdup_printf(TEXT_MORE_FILES, shown->len - count);
	gtk_label_set_text(GTK_LABEL(list->more), more);
	gtk_widget_set_visible(list->more, shown->len > count);
	gtk_box_append(GTK_BOX(list->box), list->more);
	for (GtkWidget *child = gtk_widget_get_first_child(list->box); child; child = gtk_widget_get_next_sibling(child))
		if (GTK_IS_REVEALER(child) && gtk_revealer_get_reveal_child(GTK_REVEALER(child)))
			g_hash_table_insert(list->revealers, g_strdup(g_object_get_data(G_OBJECT(child), KEY_KEY)), child);
}


void refresh_card_list(CardList *list) {
	for (GtkWidget *child = gtk_widget_get_first_child(list->box); child; child = gtk_widget_get_next_sibling(child))
		if (GTK_IS_REVEALER(child))
			refresh_file_card(gtk_revealer_get_child(GTK_REVEALER(child)));
}


void free_card_list(CardList *list) {
	g_hash_table_unref(list->revealers);
	g_object_unref(list->more);
	g_free(list);
}
