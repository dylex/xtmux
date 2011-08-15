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

static void xtmux_main(struct tty *);

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
	{0x80,0x80,0x80}, /* 8 bright black */
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
	Font		font_bold, font_italic;
	u_short		font_width, font_height;

	GC		gc, cursor_gc;
	unsigned long	fg, bg;
	unsigned long	colors[XTMUX_NUM_COLORS];

	int		cx, cy; /* last drawn cursor location, or -1 if none/hidden */

	KeySym		prefix_key;
	char		prefix_mod;

	struct client	*client; /* pointer back up to support redraws */
};

#define XSCREEN		DefaultScreen(x->display)
#define XCOLORMAP	DefaultColormap(x->display, XSCREEN)
// #define XUPDATE()	XFlush(tty->xtmux->display)
// #define XUPDATE()	xtmux_main(tty)
#define XUPDATE()	event_active(&tty->xtmux->event, EV_WRITE, 0)

#define C2W(C) 		(x->font_width * (C))
#define C2H(C) 		(x->font_height * (C))
#define C2X(C) 		C2W(C)
#define C2Y(C) 		C2H(C)
#define PANE_X(X) 	(ctx->wp->xoff + (X))
#define PANE_Y(Y) 	(ctx->wp->yoff + (Y))
#define PANE_CX 	(PANE_X(ctx->ocx))
#define PANE_CY 	(PANE_Y(ctx->ocy))

#define INSIDE1(x, X, L) \
			((unsigned)((x) - (X)) < (unsigned)(L))
#define INSIDE(x, y, X, Y, W, H) \
			(INSIDE1(x, X, W) && INSIDE1(y, Y, H))

#define EVENT_MASK	(KeyPressMask | ExposureMask | StructureNotifyMask)

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
	xtmux_main((struct tty *)data);
}

static unsigned long
xt_parse_color(struct xtmux *x, char *s, unsigned long def)
{
	int n;
	XColor c, m;
	size_t cl;
	char *p = s;
	const char *e;

	/* partial colour_fromstring */
	if (strncasecmp(p, "colour", cl = (sizeof "colour") - 1) == 0 ||
			strncasecmp(p, "color", cl = (sizeof "color") - 1) == 0)
		p += cl;
	n = strtonum(p, 0, 255, &e);
	if (!e)
		return x->colors[n];

	if (XLookupColor(x->display, XCOLORMAP, s, &m, &c) &&
			XAllocColor(x->display, XCOLORMAP, &c))
		return c.pixel;

	return def;
}

static void
xt_fill_color(struct xtmux *x, u_int i, u_char r, u_char g, u_char b)
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
xt_fill_colors(struct xtmux *x, const char *colors)
{
	u_int c, i;
	u_char r, g, b;
	char *s, *cs, *cn;

	/* hard-coded */
	for (c = 0; c < 16; c ++)
		xt_fill_color(x, c, xtmux_colors[c][0], xtmux_colors[c][1], xtmux_colors[c][2]);

	/* 6x6x6 cube */
	for (r = 0; r < 6; r ++)
		for (g = 0; g < 6; g ++)
			for (b = 0; b < 6; b ++)
				xt_fill_color(x, c ++, 51*r, 51*g, 51*b);

	/* gray ramp */
	for (i = 1; i < 25; i ++)
		xt_fill_color(x, c ++, (51*i+3)/5, (51*i+3)/5, (51*i+3)/5);

	cn = s = xstrdup(colors);
	while ((cs = strsep(&cn, ";, ")))
	{
		char *es;
		int ci;
		if (!(es = strchr(cs, '=')))
			continue;
		*es++ = 0;

		if ((ci = colour_fromstring(cs)) < 0)
			continue;
		x->colors[ci & 0xff] = xt_parse_color(x, es, x->colors[ci]);
	}
	xfree(s);
}

