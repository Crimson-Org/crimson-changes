#pragma once

#include "window/window.h"

/* Asks only what a new repository needs: who commits, the first branch's name, what to leave out, and whether to
 * commit everything right away. Then makes the repository and opens it */
void show_setup_dialog(Window *window, const char *folder);
