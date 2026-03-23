// Copyright (c) 2026, mktile contributors
// SPDX-License-Identifier: MIT

#include "mkgui/mkgui.c"

#include <unistd.h>
#include <X11/Xatom.h>

#define MAX_WINDOWS 512
#define MAX_TITLE   256

// ---------------------------------------------------------------------------
// X11 atom IDs
// ---------------------------------------------------------------------------

enum atom_id {
	ATOM_NET_CLIENT_LIST,
	ATOM_NET_CURRENT_DESKTOP,
	ATOM_NET_WM_DESKTOP,
	ATOM_NET_WM_STATE,
	ATOM_NET_WM_STATE_HIDDEN,
	ATOM_NET_WM_STATE_STICKY,
	ATOM_NET_WM_STATE_MAXIMIZED_VERT,
	ATOM_NET_WM_STATE_MAXIMIZED_HORZ,
	ATOM_NET_WM_STATE_FULLSCREEN,
	ATOM_NET_FRAME_EXTENTS,
	ATOM_NET_WM_NAME,
	ATOM_WM_NAME,
	ATOM_NET_WORKAREA,
	ATOM_NET_ACTIVE_WINDOW,
	ATOM_NET_WM_PID,
	ATOM_UTF8_STRING,
	ATOM_COUNT
};

static const char *atom_names[ATOM_COUNT] = {
	[ATOM_NET_CLIENT_LIST]              = "_NET_CLIENT_LIST",
	[ATOM_NET_CURRENT_DESKTOP]          = "_NET_CURRENT_DESKTOP",
	[ATOM_NET_WM_DESKTOP]               = "_NET_WM_DESKTOP",
	[ATOM_NET_WM_STATE]                 = "_NET_WM_STATE",
	[ATOM_NET_WM_STATE_HIDDEN]          = "_NET_WM_STATE_HIDDEN",
	[ATOM_NET_WM_STATE_STICKY]          = "_NET_WM_STATE_STICKY",
	[ATOM_NET_WM_STATE_MAXIMIZED_VERT]  = "_NET_WM_STATE_MAXIMIZED_VERT",
	[ATOM_NET_WM_STATE_MAXIMIZED_HORZ]  = "_NET_WM_STATE_MAXIMIZED_HORZ",
	[ATOM_NET_WM_STATE_FULLSCREEN]      = "_NET_WM_STATE_FULLSCREEN",
	[ATOM_NET_FRAME_EXTENTS]            = "_NET_FRAME_EXTENTS",
	[ATOM_NET_WM_NAME]                  = "_NET_WM_NAME",
	[ATOM_WM_NAME]                      = "WM_NAME",
	[ATOM_NET_WORKAREA]                 = "_NET_WORKAREA",
	[ATOM_NET_ACTIVE_WINDOW]            = "_NET_ACTIVE_WINDOW",
	[ATOM_NET_WM_PID]                   = "_NET_WM_PID",
	[ATOM_UTF8_STRING]                  = "UTF8_STRING",
};

// ---------------------------------------------------------------------------
// Data structures
// ---------------------------------------------------------------------------

struct window_entry {
	Window   id;
	char     title[MAX_TITLE];
	uint32_t checked;
};

struct undo_entry {
	Window   id;
	int32_t  x, y;
	uint32_t w, h;
	uint32_t was_maximized;
};

struct work_area {
	int32_t  x, y;
	uint32_t w, h;
};

struct tile_state {
	Display      *dpy;
	Window        root;
	Atom          atoms[ATOM_COUNT];
	pid_t         self_pid;

	struct window_entry windows[MAX_WINDOWS];
	uint32_t            win_count;

	struct undo_entry   undo[MAX_WINDOWS];
	uint32_t            undo_count;

	struct work_area    area;
};

static struct tile_state ts;

// ---------------------------------------------------------------------------
// X11 helpers
// ---------------------------------------------------------------------------

// [=]===^=[ x11_get_property ]====================================[=]
static unsigned char *x11_get_property(Window win, enum atom_id aid, Atom req_type, unsigned long *nitems_out) {
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *data = NULL;

	int rc = XGetWindowProperty(ts.dpy, win, ts.atoms[aid], 0, 1024, False, req_type, &actual_type, &actual_format, &nitems, &bytes_after, &data);

	if(rc != Success || !data || nitems == 0) {
		if(data) {
			XFree(data);
		}
		if(nitems_out) {
			*nitems_out = 0;
		}
		return NULL;
	}

	if(nitems_out) {
		*nitems_out = nitems;
	}
	return data;
}