void
xtmux_setup(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;
	struct options *o = &x->client->options;
	XFontStruct *fs;
	const char *font, *prefix;
	KeySym pkey = NoSymbol;

	font = options_get_string(o, "xtmux-font");
	fs = XLoadQueryFont(x->display, font);
	if (fs)
	{
		if (x->font)
			XFreeFont(x->display, x->font);
		x->font = fs;
		x->font_width = fs->max_bounds.width;
		x->font_height = fs->ascent + fs->descent;

		if (x->window)
		{
			Window root;
			int xpos, ypos, border, depth;
			u_int width, height;

			XGetGeometry(x->display, x->window, &root, &xpos, &ypos, &width, &height, &border, &depth);
			tty->sx = width  / x->font_width;
			tty->sy = height / x->font_height;

			recalculate_sizes();
		}
	}

	if (x->font_bold)
	{
		XUnloadFont(x->display, x->font_bold);
		x->font_bold = None;
	}
	if (*(font = options_get_string(o, "xtmux-bold-font")))
		x->font_bold = XLoadFont(x->display, font);

	if (x->font_italic)
	{
		XUnloadFont(x->display, x->font_italic);
		x->font_italic = None;
	}
	if (*(font = options_get_string(o, "xtmux-italic-font")))
		x->font_italic = XLoadFont(x->display, font);

	xt_fill_colors(x, options_get_string(o, "xtmux-colors"));
	x->bg = xt_parse_color(x, options_get_string(o, "xtmux-bg"), BlackPixel(x->display, XSCREEN));
	x->fg = xt_parse_color(x, options_get_string(o, "xtmux-fg"), WhitePixel(x->display, XSCREEN));
	if (x->window)
		XSetWindowBackground(x->display, x->window, x->bg);

	prefix = options_get_string(o, "xtmux-prefix");
	x->prefix_mod = -1;
	if (strlen(prefix) == 4 && !strncasecmp(prefix, "mod", 3) && prefix[3] >= '1' && prefix[3] <= '5')
		x->prefix_mod = Mod1MapIndex + prefix[3] - '1';
	else if (!strcasecmp(prefix, "meta")) 
		pkey = XK_Meta_L;
	else if (!strcasecmp(prefix, "alt")) 
		pkey = XK_Alt_L;
	else if (!strcasecmp(prefix, "super")) 
		pkey = XK_Super_L;
	else if (!strcasecmp(prefix, "hyper")) 
		pkey = XK_Hyper_L;
	else if (!strcasecmp(prefix, "control") || !strcasecmp(prefix, "ctrl")) 
		pkey = XK_Control_L;
	else if (*prefix)
		pkey = XStringToKeysym(prefix);
	if (pkey != NoSymbol)
	{
		XModifierKeymap *xmodmap = XGetModifierMapping(x->display);
		int i;

		for (i = 0; i < 8*xmodmap->max_keypermod; i ++)
			if (xmodmap->modifiermap[i] == pkey)
			{
				pkey = NoSymbol;
				x->prefix_mod = i/xmodmap->max_keypermod;
				break;
			}

		XFreeModifiermap(xmodmap);
	}
	x->prefix_key = pkey;
}

