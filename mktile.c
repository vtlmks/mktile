// Copyright (c) 2026 — mktile
// SPDX-License-Identifier: MIT

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#define MAX_WINDOWS 512
#define MAX_TITLE   256

// [=]===^=[ atom IDs ]====================================[=]

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

// [=]===^=[ data structures ]=============================[=]

struct window_entry {
	Window   id;
	char     title[MAX_TITLE];
};

struct undo_entry {
	Window   id;
	int32_t  x, y;
	uint32_t w, h;
	bool     was_maximized;
};

struct work_area {
	int32_t  x, y;
	uint32_t w, h;
};

enum {
	COL_CHECKED = 0,
	COL_TITLE,
	COL_WIN_INDEX,
	COL_COUNT
};

struct state {
	Display      *dpy;
	Window        root;
	Atom          atoms[ATOM_COUNT];
	pid_t         self_pid;

	struct window_entry windows[MAX_WINDOWS];
	uint32_t            win_count;

	struct undo_entry   undo[MAX_WINDOWS];
	uint32_t            undo_count;

	struct work_area    area;
	uint32_t            grid_rows;
	uint32_t            grid_cols;

	GtkListStore *store;
	GtkWidget    *tree_view;
	GtkWidget    *spin_rows;
	GtkWidget    *spin_cols;
};

static struct state state;

// [=]===^=[ x11_get_property ]============================[=]
static unsigned char *x11_get_property(Window win, enum atom_id aid, Atom req_type, unsigned long *nitems_out) {
	Atom actual_type;
	int actual_format;
	unsigned long nitems, bytes_after;
	unsigned char *data = NULL;

	int rc = XGetWindowProperty(state.dpy, win, state.atoms[aid], 0, 1024, False, req_type, &actual_type, &actual_format, &nitems, &bytes_after, &data);

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

// [=]===^=[ x11_get_workarea ]============================[=]

static void x11_get_workarea(void) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(state.root, ATOM_NET_WORKAREA, XA_CARDINAL, &nitems);

	if(!data || nitems < 4) {
		state.area.x = 0;
		state.area.y = 0;
		state.area.w = DisplayWidth(state.dpy, DefaultScreen(state.dpy));
		state.area.h = DisplayHeight(state.dpy, DefaultScreen(state.dpy));
		if(data) {
			XFree(data);
		}
		return;
	}

	unsigned long *vals = (unsigned long *)data;
	state.area.x = (int32_t)vals[0];
	state.area.y = (int32_t)vals[1];
	state.area.w = (uint32_t)vals[2];
	state.area.h = (uint32_t)vals[3];
	XFree(data);
}

// [=]===^=[ x11_get_current_desktop ]=====================[=]
static unsigned long x11_get_current_desktop(void) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(state.root, ATOM_NET_CURRENT_DESKTOP, XA_CARDINAL, &nitems);

	if(!data) {
		return 0;
	}

	unsigned long desk = ((unsigned long *)data)[0];
	XFree(data);
	return desk;
}

// [=]===^=[ x11_get_window_desktop ]=======================[=]
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

// [=]===^=[ x11_get_window_title ]========================[=]
static void x11_get_window_title(Window win, char *buf, uint32_t buflen) {
	buf[0] = '\0';

	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_WM_NAME, state.atoms[ATOM_UTF8_STRING], &nitems);

	if(!data) {
		data = x11_get_property(win, ATOM_WM_NAME, XA_STRING, &nitems);
	}

	if(data) {
		snprintf(buf, buflen, "%s", (char *)data);
		XFree(data);
	}
}

// [=]===^=[ x11_get_frame_extents ]=======================[=]
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

// [=]===^=[ x11_has_wm_state ]============================[=]
static bool x11_has_wm_state(Window win, enum atom_id state_atom) {
	unsigned long nitems;
	unsigned char *data = x11_get_property(win, ATOM_NET_WM_STATE, XA_ATOM, &nitems);

	if(!data) {
		return false;
	}

	Atom *atoms = (Atom *)data;
	Atom target = state.atoms[state_atom];
	bool found = false;
	for(unsigned long i = 0; i < nitems; ++i) {
		if(atoms[i] == target) {
			found = true;
			break;
		}
	}

	XFree(data);
	return found;
}