// [=]===^=[ x11_get_workarea ]====================================[=]
static void x11_get_workarea(void) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(ts.root, ATOM_NET_WORKAREA, XA_CARDINAL, &nitems);

	if(!data || nitems < 4) {
		ts.area.x = 0;
		ts.area.y = 0;
		ts.area.w = (uint32_t)DisplayWidth(ts.dpy, DefaultScreen(ts.dpy));
		ts.area.h = (uint32_t)DisplayHeight(ts.dpy, DefaultScreen(ts.dpy));
		if(data) {
			XFree(data);
		}
		return;
	}

	unsigned long *vals = (unsigned long *)data;
	ts.area.x = (int32_t)vals[0];
	ts.area.y = (int32_t)vals[1];
	ts.area.w = (uint32_t)vals[2];
	ts.area.h = (uint32_t)vals[3];
	XFree(data);
}

// [=]===^=[ x11_get_current_desktop ]=============================[=]
static unsigned long x11_get_current_desktop(void) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(ts.root, ATOM_NET_CURRENT_DESKTOP, XA_CARDINAL, &nitems);

	if(!data) {
		return 0;
	}

	unsigned long desk = ((unsigned long *)data)[0];
	XFree(data);
	return desk;
}

// [=]===^=[ x11_get_window_desktop ]==============================[=]
static unsigned long x11_get_window_desktop(Window win) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_WM_DESKTOP, XA_CARDINAL, &nitems);

	if(!data) {
		return (unsigned long)-1;
	}

	unsigned long desk = ((unsigned long *)data)[0];
	XFree(data);
	return desk;
}

// [=]===^=[ x11_get_window_title ]================================[=]
static void x11_get_window_title(Window win, char *buf, uint32_t buflen) {
	buf[0] = '\0';

	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_WM_NAME, ts.atoms[ATOM_UTF8_STRING], &nitems);

	if(!data) {
		data = x11_get_property(win, ATOM_WM_NAME, XA_STRING, &nitems);
	}

	if(data) {
		snprintf(buf, buflen, "%s", (char *)data);
		XFree(data);
	}
}

// [=]===^=[ x11_get_frame_extents ]===============================[=]
static void x11_get_frame_extents(Window win, uint32_t *left, uint32_t *right, uint32_t *top, uint32_t *bottom) {
	*left = *right = *top = *bottom = 0;

	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_FRAME_EXTENTS, XA_CARDINAL, &nitems);

	if(!data || nitems < 4) {
		if(data) {
			XFree(data);
		}
		return;
	}

	unsigned long *vals = (unsigned long *)data;
	*left   = (uint32_t)vals[0];
	*right  = (uint32_t)vals[1];
	*top    = (uint32_t)vals[2];
	*bottom = (uint32_t)vals[3];
	XFree(data);
}

// [=]===^=[ x11_has_wm_state ]====================================[=]
static uint32_t x11_has_wm_state(Window win, enum atom_id state_atom) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_WM_STATE, XA_ATOM, &nitems);

	if(!data) {
		return 0;
	}

	Atom *atoms = (Atom *)data;
	Atom target = ts.atoms[state_atom];
	uint32_t found = 0;
	for(unsigned long i = 0; i < nitems; ++i) {
		if(atoms[i] == target) {
			found = 1;
			break;
		}
	}

	XFree(data);
	return found;
}

// [=]===^=[ x11_get_window_pid ]==================================[=]
static pid_t x11_get_window_pid(Window win) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_WM_PID, XA_CARDINAL, &nitems);

	if(!data) {
		return 0;
	}

	pid_t pid = (pid_t)((unsigned long *)data)[0];
	XFree(data);
	return pid;
}

// [=]===^=[ x11_get_window_geom ]=================================[=]
static void x11_get_window_geom(Window win, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
	Window root_ret;
	int xr, yr;
	unsigned int wr, hr, bw, depth;

	XGetGeometry(ts.dpy, win, &root_ret, &xr, &yr, &wr, &hr, &bw, &depth);

	Window child;
	XTranslateCoordinates(ts.dpy, win, ts.root, 0, 0, &xr, &yr, &child);

	uint32_t fl, fr, ft, fb;
	x11_get_frame_extents(win, &fl, &fr, &ft, &fb);

	*x = xr - (int32_t)fl;
	*y = yr - (int32_t)ft;
	*w = wr + fl + fr;
	*h = hr + ft + fb;
}