int
xtmux_open(struct tty *tty, char **cause)
{
	struct xtmux *x = tty->xtmux;
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

	xtmux_setup(tty);
	if (!x->font)
	{
		xasprintf(cause, "could not load font");
		return -1;
	}

	x->window = XCreateSimpleWindow(x->display, DefaultRootWindow(x->display),
			0, 0, C2W(tty->sx), C2H(tty->sy),
			0, 0, x->bg);
	if (x->window == None)
	{
		xasprintf(cause, "could not create window");
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

	gc_values.foreground = x->fg;
	gc_values.background = x->bg;
	gc_values.font = x->font->fid;
	x->gc = XCreateGC(x->display, x->window, GCForeground | GCBackground | GCFont, &gc_values);

	gc_values.foreground = WhitePixel(x->display, XSCREEN);
	gc_values.background = BlackPixel(x->display, XSCREEN);
	gc_values.function = GXxor; /* this'll be fine for TrueColor, etc, but we might want to avoid PseudoColor for this */
	x->cursor_gc = XCreateGC(x->display, x->window, GCFunction | GCForeground | GCBackground, &gc_values);

	XSelectInput(x->display, x->window, EVENT_MASK);

	XMapWindow(x->display, x->window);

	tty->flags |= TTY_OPENED | TTY_UTF8;
	x->cx = x->cy = -1;

	XUPDATE();

	return 0;
}

void 
xtmux_close(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;

	tty->flags &= ~TTY_OPENED;

	if (x->cursor_gc != None)
	{
		XFreeGC(x->display, x->cursor_gc);
		x->cursor_gc = None;
	}

	if (x->gc != None)
	{
		XFreeGC(x->display, x->gc);
		x->gc = None;
	}

	if (x->window != None)
	{
		XFreeColors(x->display, XCOLORMAP, x->colors, XTMUX_NUM_COLORS, 0);

		XDestroyWindow(x->display, x->window);
		x->window = None;
	}

	if (x->font_bold != None)
	{
		XUnloadFont(x->display, x->font_bold);
		x->font_bold = None;
	}

	if (x->font_italic != None)
	{
		XUnloadFont(x->display, x->font_italic);
		x->font_italic = None;
	}

	if (x->font != NULL)
	{
		XFreeFont(x->display, x->font);
		x->font = NULL;
	}

	if (x->display != NULL)
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
xtmux_set_title(struct tty *tty, const char *title)
{
	struct xtmux *x = tty->xtmux;

	if (!x->window)
		return;

	XChangeProperty(x->display, x->window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, title, strlen(title));
}

void
xtmux_attributes(struct tty *tty, const struct grid_cell *gc)
{
	tty->cell = *gc;
}

void
xtmux_reset(struct tty *tty)
{
	xtmux_attributes(tty, &grid_default_cell);
}

static int
xt_draw_cursor(struct xtmux *x, int cx, int cy)
{
	if (x->cx == cx && x->cy == cy)
		return 0;

	if (x->cx >= 0)
	{
		XFillRectangle(x->display, x->window, x->cursor_gc, C2X(x->cx), C2Y(x->cy), x->font_width, x->font_height);
		x->cx = -1;
		x->cy = -1;
	}

	if (cx >= 0)
	{
		XFillRectangle(x->display, x->window, x->cursor_gc, C2X(cx), C2Y(cy), x->font_width, x->font_height);
		x->cx = cx;
		x->cy = cy;
	}

	return 1;
}

#define DRAW_CURSOR(X, CX, CY) ({ if (xt_draw_cursor(X, CX, CY)) XUPDATE(); })

static void
xt_invalidate(struct xtmux *x, u_int cx, u_int cy, u_int w, u_int h)
{
	if (INSIDE(x->cx, x->cy, cx, cy, w, h))
		x->cx = x->cy = -1;
}

static void
xt_clear(struct xtmux *x, u_int cx, u_int cy, u_int w, u_int h)
{
	xt_invalidate(x, cx, cy, w, h);
	XClearArea(x->display, x->window, C2X(cx), C2Y(cy), C2W(w), C2H(h), False);
}

static void
xt_copy(struct xtmux *x, u_int x1, u_int y1, u_int x2, u_int y2, u_int w, u_int h)
{
	if (INSIDE(x->cx, x->cy, x1, y1, w, h))
		xt_draw_cursor(x, -1, -1);
	else 
		xt_invalidate(x, x2, y2, w, h);
	XCopyArea(x->display, x->window, x->window, x->gc,
			C2X(x1), C2Y(y1), C2W(w), C2H(h), C2X(x2), C2Y(y2));
}

void
xtmux_cursor(struct tty *tty, u_int cx, u_int cy)
{
	if (tty->mode & MODE_CURSOR)
		DRAW_CURSOR(tty->xtmux, cx, cy);

	tty->cx = cx;
	tty->cy = cy;
}

void
xtmux_update_mode(struct tty *tty, int mode, struct screen *s)
{
	struct xtmux *x = tty->xtmux;

	if (mode & MODE_CURSOR)
		DRAW_CURSOR(x, tty->cx, tty->cy);
	else if (!(s->mode & MODE_CURSOR))
		DRAW_CURSOR(x, -1, -1);

	if ((tty->mode ^ mode) & (MODE_MOUSE_STANDARD | MODE_MOUSE_BUTTON | MODE_MOUSE_ANY))
	{
		long em = EVENT_MASK;

		if (mode & (MODE_MOUSE_STANDARD | MODE_MOUSE_BUTTON | MODE_MOUSE_ANY))
			em |= ButtonPressMask | ButtonReleaseMask;
		if (mode & MODE_MOUSE_ANY)
			em |= PointerMotionMask;
		else if (mode & MODE_MOUSE_BUTTON)
			em |= ButtonMotionMask;
		XSelectInput(x->display, x->window, em);
	}

	tty->mode = mode;
}

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
xt_draw_char(struct xtmux *x, u_int cx, u_int cy, u_int c, const struct grid_cell *gc, int cleared)
{
	u_int			px = C2X(cx), py = C2Y(cy);
	u_char			fgc = gc->fg, bgc = gc->bg;
	unsigned long		fg, bg;

	if (fgc >= 90 && fgc <= 97 && !(gc->flags & GRID_FLAG_FG256))
		fgc -= 90 - 8;
	if (bgc >= 100 && bgc <= 107 && !(gc->flags & GRID_FLAG_BG256))
		bgc -= 100 - 8;

	if (fgc == 8 && !(gc->flags & GRID_FLAG_FG256))
		fg = x->fg;
	else
		fg = x->colors[fgc];

	if (bgc == 8 && !(gc->flags & GRID_FLAG_BG256))
		bg = x->bg;
	else
		bg = x->colors[bgc];

	if (gc->attr & GRID_ATTR_REVERSE)
	{
		unsigned long xg;
		u_char xgc;
		xg = fg; xgc = fgc;
		fg = bg; fgc = bgc;
		bg = xg; bgc = xgc;
	}

	/* TODO: configurable BRIGHT semantics */
	if (gc->attr & GRID_ATTR_BRIGHT && fgc < 8 && fg == bg)
		fg = x->colors[fgc += 8];

	/* TODO: DIM, maybe only for TrueColor */

	if (c == ' ' || gc->attr & GRID_ATTR_HIDDEN)
	{
		if (bg == x->bg)
		{
			if (!cleared)
				XClearArea(x->display, x->window, px, py, x->font_width, x->font_height, False);
		}
		else
		{
			XSetForeground(x->display, x->gc, bg);
			XFillRectangle(x->display, x->window, x->gc, px, py, x->font_width, x->font_height);
		}
	}
	else if (c > ' ')
	{
		XChar2b c2;
		
		XSetForeground(x->display, x->gc, fg);

		if (gc->attr & GRID_ATTR_ITALICS && x->font_italic)
			XSetFont(x->display, x->gc, x->font_italic);
		else if (gc->attr & GRID_ATTR_BRIGHT && x->font_bold)
			XSetFont(x->display, x->gc, x->font_bold);
		else
			XSetFont(x->display, x->gc, x->font->fid);

		/* TODO: fix ACS arrows, block, etc? */
		if (gc->attr & GRID_ATTR_CHARSET && c > 0x5f && c < 0x7f)
			c -= 0x5f;
		c2.byte1 = c >> 8;
		c2.byte2 = c;

		if (cleared && bg == x->bg)
			XDrawString16(x->display, x->window, x->gc, px, py + x->font->ascent, &c2, 1);
		else
		{
			XSetBackground(x->display, x->gc, bg);
			XDrawImageString16(x->display, x->window, x->gc, px, py + x->font->ascent, &c2, 1);
		}
	}

	if (gc->attr & GRID_ATTR_UNDERSCORE)
	{
		u_int y = py + x->font->ascent;
		if (x->font->descent > 1)
			y ++;
		XSetForeground(x->display, x->gc, fg);
		XDrawLine(x->display, x->window, x->gc, 
				px, y,
				px + x->font_width - 1, y);
	}
	if (gc->attr & GRID_ATTR_BLINK)
	{
		/* a little odd but blink is weird anyway */
		XDrawRectangle(x->display, x->window, x->cursor_gc,
				px-1, py-1,
				x->font_width, x->font_height);
	}
}

static void
xt_draw_cell(struct xtmux *x, u_int cx, u_int cy, const struct grid_cell *gc, const struct grid_utf8 *gu)
{
	if (gc->flags & GRID_FLAG_PADDING)
		return;
	xt_draw_char(x, cx, cy, grid_char(gc, gu), gc, 1);
}

static void
xtmux_putwc(struct tty *tty, u_int c)
{
	if (tty->cx >= tty->sx)
	{
		tty->cx = 0;
		if (tty->cy < tty->rlower)
			tty->cy ++;
	}
	xt_draw_char(tty->xtmux, tty->cx, tty->cy, c, &tty->cell, 0);
	xt_invalidate(tty->xtmux, tty->cx, tty->cy, 1, 1);
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

	xt_copy(x,
			PANE_CX, PANE_CY,
			PANE_X(dx), PANE_CY,
			screen_size_x(s) - dx, 1);
	xt_clear(x,
			PANE_CX, PANE_CY,
			ctx->num, 1);

	XUPDATE();
}

void
xtmux_cmd_deletecharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dx = ctx->ocx + ctx->num;

	xt_copy(x,
			PANE_X(dx), PANE_CY, 
			PANE_CX, PANE_CY,
			screen_size_x(s) - dx, 1);
	xt_clear(x,
			PANE_X(screen_size_x(s) - ctx->num), PANE_CY,
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
		xt_copy(x,
				PANE_X(0), PANE_CY,
				PANE_X(0), PANE_Y(dy),
				screen_size_x(s), ctx->orlower + 1 - dy);
	xt_clear(x,
			PANE_X(0), PANE_CY,
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
		xt_copy(x,
				PANE_X(0), PANE_Y(dy),
				PANE_X(0), PANE_CY,
				screen_size_x(s), ctx->orlower + 1 - dy);
	xt_clear(x,
			PANE_X(0), PANE_Y(ctx->orlower + 1 - ctx->num),
			screen_size_x(s), ctx->num);

	XUPDATE();
}

void
xtmux_cmd_clearline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	xt_clear(x,
			PANE_X(0), PANE_CY,
			screen_size_x(s), 1);

	XUPDATE();
}

void
xtmux_cmd_clearendofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	xt_clear(x,
			PANE_CX, PANE_CY,
			screen_size_x(s) - ctx->ocx, 1);

	XUPDATE();
}

void
xtmux_cmd_clearstartofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;

	xt_clear(x,
			PANE_X(0), PANE_CY,
			ctx->ocx + 1, 1);

	XUPDATE();
}

