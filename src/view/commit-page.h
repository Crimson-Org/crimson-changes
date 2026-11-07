#pragma once

#include <gtk/gtk.h>

#include "git/log.h"
#include "window/window.h"

/* One commit: its full message, who made it and when, and every file it changed */
typedef struct CommitPage CommitPage;

CommitPage *create_commit_page(Window *window);
GtkWidget *get_commit_page_widget(CommitPage *page);
void glide_commit_page_to_top(CommitPage *page);
/* Shows the commit, or the stash when a stash name is given, reading its details and diff in the background */
void show_commit_page(CommitPage *page, const Commit *commit, const char *stash_name);
/* The commit shown, or NULL */
const char *get_commit_page_hash(CommitPage *page);
/* Reads the commit again, such as after the diff settings changed */
void reload_commit_page(CommitPage *page);
/* Places the menu of things to do with the commit beside its hash, where it belongs */
void set_commit_page_menu(CommitPage *page, GtkWidget *menu_button);
void free_commit_page(CommitPage *page);
