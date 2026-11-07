#pragma once

#include "window/window.h"

/* Who last changed each line of the file, as of the commit, or of the work folder when revision is NULL */
void show_blame_page(Window *window, const char *path, const char *revision);