void
xtmux_cmd_reverseindex(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	/* same as insertline(1) at top */
	xt_copy(x,
			PANE_X(0), PANE_Y(ctx->orupper),
			PANE_X(0), PANE_Y(ctx->orupper + 1),
			screen_size_x(s), ctx->orlower - ctx->orupper);
	xt_clear(x,
			PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), 1);

	XUPDATE();
}

void
xtmux_cmd_linefeed(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	/* same as deleteline(1) at top */
	xt_copy(x,
			PANE_X(0), PANE_Y(ctx->orupper+1),
			PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), ctx->orlower - ctx->orupper);
	xt_clear(x,
			PANE_X(0), PANE_Y(ctx->orlower),
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
			xt_clear(x,
					PANE_CX, PANE_CY,
					screen_size_x(s) - ctx->ocx, 1);
		y ++;
	}
	if (y <= ctx->orlower)
		xt_clear(x,
				PANE_X(0), PANE_Y(y),
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
		xt_clear(x,
				PANE_X(0), PANE_CY,
				ctx->ocx + 1, 1);
	else
		y ++;
	if (y > ctx->orupper)
		xt_clear(x,
				PANE_X(0), PANE_Y(ctx->orupper),
				screen_size_x(s), y);

	XUPDATE();
}

void
xtmux_cmd_clearscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	xt_clear(x,
			PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), ctx->orlower + 1 - ctx->orupper);

	XUPDATE();
}

