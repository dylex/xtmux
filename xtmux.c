/*
 * Copyright (c) 2011 Dylan Simon <dylan-tmux@dylex.net>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#include "tmux.h"

static void xtmux_update(struct tty *);
static void xtmux_fill_colors(struct xtmux *);

#define XTMUX_NUM_COLORS 256

static const unsigned char xtmux_colors[16][3] = {
	{0x00,0x00,0x00}, /* 0 black */
	{0xCC,0x00,0x00}, /* 1 red */
	{0x00,0xCC,0x00}, /* 2 green */
	{0xCC,0xCC,0x00}, /* 3 yellow */
	{0x00,0x00,0xCC}, /* 4 blue */
	{0xCC,0x00,0xCC}, /* 5 magenta */
	{0x00,0xCC,0xCC}, /* 6 cyan */
	{0xCC,0xCC,0xCC}, /* 7 white */
	{0x40,0x40,0x40}, /* 8 bright black */
	{0xFF,0x00,0x00}, /* 9 bright red */
	{0x00,0xFF,0x00}, /* 10 bright green */
	{0xFF,0xFF,0x00}, /* 11 bright yellow */
	{0x00,0x00,0xFF}, /* 12 bright blue */
	{0xFF,0x00,0xFF}, /* 13 bright magenta */
	{0x00,0xFF,0xFF}, /* 14 bright cyan */
	{0xFF,0xFF,0xFF}, /* 15 bright white */
};

struct xtmux {
	Display		*display;
	struct event	event;
	Window		window;

	XFontStruct	*font;
	u_short		font_width, font_height;

	GC		gc;
	unsigned long	colors[XTMUX_NUM_COLORS];
	u_char		fg, bg;

	struct client	*client; /* pointer back up to support redraws */
};

#define XSCREEN		DefaultScreen(x->display)
#define XCOLORMAP	DefaultColormap(x->display, XSCREEN)
// #define XUPDATE()	xtmux_update(tty)
#define XUPDATE()	event_active(&tty->xtmux->event, EV_READ, 0)

#define C2W(C) 		(x->font_width * (C))
#define C2H(C) 		(x->font_height * (C))
#define C2X(C) 		C2W(C)
#define C2Y(C) 		C2H(C)
#define PANE_X(X) 	(ctx->wp->xoff + (X))
#define PANE_Y(Y) 	(ctx->wp->yoff + (Y))
#define PANE_CX 	(PANE_X(ctx->ocx))
#define PANE_CY 	(PANE_Y(ctx->ocy))

void
xtmux_init(struct client *c, char *display)
{
	c->tty.xtmux = xcalloc(1, sizeof *c->tty.xtmux);

	xfree(c->tty.termname);
	c->tty.termname = xstrdup("xtmux");
	xfree(c->tty.path);
	c->tty.path = xstrdup(display);
	
	c->tty.xtmux->client = c;
}

static void
xdisplay_connection_callback(int fd, short events, void *data)
{
	struct tty *tty = data;

	if (events & EV_READ)
		XProcessInternalConnection(tty->xtmux->display, fd);
}

static void
xdisplay_connection_watch(unused Display *display, XPointer data, int fd, Bool opening, XPointer *watch_data)
{
	struct tty *tty = (struct tty *)data;
	struct event *ev;

	if (opening)
	{
		ev = xmalloc(sizeof *ev);
		event_set(ev, fd, EV_READ|EV_PERSIST, xdisplay_connection_callback, tty);
		if (event_add(ev, NULL) < 0)
			fatal("failed to add display X connection");
		*watch_data = (XPointer)ev;
	}
	else
	{
		ev = (struct event *)*watch_data;
		event_del(ev);
		xfree(ev);
	}
}

static void
xdisplay_callback(unused int fd, unused short events, void *data)
{
	log_debug("xdisplay_callback");
	xtmux_update((struct tty *)data);
}

