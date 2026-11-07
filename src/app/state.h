#pragma once

#include "common/window/window-geometry.h"

#define SIDEBAR_WIDTH 380
#define SIDEBAR_MINIMUM 300
#define SIDEBAR_MAXIMUM 640

/* Window layout kept between launches */
typedef struct {
	WindowGeometry geometry;
	int sidebar_width;
} WindowState;

void load_window_state(WindowState *state);
void save_window_state(const WindowState *state);

/* Repositories opened before, newest first, which the caller must not free */
const char *const *get_recent_repositories(void);
void add_recent_repository(const char *folder);
void forget_recent_repository(const char *folder);