void
xtmux_cmd_setselection(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;

	XSetSelectionOwner(x->display, XA_PRIMARY, x->window, CurrentTime);
	if (XGetSelectionOwner(x->display, XA_PRIMARY) != x->window)
		return;

	XChangeProperty(x->display, DefaultRootWindow(x->display),
			XA_CUT_BUFFER0, XA_STRING, 8, PropModeReplace, ctx->ptr, ctx->num);

	XUPDATE();
}

void
xtmux_bell(struct tty *tty)
{
	XBell(tty->xtmux->display, 100);

	XUPDATE();
}

static void
xt_draw_line(struct xtmux *x, struct screen *s, u_int py, u_int left, u_int right, u_int ox, u_int oy)
{
	const struct grid_line *gl = &s->grid->linedata[s->grid->hsize+py];
	u_int sx = right;
	u_int i;

	if (sx > gl->cellsize)
		sx = gl->cellsize;
	for (i = left; i < sx; i ++)
	{
		struct grid_cell gc = gl->celldata[i];
		const struct grid_utf8 *gu = &gl->utf8data[i];

		if (screen_check_selection(s, i, py)) {
			gc.attr = s->sel.cell.attr;
			gc.flags &= ~(GRID_FLAG_FG256|GRID_FLAG_BG256);
			gc.flags |= s->sel.cell.flags & (GRID_FLAG_FG256|GRID_FLAG_BG256);
			gc.fg = s->sel.cell.fg;
			gc.bg = s->sel.cell.bg;
		}
		xt_draw_cell(x, ox+i, oy+py, &gc, gu);
	}
}