int
xtmux_open(struct tty *tty, char **cause)
{
	struct xtmux *x = tty->xtmux;
	const char *font = "fixed";
	XWMHints wm_hints;
	XClassHint class_hints;
	XSizeHints size_hints;
	XGCValues gc_values;

	x->display = XOpenDisplay(tty->path);
	if (!x->display)
	{
		xasprintf(cause, "could not open X display: %s", tty->path);
		return -1;
	}

	event_set(&x->event, ConnectionNumber(x->display), EV_READ|EV_PERSIST, xdisplay_callback, tty);
	if (event_add(&x->event, NULL) < 0)
		fatal("failed to add X display event");

	if (!XAddConnectionWatch(x->display, &xdisplay_connection_watch, (XPointer)tty))
	{
		xasprintf(cause, "could not get X display connection: %s", tty->path);
		return -1;
	}

	x->font = XLoadQueryFont(x->display, font);
	if (!x->font)
	{
		xasprintf(cause, "could not load font: %s", font);
		return -1;
	}

	x->font_width = x->font->max_bounds.width;
	x->font_height = x->font->ascent + x->font->descent;

	x->window = XCreateSimpleWindow(x->display, DefaultRootWindow(x->display),
			0, 0, C2W(tty->sx), C2H(tty->sy),
			0, 0, 0);
	if (!x->window)
	{
		xasprintf(cause, "could create window");
		return -1;
	}

	size_hints.min_width = x->font_width;
	size_hints.min_height = x->font_height;
	size_hints.width_inc = x->font_width;
	size_hints.height_inc = x->font_height;
	size_hints.win_gravity = NorthWestGravity;
	size_hints.flags = PMinSize | PResizeInc | PWinGravity;
	wm_hints.input = True;
	wm_hints.initial_state = NormalState;
	wm_hints.flags = InputHint | StateHint;
	class_hints.res_name = (char *)"xtmux";
	class_hints.res_class = (char *)"Xtmux";
	Xutf8SetWMProperties(x->display, x->window, "xtmux", "xtmux", NULL, 0, &size_hints, &wm_hints, &class_hints);

	xtmux_fill_colors(x);
	x->fg = 7;
	x->bg = 0;

	gc_values.foreground = x->colors[x->fg];
	gc_values.background = x->colors[x->bg];
	gc_values.font = x->font->fid;
	x->gc = XCreateGC(x->display, x->window, GCForeground | GCBackground | GCFont, &gc_values);

	XSelectInput(x->display, x->window, KeyPressMask | ExposureMask | StructureNotifyMask);

	XMapWindow(x->display, x->window);

	tty->flags |= TTY_OPENED | TTY_NOCURSOR /* | TTY_UTF8 */;

	XUPDATE();

	return 0;
}

void 
xtmux_close(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;

	tty->flags &= ~TTY_OPENED;

	if (x->gc)
	{
		XFreeGC(x->display, x->gc);
		x->gc = 0;
	}

	if (x->window)
	{
		XFreeColors(x->display, XCOLORMAP, x->colors, XTMUX_NUM_COLORS, 0);

		XDestroyWindow(x->display, x->window);
		x->window = 0;
	}

	if (x->font)
	{
		XFreeFont(x->display, x->font);
		x->font = NULL;
	}

	if (x->display)
	{
		event_del(&x->event);

		XCloseDisplay(x->display);
		x->display = NULL;
	}
}

void 
xtmux_free(struct tty *tty)
{
	xtmux_close(tty);
	xfree(tty->xtmux);
}

void
xtmux_cursor(struct tty *tty, u_int cx, u_int cy)
{
	tty->cx = cx;
	tty->cy = cy;
}

void
xtmux_set_title(struct tty *tty, const char *title)
{
	struct xtmux *x = tty->xtmux;

	if (!x->window)
		return;

	XChangeProperty(x->display, x->window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, title, strlen(title));
}

static void
xtmux_fill_color(struct xtmux *x, u_int i, u_char r, u_char g, u_char b)
{
	XColor c;
	c.red   = r << 8 | r;
	c.green = g << 8 | g;
	c.blue  = b << 8 | b;
	if (!XAllocColor(x->display, XCOLORMAP, &c))
		c.pixel = (i & 1) ? WhitePixel(x->display, XSCREEN) : BlackPixel(x->display, XSCREEN);
	x->colors[i] = c.pixel;
}

static void
xtmux_fill_colors(struct xtmux *x)
{
	u_int c, i;
	u_char r, g, b;

	/* hard-coded */
	for (c = 0; c < 16; c ++)
		xtmux_fill_color(x, c, xtmux_colors[c][0], xtmux_colors[c][1], xtmux_colors[c][2]);

	/* 6x6x6 cube */
	for (r = 0; r < 6; r ++)
		for (g = 0; g < 6; g ++)
			for (b = 0; b < 6; b ++)
				xtmux_fill_color(x, c ++, 51*r, 51*g, 51*b);

	/* gray ramp */
	for (i = 1; i < 25; i ++)
		xtmux_fill_color(x, c ++, (51*i+3)/5, (51*i+3)/5, (51*i+3)/5);
}

