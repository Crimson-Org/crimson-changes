#pragma once

#include <adwaita.h>

#include "git/repository.h"
#include "git/log.h"

typedef struct Window Window;

typedef void (*UndoFunction)(gpointer data);

Window *create_window(GtkApplication *application);
/* Keeps the window and its parts alive until released, for an answer from git that may come after it closed */
Window *keep_window(Window *window);
void release_window(Window *window);
GtkWindow *get_gtk_window(Window *window);
GtkWidget *get_window_widget(Window *window);
/* The repository shown, or NULL on the start pages */
Repository *get_window_repository(Window *window);

/* Asks for a folder with the system's own dialog, then opens it */
void ask_for_folder(Window *window);
void open_folder(Window *window, const char *path);
gboolean has_window_folder(Window *window);

void show_toast(Window *window, const char *text);
/* A toast with an Undo button, which runs undo once if pressed, and frees data either way */
void show_undo_toast(Window *window, const char *text, UndoFunction undo, gpointer data, GDestroyNotify destroy);
/* Says in a toast that something failed with git's own reason, with the full output a click away */
void show_git_failure(Window *window, const char *what, const GitResult *result);

/* The uncommitted changes, or one commit, in the main pane */
void show_work_changes(Window *window);
void show_commit(Window *window, const Commit *commit);
/* Finds the commit in the timeline and shows it, loading more history when needed */
void go_to_commit(Window *window, const char *hash);
/* Slides a page in over the main pane, such as a file's blame, with a way back */
void push_window_page(Window *window, AdwNavigationPage *page);
/* The timeline lists every branch or only the one checked out */
void reload_timeline(Window *window);