void
xtmux_draw_line(struct tty *tty, struct screen *s, u_int py, u_int ox, u_int oy)
{
	u_int sx = screen_size_x(s);

	if (sx > tty->sx)
		sx = tty->sx;

	xt_clear(tty->xtmux, ox, oy+py, sx, 1);
	xt_draw_line(tty->xtmux, s, py, 0, sx, ox, oy);

	XUPDATE();
}

static void
xtmux_redraw_pane(struct tty *tty, struct window_pane *wp, int left, int top, int right, int bot)
{
	struct xtmux *x = tty->xtmux;
	struct screen *s = wp->screen;
	u_int y;

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

	for (y = top; y < (u_int)bot; y ++)
		xt_draw_line(x, s, y, left, right, wp->xoff, wp->yoff);
}

/* much like screen_redraw_screen, should possibly replace it */
static void
xtmux_redraw(struct client *c, int left, int top, int right, int bot)
{
	struct tty *tty = &c->tty;
	struct window *w = c->session->curw->window;
	struct window_pane *wp;

	xt_invalidate(tty->xtmux, left, top, right-left, bot-top);

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

	/* TODO: borders, numbers */

	xtmux_cursor(tty, tty->cx, tty->cy);
}

static void
xtmux_key_press(struct tty *tty, XKeyEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	int r, i, key;
	static char buf[32];
	KeySym xks;

	r = XLookupString(xev, buf, sizeof buf, &xks, NULL);
	if (x->prefix_key && xks == x->prefix_key)
		key = KEYC_PREFIX;
	else switch (xks)
	{
		case XK_BackSpace: 	key = KEYC_BSPACE;	break;
		case XK_F1: 		key = KEYC_F1; 	   	break;
		case XK_F2: 		key = KEYC_F2; 	   	break;
		case XK_F3: 		key = KEYC_F3; 	   	break;
		case XK_F4: 		key = KEYC_F4; 	 	break;
		case XK_F5: 		key = KEYC_F5; 	 	break;
		case XK_F6: 		key = KEYC_F6; 	 	break;
		case XK_F7: 		key = KEYC_F7; 	 	break;
		case XK_F8: 		key = KEYC_F8; 	 	break;
		case XK_F9: 		key = KEYC_F9; 	 	break;
		case XK_F10: 		key = KEYC_F10;  	break;
		case XK_F11: 		key = KEYC_F11;  	break;
		case XK_F12: 		key = KEYC_F12;  	break;
		case XK_F13: 		key = KEYC_F13;  	break;
		case XK_F14: 		key = KEYC_F14;  	break;
		case XK_F15: 		key = KEYC_F15;  	break;
		case XK_F16: 		key = KEYC_F16;  	break;
		case XK_F17: 		key = KEYC_F17;  	break;
		case XK_F18: 		key = KEYC_F18;  	break;
		case XK_F19: 		key = KEYC_F19;  	break;
		case XK_F20: 		key = KEYC_F20;  	break;
		case XK_KP_Insert:
		case XK_Insert:		key = KEYC_IC;		break;
		case XK_KP_Delete:
		case XK_Delete:		key = KEYC_DC;		break;
		case XK_KP_Begin:
		case XK_Begin:
		case XK_KP_Home:
		case XK_Home:		key = KEYC_HOME;	break;
		case XK_KP_End:
		case XK_End:		key = KEYC_END;		break;
		case XK_KP_Next:
		case XK_Next:		key = KEYC_NPAGE;	break;
		case XK_KP_Prior:
		case XK_Prior:		key = KEYC_PPAGE;	break;
		case XK_ISO_Left_Tab:	key = KEYC_BTAB;	break;
		case XK_KP_Up:
		case XK_Up:		key = KEYC_UP;		break;
		case XK_KP_Down:
		case XK_Down:		key = KEYC_DOWN;	break;
		case XK_KP_Left:
		case XK_Left:		key = KEYC_LEFT;	break;
		case XK_KP_Right:
		case XK_Right:		key = KEYC_RIGHT;	break;
		case XK_KP_Divide:	key = KEYC_KP_SLASH;	break;
		case XK_KP_Multiply:	key = KEYC_KP_STAR;	break;
		case XK_KP_Subtract:	key = KEYC_KP_MINUS;	break;
		case XK_KP_7:		key = KEYC_KP_SEVEN;	break;
		case XK_KP_8:		key = KEYC_KP_EIGHT;	break;
		case XK_KP_9:		key = KEYC_KP_NINE;	break;
		case XK_KP_Add:		key = KEYC_KP_PLUS;	break;
		case XK_KP_4:		key = KEYC_KP_FOUR;	break;
		case XK_KP_5:		key = KEYC_KP_FIVE;	break;
		case XK_KP_6:		key = KEYC_KP_SIX;	break;
		case XK_KP_1:		key = KEYC_KP_ONE;	break;
		case XK_KP_2:		key = KEYC_KP_TWO;	break;
		case XK_KP_3:		key = KEYC_KP_THREE;	break;
		case XK_KP_Enter:	key = KEYC_KP_ENTER;	break;
		case XK_KP_0:		key = KEYC_KP_ZERO;	break;
		case XK_KP_Decimal:	key = KEYC_KP_PERIOD;	break;
		default:		key = 0;
	}

	if (key)
	{
		r = -1;
		if (xev->state & ShiftMask)
			key |= KEYC_SHIFT;
		if (xev->state & ControlMask)
			key |= KEYC_CTRL;
		if (xev->state & (x->prefix_mod == Mod1MapIndex ? Mod4Mask : Mod1Mask)) /* ALT */
			key |= KEYC_ESCAPE;
	}

	if (x->prefix_mod >= 0 && xev->state & (1<<x->prefix_mod))
		key |= KEYC_PREFIX;

	if (r < 0)
		tty->key_callback(key, NULL, tty->key_data);
	else for (i = 0; i < r; i ++)
		tty->key_callback(key | buf[i], NULL, tty->key_data);
}