// [=]===^=[ x11_send_client_msg ]=================================[=]
static void x11_send_client_msg(Window win, enum atom_id msg, long d0, long d1, long d2, long d3, long d4) {
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type         = ClientMessage;
	ev.xclient.serial       = 0;
	ev.xclient.send_event   = True;
	ev.xclient.message_type = ts.atoms[msg];
	ev.xclient.window       = win;
	ev.xclient.format       = 32;
	ev.xclient.data.l[0]    = d0;
	ev.xclient.data.l[1]    = d1;
	ev.xclient.data.l[2]    = d2;
	ev.xclient.data.l[3]    = d3;
	ev.xclient.data.l[4]    = d4;

	XSendEvent(ts.dpy, ts.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

// [=]===^=[ x11_moveresize ]======================================[=]
static void x11_moveresize(Window win, int32_t x, int32_t y, uint32_t w, uint32_t h) {
	XSync(ts.dpy, False);

	x11_send_client_msg(win, ATOM_NET_WM_STATE, 0, (long)ts.atoms[ATOM_NET_WM_STATE_FULLSCREEN], 0, 0, 0);
	x11_send_client_msg(win, ATOM_NET_WM_STATE, 0, (long)ts.atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT], (long)ts.atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ], 0, 0);
	x11_send_client_msg(win, ATOM_NET_ACTIVE_WINDOW, 0, 0, 0, 0, 0);

	XMapRaised(ts.dpy, win);
	XSync(ts.dpy, False);

	uint32_t fl, fr, ft, fb;
	x11_get_frame_extents(win, &fl, &fr, &ft, &fb);

	uint32_t cw = (w > fl + fr) ? w - fl - fr : 1;
	uint32_t ch = (h > ft + fb) ? h - ft - fb : 1;

	XMoveResizeWindow(ts.dpy, win, x + (int32_t)fl, y + (int32_t)ft, cw, ch);
	XSync(ts.dpy, False);
}

// [=]===^=[ x11_init ]============================================[=]
static uint32_t x11_init(void) {
	ts.dpy = XOpenDisplay(NULL);
	if(!ts.dpy) {
		fprintf(stderr, "mktile: cannot open X display\n");
		return 0;
	}

	ts.root = DefaultRootWindow(ts.dpy);
	ts.self_pid = getpid();

	for(uint32_t i = 0; i < ATOM_COUNT; ++i) {
		ts.atoms[i] = XInternAtom(ts.dpy, atom_names[i], False);
	}

	x11_get_workarea();
	return 1;
}

// ---------------------------------------------------------------------------
// Window enumeration
// ---------------------------------------------------------------------------

// [=]===^=[ enumerate_windows ]===================================[=]
static void enumerate_windows(void) {
	ts.win_count = 0;

	unsigned long nitems;
	unsigned char *data = x11_get_property(ts.root, ATOM_NET_CLIENT_LIST, XA_WINDOW, &nitems);

	if(!data) {
		return;
	}

	unsigned long cur_desk = x11_get_current_desktop();
	Window *clients = (Window *)data;

	for(unsigned long i = 0; i < nitems && ts.win_count < MAX_WINDOWS; ++i) {
		Window w = clients[i];

		if(x11_get_window_pid(w) == ts.self_pid) {
			continue;
		}

		if(x11_has_wm_state(w, ATOM_NET_WM_STATE_HIDDEN)) {
			continue;
		}

		if(x11_has_wm_state(w, ATOM_NET_WM_STATE_STICKY)) {
			continue;
		}

		unsigned long wdesk = x11_get_window_desktop(w);
		if(wdesk != cur_desk && wdesk != (unsigned long)-1) {
			continue;
		}

		struct window_entry *ent = &ts.windows[ts.win_count];
		ent->id = w;
		ent->checked = 0;
		x11_get_window_title(w, ent->title, MAX_TITLE);

		if(ent->title[0] == '\0') {
			snprintf(ent->title, MAX_TITLE, "(untitled)");
		}

		++ts.win_count;
	}

	XFree(data);
}

// ---------------------------------------------------------------------------
// Tiling
// ---------------------------------------------------------------------------