static void
xtmux_attributes_set(struct xtmux *x, const struct grid_cell *gc)
{
	u_char			 fg = gc->fg, bg = gc->bg, flags = gc->flags;
	XGCValues		 gcv;
	unsigned long		 gcm = 0;

	if (!(flags & GRID_FLAG_FG256))
	{
		if (fg == 8)
			fg = x->fg;
		if (fg >= 90 && fg <= 97)
			fg -= 90 - 8;
	}
	if (!(flags & GRID_FLAG_BG256))
	{
		if (bg == 8)
			bg = x->bg;
		if (bg >= 100 && bg <= 107)
			bg -= 100 - 8;
	}

	if (flags & GRID_ATTR_REVERSE)
	{
		u_char xg = fg;
		fg = bg;
		bg = xg;
	}

	gcv.foreground = x->colors[fg];
	gcm |= GCForeground;
	gcv.background = x->colors[bg];
	gcm |= GCBackground;

	XChangeGC(x->display, x->gc, gcm, &gcv);
}

void
xtmux_attributes(struct tty *tty, const struct grid_cell *gc)
{
	xtmux_attributes_set(tty->xtmux, gc);
	tty->cell = *gc;
}

void
xtmux_reset(struct tty *tty)
{
	xtmux_attributes(tty, &grid_default_cell);
}

#define CLEAR(X, Y, W, H) \
	XClearArea(x->display, x->window, C2X(X), C2Y(Y), C2W(W), C2H(H), False)
#define COPY(X1, Y1, W, H, X2, Y2) \
	XCopyArea(x->display, x->window, x->window, x->gc, \
			C2X(X1), C2Y(Y1), C2W(W), C2H(H), C2X(X2), C2Y(Y2))

static u_int
grid_char(const struct grid_cell *gc, const struct grid_utf8 *gu)
{
	if (gc->flags & GRID_FLAG_PADDING)
		return 0;
	if (gc->flags & GRID_FLAG_UTF8)
	{
		struct utf8_data u8;
		u8.size = grid_utf8_size(gu);
		memcpy(u8.data, gu->data, u8.size);
		return utf8_combine(&u8);
	}
	return gc->data;
}

static void
xtmux_draw_char(struct xtmux *x, u_int cx, u_int cy, u_int c)
{
	u_int px = C2X(cx);
	u_int py = C2Y(cy);

	if (c == ' ')
	{
		XGCValues gcv;
		XGetGCValues(x->display, x->gc, GCForeground | GCBackground, &gcv);
		if (gcv.background == x->colors[x->bg])
			CLEAR(cx, cy, 1, 1);
		else
		{
			XSetForeground(x->display, x->gc, gcv.background);
			XFillRectangle(x->display, x->window, x->gc, px, py, x->font_width, x->font_height);
			XSetForeground(x->display, x->gc, gcv.foreground);
		}
	}
	else if (c > ' ')
	{
		XChar2b c2;
		py += x->font->ascent;
		c2.byte1 = c >> 8;
		c2.byte2 = c;
#if 0
		if (tty->cell.bg == x->bg)
			XDrawString16(x->display, x->window, x->gc, px, py, &c2, 1);
		else
#endif
			XDrawImageString16(x->display, x->window, x->gc, px, py, &c2, 1);
	}
}

static void
xtmux_draw_cell(struct xtmux *x, u_int cx, u_int cy, const struct grid_cell *gc, const struct grid_utf8 *gu)
{
	if (gc->flags & GRID_FLAG_PADDING)
		return;
	xtmux_attributes_set(x, gc);
	xtmux_draw_char(x, cx, cy, grid_char(gc, gu));
}

static void
xtmux_putwc(struct tty *tty, u_int c)
{
	xtmux_draw_char(tty->xtmux, tty->cx, tty->cy, c);
	XUPDATE();
}

void
xtmux_putc(struct tty *tty, u_char c)
{
	return xtmux_putwc(tty, c);
}