// [=]===^=[ x11_get_window_pid ]===========================[=]
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

// [=]===^=[ x11_get_window_geom ]=========================[=]
static void x11_get_window_geom(Window win, int32_t *x, int32_t *y, uint32_t *w, uint32_t *h) {
	Window root_ret;
	int xr, yr;
	unsigned int wr, hr, bw, depth;

	XGetGeometry(state.dpy, win, &root_ret, &xr, &yr, &wr, &hr, &bw, &depth);

	Window child;
	XTranslateCoordinates(state.dpy, win, state.root, 0, 0, &xr, &yr, &child);

	uint32_t fl, fr, ft, fb;
	x11_get_frame_extents(win, &fl, &fr, &ft, &fb);

	*x = xr - (int32_t)fl;
	*y = yr - (int32_t)ft;
	*w = wr + fl + fr;
	*h = hr + ft + fb;
}

// [=]===^=[ x11_send_client_msg ]=========================[=]
static void x11_send_client_msg(Window win, enum atom_id msg, long d0, long d1, long d2, long d3, long d4) {
	XEvent ev;
	memset(&ev, 0, sizeof(ev));
	ev.xclient.type         = ClientMessage;
	ev.xclient.serial       = 0;
	ev.xclient.send_event   = True;
	ev.xclient.message_type = state.atoms[msg];
	ev.xclient.window       = win;
	ev.xclient.format       = 32;
	ev.xclient.data.l[0]    = d0;
	ev.xclient.data.l[1]    = d1;
	ev.xclient.data.l[2]    = d2;
	ev.xclient.data.l[3]    = d3;
	ev.xclient.data.l[4]    = d4;

	XSendEvent(state.dpy, state.root, False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

// [=]===^=[ x11_moveresize ]==============================[=]
static void x11_moveresize(Window win, int32_t x, int32_t y, uint32_t w, uint32_t h) {
	XSync(state.dpy, False);

	x11_send_client_msg(win, ATOM_NET_WM_STATE, 0, (long)state.atoms[ATOM_NET_WM_STATE_FULLSCREEN], 0, 0, 0);
	x11_send_client_msg(win, ATOM_NET_WM_STATE, 0, (long)state.atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT], (long)state.atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ], 0, 0);
	x11_send_client_msg(win, ATOM_NET_ACTIVE_WINDOW, 0, 0, 0, 0, 0);

	XMapRaised(state.dpy, win);
	XSync(state.dpy, False);

	uint32_t fl, fr, ft, fb;
	x11_get_frame_extents(win, &fl, &fr, &ft, &fb);

	uint32_t cw = (w > fl + fr) ? w - fl - fr : 1;
	uint32_t ch = (h > ft + fb) ? h - ft - fb : 1;

	XMoveResizeWindow(state.dpy, win, x + (int32_t)fl, y + (int32_t)ft, cw, ch);
	XSync(state.dpy, False);
}

// [=]===^=[ x11_init ]====================================[=]
static bool x11_init(void) {
	state.dpy = XOpenDisplay(NULL);
	if(!state.dpy) {
		fprintf(stderr, "mktile: cannot open X display\n");
		return false;
	}

	state.root = DefaultRootWindow(state.dpy);
	state.self_pid = getpid();

	for(uint32_t i = 0; i < ATOM_COUNT; ++i) {
		state.atoms[i] = XInternAtom(state.dpy, atom_names[i], False);
	}

	x11_get_workarea();
	return true;
}