// [=]===^=[ tile_vertical ]=======================================[=]
static void tile_vertical(Window *wins, uint32_t count) {
	if(count == 0) {
		return;
	}

	int32_t ax = ts.area.x;
	int32_t ay = ts.area.y;
	uint32_t aw = ts.area.w;
	uint32_t ah = ts.area.h;
	uint32_t step = ah / count;

	for(uint32_t i = 0; i < count; ++i) {
		uint32_t h = (i == count - 1) ? (ah - step * i) : step;
		x11_moveresize(wins[i], ax, ay + (int32_t)(step * i), aw, h);
	}
}

// [=]===^=[ tile_horizontal ]=====================================[=]
static void tile_horizontal(Window *wins, uint32_t count) {
	if(count == 0) {
		return;
	}

	int32_t ax = ts.area.x;
	int32_t ay = ts.area.y;
	uint32_t aw = ts.area.w;
	uint32_t ah = ts.area.h;
	uint32_t step = aw / count;

	for(uint32_t i = 0; i < count; ++i) {
		uint32_t w = (i == count - 1) ? (aw - step * i) : step;
		x11_moveresize(wins[i], ax + (int32_t)(step * i), ay, w, ah);
	}
}

// [=]===^=[ tile_grid ]===========================================[=]
static void tile_grid(Window *wins, uint32_t count, uint32_t rows, uint32_t cols) {
	if(count == 0 || rows == 0 || cols == 0) {
		return;
	}

	uint32_t max_slots = rows * cols;
	if(count > max_slots) {
		count = max_slots;
	}

	int32_t ax = ts.area.x;
	int32_t ay = ts.area.y;
	uint32_t aw = ts.area.w;
	uint32_t ah = ts.area.h;
	uint32_t cw = aw / cols;
	uint32_t ch = ah / rows;

	for(uint32_t i = 0; i < count; ++i) {
		uint32_t col = i % cols;
		uint32_t row = i / cols;
		uint32_t w = (col == cols - 1) ? (aw - cw * col) : cw;
		uint32_t h = (row == rows - 1) ? (ah - ch * row) : ch;
		x11_moveresize(wins[i], ax + (int32_t)(cw * col), ay + (int32_t)(ch * row), w, h);
	}
}

// ---------------------------------------------------------------------------
// Undo
// ---------------------------------------------------------------------------

// [=]===^=[ undo_save ]===========================================[=]
static void undo_save(Window *wins, uint32_t count) {
	ts.undo_count = 0;

	for(uint32_t i = 0; i < count && i < MAX_WINDOWS; ++i) {
		struct undo_entry *u = &ts.undo[ts.undo_count];
		u->id = wins[i];
		x11_get_window_geom(wins[i], &u->x, &u->y, &u->w, &u->h);
		u->was_maximized =
			x11_has_wm_state(wins[i], ATOM_NET_WM_STATE_MAXIMIZED_VERT) ||
			x11_has_wm_state(wins[i], ATOM_NET_WM_STATE_MAXIMIZED_HORZ);
		++ts.undo_count;
	}
}

// [=]===^=[ undo_restore ]========================================[=]
static void undo_restore(void) {
	for(uint32_t i = 0; i < ts.undo_count; ++i) {
		struct undo_entry *u = &ts.undo[i];

		if(u->was_maximized) {
			x11_send_client_msg(u->id, ATOM_NET_WM_STATE, 1,
				(long)ts.atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT],
				(long)ts.atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ], 0, 0);
			XSync(ts.dpy, False);

		} else {
			x11_moveresize(u->id, u->x, u->y, u->w, u->h);
		}
	}
}

// ---------------------------------------------------------------------------
// Collect checked windows
// ---------------------------------------------------------------------------

// [=]===^=[ collect_checked ]=====================================[=]
static uint32_t collect_checked(Window *out, uint32_t max) {
	uint32_t count = 0;
	for(uint32_t i = 0; i < ts.win_count && count < max; ++i) {
		if(ts.windows[i].checked) {
			out[count++] = ts.windows[i].id;
		}
	}
	return count;
}

// ---------------------------------------------------------------------------
// mkgui UI
// ---------------------------------------------------------------------------

enum {
	ID_WINDOW = 0,
	ID_VBOX,
	ID_LISTVIEW,
	ID_SEL_HBOX,
	ID_BTN_ALL,
	ID_BTN_NONE,
	ID_BTN_REFRESH,
	ID_TILE_HBOX,
	ID_BTN_TILE_V,
	ID_BTN_TILE_H,
	ID_BTN_UNDO,
	ID_GRID_HBOX,
	ID_LBL_ROWS,
	ID_SPN_ROWS,
	ID_LBL_COLS,
	ID_SPN_COLS,
	ID_BTN_GRID,
};

