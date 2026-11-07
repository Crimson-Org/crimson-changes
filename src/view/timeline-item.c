#include "lang/en.h"
#include "view/timeline-item.h"

G_DEFINE_FINAL_TYPE(TimelineItem, timeline_item, G_TYPE_OBJECT)


static void finalize_item(GObject *object) {
	TimelineItem *item = CHANGES_TIMELINE_ITEM(object);
	free_commit(item->commit);
	g_free(item->stash_name);
	g_free(item->heading);
	free_graph_row(item->graph);
	G_OBJECT_CLASS(timeline_item_parent_class)->finalize(object);
}


static void timeline_item_class_init(TimelineItemClass *class) {
	G_OBJECT_CLASS(class)->finalize = finalize_item;
}


static void timeline_item_init(TimelineItem *item) {
}


static TimelineItem *create_item(TimelineKind kind) {
	TimelineItem *item = g_object_new(TYPE_TIMELINE_ITEM, NULL);
	item->kind = kind;
	return item;
}


TimelineItem *create_now_item(void) {
	return create_item(TIMELINE_NOW);
}


/* A stash is a commit whose first parent is where it was made, so it shows like one */
TimelineItem *create_stash_item(const Stash *stash) {
	TimelineItem *item = create_item(TIMELINE_STASH);
	item->stash_name = g_strdup(stash->name);
	item->commit = g_new0(Commit, 1);
	item->commit->hash = g_strdup(stash->hash);
	item->commit->parents = g_new0(char *, 1);
	/* A stash put aside without a name is named for what it is, never for the commit it was made on */
	item->commit->subject = stash->message ? g_strdup(stash->message) : stash->branch ?
		g_strdup_printf(TEXT_UNNAMED_STASH_ON, stash->branch) : g_strdup(TEXT_UNNAMED_STASH);
	/* The body holds what git wrote, so a dropped stash can be stored back as it was */
	item->commit->body = g_strdup(stash->subject);
	item->commit->author_time = item->commit->commit_time = stash->time;
	item->commit->references = g_ptr_array_new();
	return item;
}


TimelineItem *create_heading_item(char *heading, GraphRow *graph) {
	TimelineItem *item = create_item(TIMELINE_HEADING);
	item->heading = heading;
	item->graph = graph;
	return item;
}


TimelineItem *create_commit_item(Commit *commit, GraphRow *graph) {
	TimelineItem *item = create_item(TIMELINE_COMMIT);
	item->commit = commit;
	item->graph = graph;
	return item;
}