// [=]===^=[ enumerate_windows ]===========================[=]
static void enumerate_windows(void) {
	state.win_count = 0;

	unsigned long nitems;
	unsigned char *data = x11_get_property(state.root, ATOM_NET_CLIENT_LIST,
		XA_WINDOW, &nitems);

	if(!data) {
		return;
	}

	unsigned long cur_desk = x11_get_current_desktop();
	Window *clients = (Window *)data;

	for(unsigned long i = 0; i < nitems && state.win_count < MAX_WINDOWS; ++i) {
		Window w = clients[i];

		pid_t wpid = x11_get_window_pid(w);
		if(wpid == state.self_pid) {
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

		struct window_entry *ent = &state.windows[state.win_count];
		ent->id = w;
		x11_get_window_title(w, ent->title, MAX_TITLE);

		if(ent->title[0] == '\0') {
			snprintf(ent->title, MAX_TITLE, "(untitled)");
		}

		++state.win_count;
	}

	XFree(data);
}

// [=]===^=[ tile_vertical ]===============================[=]
static void tile_vertical(Window *wins, uint32_t count) {
	if(count == 0) {
		return;
	}

	int32_t ax = state.area.x;
	int32_t ay = state.area.y;
	uint32_t aw = state.area.w;
	uint32_t ah = state.area.h;
	uint32_t step = ah / count;

	for(uint32_t i = 0; i < count; ++i) {
		uint32_t h = (i == count - 1) ? (ah - step * i) : step;
		x11_moveresize(wins[i], ax, ay + (int32_t)(step * i), aw, h);
	}
}

// [=]===^=[ tile_horizontal ]=============================[=]
static void tile_horizontal(Window *wins, uint32_t count) {
	if(count == 0) {
		return;
	}

	int32_t ax = state.area.x;
	int32_t ay = state.area.y;
	uint32_t aw = state.area.w;
	uint32_t ah = state.area.h;
	uint32_t step = aw / count;

	for(uint32_t i = 0; i < count; ++i) {
		uint32_t w = (i == count - 1) ? (aw - step * i) : step;
		x11_moveresize(wins[i], ax + (int32_t)(step * i), ay, w, ah);
	}
}

// [=]===^=[ tile_grid ]===================================[=]
static void tile_grid(Window *wins, uint32_t count, uint32_t rows, uint32_t cols) {
	if(count == 0 || rows == 0 || cols == 0) {
		return;
	}

	uint32_t max_slots = rows * cols;
	if(count > max_slots) {
		count = max_slots;
	}

	int32_t ax = state.area.x;
	int32_t ay = state.area.y;
	uint32_t aw = state.area.w;
	uint32_t ah = state.area.h;
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

// [=]===^=[ undo_save ]===================================[=]
static void undo_save(Window *wins, uint32_t count) {
	state.undo_count = 0;

	for(uint32_t i = 0; i < count && i < MAX_WINDOWS; ++i) {
		struct undo_entry *u = &state.undo[state.undo_count];
		u->id = wins[i];
		x11_get_window_geom(wins[i], &u->x, &u->y, &u->w, &u->h);
		u->was_maximized =
			x11_has_wm_state(wins[i], ATOM_NET_WM_STATE_MAXIMIZED_VERT) ||
			x11_has_wm_state(wins[i], ATOM_NET_WM_STATE_MAXIMIZED_HORZ);
		++state.undo_count;
	}
}

// [=]===^=[ undo_restore ]================================[=]
static void undo_restore(void) {
	for(uint32_t i = 0; i < state.undo_count; ++i) {
		struct undo_entry *u = &state.undo[i];

		if(u->was_maximized) {
			x11_send_client_msg(u->id, ATOM_NET_WM_STATE, 1,
				(long)state.atoms[ATOM_NET_WM_STATE_MAXIMIZED_VERT],
				(long)state.atoms[ATOM_NET_WM_STATE_MAXIMIZED_HORZ], 0, 0);
			XSync(state.dpy, False);

		} else {
			x11_moveresize(u->id, u->x, u->y, u->w, u->h);
		}
	}
}

// [=]===^=[ collect_checked ]=============================[=]
static uint32_t collect_checked(Window *out, uint32_t max) {
	uint32_t count = 0;
	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(state.store), &iter);

	while(valid && count < max) {
		gboolean checked;
		gint idx;
		gtk_tree_model_get(GTK_TREE_MODEL(state.store), &iter, COL_CHECKED, &checked, COL_WIN_INDEX, &idx, -1);

		if(checked && (uint32_t)idx < state.win_count) {
			out[count++] = state.windows[idx].id;
		}

		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(state.store), &iter);
	}

	return count;
}

// [=]===^=[ populate_store ]==============================[=]
static void populate_store(void) {
	gtk_list_store_clear(state.store);

	for(uint32_t i = 0; i < state.win_count; ++i) {
		GtkTreeIter iter;
		gtk_list_store_append(state.store, &iter);
		gtk_list_store_set(state.store, &iter, COL_CHECKED, FALSE, COL_TITLE, state.windows[i].title, COL_WIN_INDEX, (gint)i, -1);
	}
}