static struct mkgui_column list_columns[] = {
	{ "",       30 },
	{ "Window", 350 },
};

// [=]===^=[ row_callback ]========================================[=]
static void row_callback(uint32_t row, uint32_t col, char *buf, uint32_t buf_size, void *userdata) {
	(void)userdata;
	if(row >= ts.win_count) {
		buf[0] = '\0';
		return;
	}
	if(col == 0) {
		buf[0] = ts.windows[row].checked ? '1' : '0';
		buf[1] = '\0';
	} else {
		snprintf(buf, buf_size, "%s", ts.windows[row].title);
	}
}

// [=]===^=[ refresh_list ]========================================[=]
static void refresh_list(struct mkgui_ctx *ctx) {
	x11_get_workarea();
	enumerate_windows();
	mkgui_listview_setup(ctx, ID_LISTVIEW, ts.win_count, 2, list_columns, row_callback, NULL);
	mkgui_listview_set_cell_type(ctx, ID_LISTVIEW, 0, MKGUI_CELL_CHECKBOX);
	dirty_all(ctx);
}

// [=]===^=[ main ]================================================[=]
int32_t main(int32_t argc, char **argv) {
	(void)argc;
	(void)argv;

	if(!x11_init()) {
		return 1;
	}

	enumerate_windows();

	struct mkgui_widget widgets[] = {
		{ MKGUI_WINDOW,   ID_WINDOW,      "mktile",      "", 0,            0, 0, 420, 500, 0, 0 },
		{ MKGUI_VBOX,     ID_VBOX,        "",            "", ID_WINDOW,    0, 0, 0, 0, MKGUI_ANCHOR_LEFT | MKGUI_ANCHOR_TOP | MKGUI_ANCHOR_RIGHT | MKGUI_ANCHOR_BOTTOM, 0 },
		{ MKGUI_LISTVIEW, ID_LISTVIEW,    "",            "", ID_VBOX,      0, 0, 0, 0, 0, 1 },
		{ MKGUI_HBOX,     ID_SEL_HBOX,    "",            "", ID_VBOX,      0, 0, 0, 28, MKGUI_FIXED, 0 },
		{ MKGUI_BUTTON,   ID_BTN_ALL,     "Select All",  "", ID_SEL_HBOX,  0, 0, 0, 0, 0, 0 },
		{ MKGUI_BUTTON,   ID_BTN_NONE,    "Deselect All","", ID_SEL_HBOX,  0, 0, 0, 0, 0, 0 },
		{ MKGUI_BUTTON,   ID_BTN_REFRESH, "Refresh",     "", ID_SEL_HBOX,  0, 0, 0, 0, 0, 0 },
		{ MKGUI_HBOX,     ID_TILE_HBOX,   "",            "", ID_VBOX,      0, 0, 0, 28, MKGUI_FIXED, 0 },
		{ MKGUI_BUTTON,   ID_BTN_TILE_V,  "Tile V",      "", ID_TILE_HBOX, 0, 0, 0, 0, 0, 0 },
		{ MKGUI_BUTTON,   ID_BTN_TILE_H,  "Tile H",      "", ID_TILE_HBOX, 0, 0, 0, 0, 0, 0 },
		{ MKGUI_BUTTON,   ID_BTN_UNDO,    "Undo",        "", ID_TILE_HBOX, 0, 0, 0, 0, 0, 0 },
		{ MKGUI_HBOX,     ID_GRID_HBOX,   "",            "", ID_VBOX,      0, 0, 0, 28, MKGUI_FIXED, 0 },
		{ MKGUI_LABEL,    ID_LBL_ROWS,    "Rows:",       "", ID_GRID_HBOX, 0, 0, 44, 0, MKGUI_FIXED, 0 },
		{ MKGUI_SPINBOX,  ID_SPN_ROWS,    "",            "", ID_GRID_HBOX, 0, 0, 70, 0, MKGUI_FIXED, 0 },
		{ MKGUI_LABEL,    ID_LBL_COLS,    "Cols:",       "", ID_GRID_HBOX, 0, 0, 44, 0, MKGUI_FIXED, 0 },
		{ MKGUI_SPINBOX,  ID_SPN_COLS,    "",            "", ID_GRID_HBOX, 0, 0, 70, 0, MKGUI_FIXED, 0 },
		{ MKGUI_BUTTON,   ID_BTN_GRID,    "Tile Grid",   "", ID_GRID_HBOX, 0, 0, 0, 0, 0, 0 },
	};
	uint32_t widget_count = sizeof(widgets) / sizeof(widgets[0]);

	struct mkgui_ctx *ctx = mkgui_create(widgets, widget_count);

	mkgui_listview_setup(ctx, ID_LISTVIEW, ts.win_count, 2, list_columns, row_callback, NULL);
	mkgui_listview_set_cell_type(ctx, ID_LISTVIEW, 0, MKGUI_CELL_CHECKBOX);
	mkgui_spinbox_setup(ctx, ID_SPN_ROWS, 1, 99, 2, 1);
	mkgui_spinbox_setup(ctx, ID_SPN_COLS, 1, 99, 2, 1);

	struct mkgui_event ev;
	uint32_t running = 1;

	while(running) {
		while(mkgui_poll(ctx, &ev)) {
			switch(ev.type) {
				case MKGUI_EVENT_CLOSE: {
					running = 0;
				} break;

				case MKGUI_EVENT_LISTVIEW_SELECT: {
					if(ev.id == ID_LISTVIEW && (uint32_t)ev.value < ts.win_count && ev.col == 0) {
						ts.windows[ev.value].checked = !ts.windows[ev.value].checked;
						dirty_all(ctx);
					}
				} break;

				case MKGUI_EVENT_LISTVIEW_REORDER: {
					if(ev.id == ID_LISTVIEW) {
						uint32_t src = (uint32_t)ev.value;
						uint32_t dst = (uint32_t)ev.col;
						if(src < ts.win_count && dst < ts.win_count) {
							struct window_entry tmp = ts.windows[src];
							if(src < dst) {
								memmove(&ts.windows[src], &ts.windows[src + 1], (dst - src) * sizeof(struct window_entry));
							} else {
								memmove(&ts.windows[dst + 1], &ts.windows[dst], (src - dst) * sizeof(struct window_entry));
							}
							ts.windows[dst] = tmp;
							dirty_all(ctx);
						}
					}
				} break;

				case MKGUI_EVENT_CLICK: {
					if(ev.id == ID_BTN_ALL) {
						for(uint32_t i = 0; i < ts.win_count; ++i) {
							ts.windows[i].checked = 1;
						}
						dirty_all(ctx);

					} else if(ev.id == ID_BTN_NONE) {
						for(uint32_t i = 0; i < ts.win_count; ++i) {
							ts.windows[i].checked = 0;
						}
						dirty_all(ctx);

					} else if(ev.id == ID_BTN_REFRESH) {
						refresh_list(ctx);

					} else if(ev.id == ID_BTN_TILE_V) {
						Window wins[MAX_WINDOWS];
						uint32_t count = collect_checked(wins, MAX_WINDOWS);
						if(count >= 2) {
							x11_get_workarea();
							undo_save(wins, count);
							tile_vertical(wins, count);
							running = 0;
						}

					} else if(ev.id == ID_BTN_TILE_H) {
						Window wins[MAX_WINDOWS];
						uint32_t count = collect_checked(wins, MAX_WINDOWS);
						if(count >= 2) {
							x11_get_workarea();
							undo_save(wins, count);
							tile_horizontal(wins, count);
							running = 0;
						}

					} else if(ev.id == ID_BTN_UNDO) {
						if(ts.undo_count > 0) {
							undo_restore();
							ts.undo_count = 0;
						}

					} else if(ev.id == ID_BTN_GRID) {
						Window wins[MAX_WINDOWS];
						uint32_t count = collect_checked(wins, MAX_WINDOWS);
						if(count >= 2) {
							uint32_t rows = (uint32_t)mkgui_spinbox_get(ctx, ID_SPN_ROWS);
							uint32_t cols = (uint32_t)mkgui_spinbox_get(ctx, ID_SPN_COLS);
							x11_get_workarea();
							undo_save(wins, count);
							tile_grid(wins, count, rows, cols);
							running = 0;
						}
					}
				} break;
			}
		}
		mkgui_wait(ctx);
	}

	mkgui_destroy(ctx);
	XCloseDisplay(ts.dpy);
	return 0;
}