void
xtmux_pututf8(struct tty *tty, const struct grid_utf8 *gu, size_t size)
{
	struct utf8_data u8;

	u8.size = size;
	memcpy(u8.data, gu->data, u8.size);
	return xtmux_putwc(tty, utf8_combine(&u8));
}

void
xtmux_cmd_insertcharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dx = ctx->ocx + ctx->num;

	COPY(PANE_CX, PANE_CY,
			screen_size_x(s) - dx, 1,
			PANE_X(dx), PANE_CY);
	CLEAR(PANE_CX, PANE_CY,
			ctx->num, 1);

	XUPDATE();
}

void
xtmux_cmd_deletecharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dx = ctx->ocx + ctx->num;

	COPY(PANE_X(dx), PANE_CY, 
			screen_size_x(s) - dx, 1,
			PANE_CX, PANE_CY);
	CLEAR(PANE_X(screen_size_x(s) - ctx->num), PANE_CY, 
			ctx->num, 1);

	XUPDATE();
}

void
xtmux_cmd_insertline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dy = ctx->ocy + ctx->num;

	if (dy < ctx->orlower + 1)
		COPY(PANE_X(0), PANE_CY,
				screen_size_x(s), ctx->orlower + 1 - dy,
				PANE_X(0), PANE_Y(dy));
	CLEAR(PANE_X(0), PANE_CY,
			screen_size_x(s), ctx->num);

	XUPDATE();
}

void
xtmux_cmd_deleteline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dy = ctx->ocy + ctx->num;

	if (dy < ctx->orlower + 1)
		COPY(PANE_X(0), PANE_Y(dy),
				screen_size_x(s), ctx->orlower + 1 - dy,
				PANE_X(0), PANE_CY);
	CLEAR(PANE_X(0), PANE_Y(ctx->orlower + 1 - ctx->num),
			screen_size_x(s), ctx->num);

	XUPDATE();
}

void
xtmux_cmd_clearline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	CLEAR(PANE_X(0), PANE_CY,
			screen_size_x(s), 1);

	XUPDATE();
}

void
xtmux_cmd_clearendofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	CLEAR(PANE_CX, PANE_CY,
			screen_size_x(s) - ctx->ocx, 1);

	XUPDATE();
}

void
xtmux_cmd_clearstartofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;

	CLEAR(PANE_X(0), PANE_CY,
			ctx->ocx + 1, 1);

	XUPDATE();
}

void
xtmux_cmd_reverseindex(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	/* same as insertline(1) at top */
	COPY(PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), ctx->orlower - ctx->orupper,
			PANE_X(0), PANE_Y(ctx->orupper + 1));
	CLEAR(PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), 1);

	XUPDATE();
}

void
xtmux_cmd_linefeed(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	/* same as deleteline(1) at top */
	COPY(PANE_X(0), PANE_Y(ctx->orupper+1),
			screen_size_x(s), ctx->orlower - ctx->orupper,
			PANE_X(0), PANE_Y(ctx->orupper));
	CLEAR(PANE_X(0), PANE_Y(ctx->orlower),
			screen_size_x(s), 1);

	XUPDATE();
}

void
xtmux_cmd_clearendofscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int y = ctx->ocy;

	if (ctx->ocx > 0)
	{
		if (ctx->ocx < screen_size_x(s))
			CLEAR(PANE_CX, PANE_CY,
					screen_size_x(s) - ctx->ocx, 1);
		y ++;
	}
	if (y <= ctx->orlower)
		CLEAR(PANE_X(0), PANE_Y(y),
				screen_size_x(s), ctx->orlower + 1 - y);

	XUPDATE();
}

void
xtmux_cmd_clearstartofscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int y = ctx->ocy;

	if (ctx->ocx < screen_size_x(s))
		CLEAR(PANE_X(0), PANE_CY,
				ctx->ocx + 1, 1);
	else
		y ++;
	if (y > ctx->orupper)
		CLEAR(PANE_X(0), PANE_Y(ctx->orupper),
				screen_size_x(s), y);

	XUPDATE();
}

void
xtmux_cmd_clearscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	CLEAR(PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), ctx->orlower + 1 - ctx->orupper);

	XUPDATE();
}