// [=]===^=[ on_toggle ]===================================[=]
static void on_toggle(GtkCellRendererToggle *cell, gchar *path_str, gpointer data) {
	(void)cell;
	(void)data;

	GtkTreePath *path = gtk_tree_path_new_from_string(path_str);
	GtkTreeIter iter;
	gtk_tree_model_get_iter(GTK_TREE_MODEL(state.store), &iter, path);
	gtk_tree_path_free(path);

	gboolean checked;
	gtk_tree_model_get(GTK_TREE_MODEL(state.store), &iter, COL_CHECKED, &checked, -1);
	gtk_list_store_set(state.store, &iter, COL_CHECKED, !checked, -1);
}

// [=]===^=[ on_select_all ]===============================[=]
static void on_select_all(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(state.store), &iter);

	while(valid) {
		gtk_list_store_set(state.store, &iter, COL_CHECKED, TRUE, -1);
		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(state.store), &iter);
	}
}

// [=]===^=[ on_deselect_all ]=============================[=]
static void on_deselect_all(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	GtkTreeIter iter;
	gboolean valid = gtk_tree_model_get_iter_first( GTK_TREE_MODEL(state.store), &iter);

	while(valid) {
		gtk_list_store_set(state.store, &iter, COL_CHECKED, FALSE, -1);
		valid = gtk_tree_model_iter_next(GTK_TREE_MODEL(state.store), &iter);
	}
}

// [=]===^=[ on_refresh ]==================================[=]
static void on_refresh(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	x11_get_workarea();
	enumerate_windows();
	populate_store();
}

// [=]===^=[ on_tile_vertical ]============================[=]
static void on_tile_vertical(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	Window wins[MAX_WINDOWS];
	uint32_t count = collect_checked(wins, MAX_WINDOWS);
	if(count < 2) {
		return;
	}

	x11_get_workarea();
	undo_save(wins, count);
	tile_vertical(wins, count);
	gtk_main_quit();
}

// [=]===^=[ on_tile_horizontal ]==========================[=]
static void on_tile_horizontal(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	Window wins[MAX_WINDOWS];
	uint32_t count = collect_checked(wins, MAX_WINDOWS);
	if(count < 2) {
		return;
	}

	x11_get_workarea();
	undo_save(wins, count);
	tile_horizontal(wins, count);
	gtk_main_quit();
}

// [=]===^=[ on_tile_grid ]================================[=]
static void on_tile_grid(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	Window wins[MAX_WINDOWS];
	uint32_t count = collect_checked(wins, MAX_WINDOWS);
	if(count < 2) {
		return;
	}

	uint32_t rows = (uint32_t)gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(state.spin_rows));
	uint32_t cols = (uint32_t)gtk_spin_button_get_value_as_int( GTK_SPIN_BUTTON(state.spin_cols));

	x11_get_workarea();
	undo_save(wins, count);
	tile_grid(wins, count, rows, cols);
	gtk_main_quit();
}

// [=]===^=[ on_undo ]=====================================[=]
static void on_undo(GtkWidget *widget, gpointer data) {
	(void)widget;
	(void)data;

	if(state.undo_count == 0) {
		return;
	}

	undo_restore();
	state.undo_count = 0;
}