static void
xtmux_button_press(struct tty *tty, XButtonEvent *xev)
{
	struct mouse_event m;

	m.x = xev->x / tty->xtmux->font_width;
	m.y = xev->y / tty->xtmux->font_height;

	switch (xev->type) {
		case ButtonPress:
			switch (xev->button)
			{
				case Button1: m.b = MOUSE_1; break;
				case Button2: m.b = MOUSE_2; break;
				case Button3: m.b = MOUSE_3; break;
				case Button4: m.b = MOUSE_1 | MOUSE_45; break;
				case Button5: m.b = MOUSE_2 | MOUSE_45; break;
				default: return;
			}
		case ButtonRelease:
			m.b = MOUSE_UP;
			break;
		case MotionNotify:
			if (xev->state & Button1Mask)
				m.b = MOUSE_1;
			else if (xev->state & Button2Mask)
				m.b = MOUSE_2;
			else if (xev->state & Button3Mask)
				m.b = MOUSE_3;
			else if (xev->state & Button4Mask)
				m.b = MOUSE_1 | MOUSE_45;
			else if (xev->state & Button5Mask)
				m.b = MOUSE_2 | MOUSE_45;
			else
				m.b = MOUSE_UP;
			m.b |= MOUSE_DRAG;
			break;
		default:
			return;
	}

	if (xev->state & ShiftMask)
		m.b |= 4;
	if (xev->state & Mod4Mask) /* META */
		m.b |= 8;
	if (xev->state & ControlMask)
		m.b |= 16;

	tty->key_callback(KEYC_MOUSE, &m, tty->key_data);
}

static void 
xtmux_configure_notify(struct tty *tty, XConfigureEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	u_int sx = xev->width  / x->font_width;
	u_int sy = xev->height / x->font_height;

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
	int x2 = xev->x + xev->width  + x->font_width  - 1;
	int y2 = xev->y + xev->height + x->font_height - 1;

	xtmux_redraw(x->client, 
			xev->x / x->font_width, xev->y / x->font_height,
			x2     / x->font_width, y2     / x->font_height);
}

static void
xtmux_main(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;

	while (XPending(x->display))
	{
		XEvent xev;
		XNextEvent(x->display, &xev);

		switch (xev.type)
		{
			case KeyPress:
				xtmux_key_press(tty, &xev.xkey);
				break;

			case ButtonPress:
			case ButtonRelease:
			case MotionNotify:
				xtmux_button_press(tty, &xev.xbutton);
				break;

			case Expose:
				xtmux_expose(tty, &xev.xexpose);
				break;

			case ConfigureNotify:
				while (XCheckTypedWindowEvent(x->display, x->window, ConfigureNotify, &xev));
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

