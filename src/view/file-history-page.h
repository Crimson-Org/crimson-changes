#pragma once

#include "window/window.h"

/* Every commit that changed the file, following it through renames */
void show_file_history_page(Window *window, const char *path);
/* Every commit that changed these lines of the file as of the revision, or the commit checked out when NULL, each
 * with how it changed them */
void show_line_history_page(Window *window, const char *path, const char *revision, int first_line, int last_line);