// [=]===^=[ build_gui ]===================================[=]
static GtkWidget *build_gui(void) {
	GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
	gtk_window_set_title(GTK_WINDOW(window), "mktile");
	gtk_window_set_default_size(GTK_WINDOW(window), 420, 500);
	g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

	GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
	gtk_container_set_border_width(GTK_CONTAINER(vbox), 6);
	gtk_container_add(GTK_CONTAINER(window), vbox);

	state.store = gtk_list_store_new(COL_COUNT, G_TYPE_BOOLEAN, G_TYPE_STRING, G_TYPE_INT);

	state.tree_view = gtk_tree_view_new_with_model( GTK_TREE_MODEL(state.store));
	g_object_unref(state.store);

	GtkCellRenderer *toggle = gtk_cell_renderer_toggle_new();
	g_signal_connect(toggle, "toggled", G_CALLBACK(on_toggle), NULL);
	gtk_tree_view_append_column(GTK_TREE_VIEW(state.tree_view), gtk_tree_view_column_new_with_attributes("", toggle, "active", COL_CHECKED, NULL));

	GtkCellRenderer *text = gtk_cell_renderer_text_new();
	gtk_tree_view_append_column(GTK_TREE_VIEW(state.tree_view), gtk_tree_view_column_new_with_attributes("Window", text, "text", COL_TITLE, NULL));

	GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
	gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
	gtk_container_add(GTK_CONTAINER(scroll), state.tree_view);
	gtk_box_pack_start(GTK_BOX(vbox), scroll, TRUE, TRUE, 0);

	GtkWidget *sel_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(vbox), sel_box, FALSE, FALSE, 0);

	GtkWidget *btn_all = gtk_button_new_with_label("Select All");
	g_signal_connect(btn_all, "clicked", G_CALLBACK(on_select_all), NULL);
	gtk_box_pack_start(GTK_BOX(sel_box), btn_all, TRUE, TRUE, 0);

	GtkWidget *btn_none = gtk_button_new_with_label("Deselect All");
	g_signal_connect(btn_none, "clicked", G_CALLBACK(on_deselect_all), NULL);
	gtk_box_pack_start(GTK_BOX(sel_box), btn_none, TRUE, TRUE, 0);

	GtkWidget *btn_refresh = gtk_button_new_with_label("Refresh");
	g_signal_connect(btn_refresh, "clicked", G_CALLBACK(on_refresh), NULL);
	gtk_box_pack_start(GTK_BOX(sel_box), btn_refresh, TRUE, TRUE, 0);

	GtkWidget *tile_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(vbox), tile_box, FALSE, FALSE, 0);

	GtkWidget *btn_v = gtk_button_new_with_label("Tile V");
	g_signal_connect(btn_v, "clicked", G_CALLBACK(on_tile_vertical), NULL);
	gtk_box_pack_start(GTK_BOX(tile_box), btn_v, TRUE, TRUE, 0);

	GtkWidget *btn_h = gtk_button_new_with_label("Tile H");
	g_signal_connect(btn_h, "clicked", G_CALLBACK(on_tile_horizontal), NULL);
	gtk_box_pack_start(GTK_BOX(tile_box), btn_h, TRUE, TRUE, 0);

	GtkWidget *btn_undo = gtk_button_new_with_label("Undo");
	g_signal_connect(btn_undo, "clicked", G_CALLBACK(on_undo), NULL);
	gtk_box_pack_start(GTK_BOX(tile_box), btn_undo, TRUE, TRUE, 0);

	GtkWidget *grid_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
	gtk_box_pack_start(GTK_BOX(vbox), grid_box, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(grid_box), gtk_label_new("Rows:"), FALSE, FALSE, 0);
	state.spin_rows = gtk_spin_button_new_with_range(1, 99, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(state.spin_rows), 2);
	gtk_box_pack_start(GTK_BOX(grid_box), state.spin_rows, FALSE, FALSE, 0);

	gtk_box_pack_start(GTK_BOX(grid_box), gtk_label_new("Cols:"), FALSE, FALSE, 0);
	state.spin_cols = gtk_spin_button_new_with_range(1, 99, 1);
	gtk_spin_button_set_value(GTK_SPIN_BUTTON(state.spin_cols), 2);
	gtk_box_pack_start(GTK_BOX(grid_box), state.spin_cols, FALSE, FALSE, 0);

	GtkWidget *btn_grid = gtk_button_new_with_label("Tile Grid");
	g_signal_connect(btn_grid, "clicked", G_CALLBACK(on_tile_grid), NULL);
	gtk_box_pack_start(GTK_BOX(grid_box), btn_grid, TRUE, TRUE, 0);

	return window;
}

// [=]===^=[ main ]========================================[=]
int main(int argc, char **argv) {
	gtk_init(&argc, &argv);

	if(!x11_init()) {
		return 1;
	}

	enumerate_windows();

	GtkWidget *window = build_gui();
	populate_store();
	gtk_widget_show_all(window);

	gtk_main();

	XCloseDisplay(state.dpy);
	return 0;
}