static void
xtmux_redraw_pane(struct tty *tty, struct window_pane *wp, int left, int top, int right, int bot)
{
	struct xtmux *x = tty->xtmux;
	struct screen *s = wp->screen;
	u_int i, j;

	left -= wp->xoff;
	if (left < 0)
		left = 0;
	else if ((u_int)left > wp->sx)
		return;
	right -= wp->xoff;
	if (right <= 0)
		return;
	else if ((u_int)right > wp->sx)
		right = wp->sx;
	top -= wp->yoff;
	if (top < 0)
		top = 0;
	bot -= wp->yoff;
	if (bot <= 0)
		return;
	if ((u_int)bot > wp->sy)
		bot = wp->sy;

	for (j = top; j < (u_int)bot; j ++)
	{
		const struct grid_line *gl = &s->grid->linedata[s->grid->hsize+j];
		u_int sx = right;

		if (sx > gl->cellsize)
			sx = gl->cellsize;
		for (i = left; i < sx; i ++)
		{
			struct grid_cell gc = gl->celldata[i];
			const struct grid_utf8 *gu = &gl->utf8data[i];

			if (screen_check_selection(s, i, j)) {
				gc.attr = s->sel.cell.attr;
				gc.flags &= ~(GRID_FLAG_FG256|GRID_FLAG_BG256);
				gc.flags |= s->sel.cell.flags & (GRID_FLAG_FG256|GRID_FLAG_BG256);
				gc.fg = s->sel.cell.fg;
				gc.bg = s->sel.cell.bg;
			}
			xtmux_draw_cell(x, wp->xoff+i, wp->yoff+j, &gc, gu);
		}
	}
}

/* much like screen_redraw_screen, should possibly replace it */
static void
xtmux_redraw(struct client *c, int left, int top, int right, int bot)
{
	struct tty *tty = &c->tty;
	struct window *w = c->session->curw->window;
	struct window_pane *wp;

	if (bot >= (int)tty->sy && (c->message_string || c->prompt_string || options_get_number(&c->session->options, "status")))
	{
		/* fake status pane */
		struct window_pane swp;
		swp.screen = &c->status;
		swp.xoff = 0;
		swp.yoff = tty->sy-1;
		swp.sx = swp.screen->grid->sx;
		swp.sy = 1;
		xtmux_redraw_pane(tty, &swp, left, top, right, bot);
		bot = tty->sy-1;
	}

	TAILQ_FOREACH(wp, &w->panes, entry)
		xtmux_redraw_pane(tty, wp, left, top, right, bot);

	/* TODO: borders, numbers, cursor */
}

static void
xtmux_key_press(struct tty *tty, XKeyEvent *xev)
{
	int r, i;
	static char buf[32];
	KeySym key;

	r = XLookupString(xev, buf, sizeof buf, &key, NULL);
	for (i = 0; i < r; i ++)
		tty->key_callback(buf[i], NULL, tty->key_data);
}

static void 
xtmux_configure_notify(struct tty *tty, XConfigureEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	u_int sx, sy;
	
	do {
		sx = xev->width / x->font_width;;
		sy = xev->height / x->font_height;
	} while (XCheckTypedWindowEvent(x->display, x->window, ConfigureNotify, (XEvent *)xev));

	if (sx != tty->sx || sy != tty->sy)
	{
		tty->sx = sx;
		tty->sy = sy;
		xtmux_cursor(tty, 0, 0);
		recalculate_sizes();
	}
}

static void
xtmux_expose(struct tty *tty, XExposeEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	int x2 = xev->x + xev->width  + x->font_width  - 1;;
	int y2 = xev->y + xev->height + x->font_height - 1;

	xtmux_redraw(x->client, 
			xev->x / x->font_width, xev->y / x->font_height,
			x2     / x->font_width, y2     / x->font_height);
}

static void
xtmux_update(struct tty *tty)
{
	while (XPending(tty->xtmux->display))
	{
		XEvent xev;
		XNextEvent(tty->xtmux->display, &xev);

		switch (xev.type)
		{
			case KeyPress:
				xtmux_key_press(tty, &xev.xkey);
				break;

			case Expose:
				xtmux_expose(tty, &xev.xexpose);
				break;

			case ConfigureNotify:
				xtmux_configure_notify(tty, &xev.xconfigure);
				break;

			case MappingNotify:
				XRefreshKeyboardMapping(&xev.xmapping);
				break;

			default:
				log_warn("unhandled x event %d", xev.type);
		}
	}
}

