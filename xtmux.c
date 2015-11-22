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

#include <setjmp.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#ifdef DEBUG
#include <assert.h>
#endif

#include "tmux.h"

#define XCHG(X, Y)	({ \
		__typeof__(X) _x = (X); \
		X = (Y); \
		Y = _x; \
	})

static void xtmux_main(struct tty *);
static void xt_putc_flush(struct xtmux *);
static int xt_scroll_flush(struct xtmux *);
static void xtmux_redraw(struct client *, int, int, int, int);
static void xt_expose(struct xtmux *, XExposeEvent *);

#define XTMUX_NUM_COLORS 256

static const struct colour_rgb xtmux_colors[16] = {
	{ 0,0x00,0x00,0x00}, /* black */
	{ 1,0xCC,0x00,0x00}, /* red */
	{ 2,0x00,0xCC,0x00}, /* green */
	{ 3,0xCC,0xCC,0x00}, /* yellow */
	{ 4,0x00,0x00,0xCC}, /* blue */
	{ 5,0xCC,0x00,0xCC}, /* magenta */
	{ 6,0x00,0xCC,0xCC}, /* cyan */
	{ 7,0xCC,0xCC,0xCC}, /* white */
	{ 8,0x80,0x80,0x80}, /* bright black */
	{ 9,0xFF,0x00,0x00}, /* bright red */
	{10,0x00,0xFF,0x00}, /* bright green */
	{11,0xFF,0xFF,0x00}, /* bright yellow */
	{12,0x00,0x00,0xFF}, /* bright blue */
	{13,0xFF,0x00,0xFF}, /* bright magenta */
	{14,0x00,0xFF,0xFF}, /* bright cyan */
	{15,0xFF,0xFF,0xFF}, /* bright white */
};

/* this is redundant with tty_acs_table ... */
static const unsigned short xtmux_acs[128] = {
	['+'] = 0x2192, /* RARROW */
	[','] = 0x2190, /* LARROW */
	['-'] = 0x2191, /* UARROW */
	['.'] = 0x2193, /* DARROW */
	['0'] = 0x2588, /* BLOCK */
	['`'] = 0x25C6, /* DIAMOND */
	['a'] = 0x2592, /* CKBOARD */
	['f'] = 0x00B0, /* DEGREE */
	['g'] = 0x00B1, /* PLMINUS */
	['h'] = 0x259A, /* ? BOARD */
	['i'] = 0x2603, /* ? LANTERN (snowman) */
	['j'] = 0x2518, /* LRCORNER */
	['k'] = 0x2510, /* URCORNER */
	['l'] = 0x250C, /* ULCORNER */
	['m'] = 0x2514, /* LLCORNER */
	['n'] = 0x253C, /* PLUS */
	['o'] = 0x23BA, /* S1 */
	['p'] = 0x23BB, /* S3 */
	['q'] = 0x2500, /* HLINE */
	['r'] = 0x23BC, /* S7 */
	['s'] = 0x23BD, /* S9 */
	['t'] = 0x251C, /* LTEE */
	['u'] = 0x2524, /* RTEE */
	['v'] = 0x2534, /* BTEE */
	['w'] = 0x252C, /* TTEE */
	['x'] = 0x2502, /* VLINE */
	['y'] = 0x2264, /* LEQUAL */
	['z'] = 0x2265, /* GEQUAL */
	['{'] = 0x03C0, /* PI */
	['|'] = 0x2260, /* NEQUAL */
	['}'] = 0x00A3, /* STERLING */
	['~'] = 0x00B7, /* BULLET */
};

typedef u_short wchar;

struct font {
	Font fid;
	char *name;
	u_short ascent, descent;
	wchar char_max;
	u_long *char_mask;
};

#define FONT_CHAR_OFF(N)	((N)/(8*sizeof(u_long)))
#define FONT_CHAR_BIT(N)	((u_long)1 << ((N)%(8*sizeof(u_long))))

enum font_type {
	FONT_TYPE_NONE		= -1,
	FONT_TYPE_BASE 		= 00,
	FONT_TYPE_BOLD 		= 01,
	FONT_TYPE_ITALIC 	= 02,
	FONT_TYPE_BOLD_ITALIC 	= 03,
	FONT_TYPE_COUNT
};

struct paste_ctx {
	Time time;
	struct window_pane *wp;
	char *sep;
};

#define PUTC_BUF_LEN 128

struct putc {
	u_int	x, y;
	u_short n;
	wchar	s[PUTC_BUF_LEN];
	struct grid_cell cell;
};

struct scroll {
	u_int	x, y, w, h; /* the full area being scrolled (h-n lines are copied) */
	int	n; /* number of lines to scroll, positive for down */
};

struct xtmux {
	char		*display_name;
	Display		*display;
	struct event	event;
	Window		window;
	Time		last_time;

	struct font	font[FONT_TYPE_COUNT];
	u_short		cw, ch;

	KeySym		prefix_key;
	short		prefix_mod;

	XComposeStatus	compose;
	XIM		xim;
	XIC		xic;

	GC		gc;
	unsigned long	fg, bg;
	unsigned long	colors[XTMUX_NUM_COLORS];

	GC		cursor_gc;
	Pixmap		cursor;

	unsigned	focus_out : 1;
	unsigned	cd : 1; /* 1 if cursor is drawn */
	u_int		cx, cy; /* last drawn cursor location */

	struct putc	putc_buf;
	struct scroll   scroll_buf;

	u_short		copy_active; /* outstanding XCopyArea; should be <= 1 */

	struct paste_ctx paste; /* one outstanding paste request at a time is enough */

	Cursor		pointer;

	struct client	*client; /* TODO: remove, redundant with tty->client */
	short		ioerror;
};

#define XSCREEN		DefaultScreen(x->display)
#define XCOLORMAP	DefaultColormap(x->display, XSCREEN)

#define XUPDATE()	event_active(&tty->xtmux->event, EV_WRITE, 1)

static u_int xdisplay_entry_count;
static jmp_buf xdisplay_recover;

/* One of these must be called at every entry point before an X call: */
#define XENTRY_CATCH 	if (tty->xtmux->ioerror || (!xdisplay_entry_count++ && setjmp(xdisplay_recover)))
#define XENTRY(E)	XENTRY_CATCH return E
#define XRETURN(R)	({ --xdisplay_entry_count; return R; })
#ifdef DEBUG
#define XRETURN_(R)	({ if (--xdisplay_entry_count) fatalx("xdisplay entry count mismatch"); return R; })
#else
#define	XRETURN_(R)	({ xdisplay_entry_count = 0; return R; })
#endif

#define C2W(C) 		(x->cw * (C))
#define C2H(C) 		(x->ch * (C))
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
#define OVERLAPS1(X1, L1, X2, L2) \
			(((X1) < (X2)+(L2) && (X1)+(L1) > X2))
#define OVERLAPS(X1, Y1, W1, H1, X2, Y2, W2, H2) \
			(OVERLAPS1(X1, W1, X2, W2) && OVERLAPS1(Y1, H1, Y2, H2))
#define WITHIN1(x, l, X, L) \
			((x) >= (X) && (x)+(l) <= (X)+(L)) /* INSIDE1(x, X, (L)-(l)+1) */
#define WITHIN(X1, Y1, W1, H1, X2, Y2, W2, H2) \
			(WITHIN1(X1, W1, X2, W2) && WITHIN1(Y1, H1, Y2, H2))

void
xtmux_init(struct client *c, const char *display)
{
	u_int i;
	size_t dl;
	char *path;

	c->tty.xtmux = xcalloc(1, sizeof *c->tty.xtmux);
	c->tty.xtmux->display_name = xstrdup(display);

	free(c->tty.termname);
	c->tty.termname = xstrdup("xtmux");

	/* update client environment to reflect current DISPLAY */
	environ_set(&c->environ, "DISPLAY", display);
	environ_unset(&c->environ, "WINDOWID"); /* will be set later when we know it */

	/* find a unique number to identify this client on this display, up to 999 */
	dl = strlen(display);
	path = xmalloc(dl+5);
	strcpy(path, display);
	path[dl++] = '/';
	for (i = 0; i < 999; i ++) {
		sprintf(&path[dl], "%u", i);
		if (!cmd_lookup_client(path))
			break;
	}
	free(c->tty.path);
	c->tty.path = path;

	if (!c->tty.ccolour)
		c->tty.ccolour = xstrdup("");
	
	c->tty.xtmux->client = c->tty.client = c;
}

static int
xdisplay_error(Display *disp, XErrorEvent *e)
{
	struct client *c;
	char msg[256] = "<unknown error>";

	XGetErrorText(disp, e->error_code, msg, sizeof msg-1);
	fprintf(stderr, "X11 error: %s %u,%u\n", msg, e->request_code, e->minor_code);

	TAILQ_FOREACH(c, &clients, entry) {
		if (!c || !c->tty.xtmux || c->tty.xtmux->display != disp)
			continue;
		c->flags |= CLIENT_EXIT;
	}

	return 0;
}

static int
xdisplay_ioerror(Display *disp)
{
	u_int i;
	struct client *c;

	fprintf(stderr, "X11 IO error\n");

	TAILQ_FOREACH(c, &clients, entry) {
		if (!c || !c->tty.xtmux || c->tty.xtmux->display != disp)
			continue;
		c->flags |= CLIENT_EXIT;
		c->tty.xtmux->ioerror = 1;
	}

	if ((i = xdisplay_entry_count))
	{
		xdisplay_entry_count = 0;
		longjmp(xdisplay_recover, i);
	}

	fatalx("X11 fatal error");
}

static void
xdisplay_connection_callback(int fd, short events, void *data)
{
	struct tty *tty = data;

	if (events & EV_READ)
	{
		XENTRY();
		XProcessInternalConnection(tty->xtmux->display, fd);
		xtmux_main(tty);
		XRETURN_();
	}
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
		free(ev);
	}
}

static void
xdisplay_callback(unused int fd, unused short events, void *data)
{
	struct tty *tty = (struct tty *)data;

	XENTRY();
	xtmux_main(tty);
	XRETURN_();
}

static unsigned long
xt_parse_color(struct xtmux *x, const char *s, unsigned long def)
{
	int n;
	XColor c, m;
	size_t cl;
	const char *p = s;
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
xt_fill_color(struct xtmux *x, u_int i, const struct colour_rgb *rgb)
{
	XColor c;
	c.red   = rgb->r << 8 | rgb->r;
	c.green = rgb->g << 8 | rgb->g;
	c.blue  = rgb->b << 8 | rgb->b;
	if (!XAllocColor(x->display, XCOLORMAP, &c))
		c.pixel = (i & 1) ? WhitePixel(x->display, XSCREEN) : BlackPixel(x->display, XSCREEN);
	x->colors[i] = c.pixel;
}

static void
xt_fill_colors(struct xtmux *x, const char *colors)
{
	u_int c;
	char *s, *cs, *cn;

	for (c = 0; c < 16; c ++)
		xt_fill_color(x, c, &xtmux_colors[c]);

	for (; c < 256; c ++)
		xt_fill_color(x, c, &colour_from_256[c]);

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
	free(s);
}

static void
xt_fill_cursor(struct xtmux *x, u_int cstyle)
{
	u_int w = x->cw;
	u_int h = x->ch;
	GC gc;

	if (!x->cursor)
		x->cursor = XCreatePixmap(x->display, DefaultRootWindow(x->display), w, h, 1);

	gc = XCreateGC(x->display, x->cursor, 0, NULL);
	XSetForeground(x->display, gc, 0);
	XFillRectangle(x->display, x->cursor, gc, 0, 0, w, h);
	XSetForeground(x->display, gc, 1);

	if (x->focus_out)
		cstyle >>= 4;
	cstyle &= 0xf;
	if (cstyle == 0)
		cstyle = x->focus_out ? 8 : 2;

	log_debug("creating cursor %d", cstyle);
	switch (cstyle)
	{
		/* based on http://invisible-island.net/xterm/ctlseqs/ctlseqs.html
		 * we don't (bother to) support blinking yet */
		default:
		case 1: /* block (blinking) */
		case 2: /* block (steady) */
			XFillRectangle(x->display, x->cursor, gc, 0, 0, w, h);
			break;
		case 3: /* underscore (blinking) */
		case 4: /* underscore (steady) */
			XDrawLine(x->display, x->cursor, gc, 0, h-1, w-1, h-1);
			break;
		/* xtmux extensions (leaving a bit for blink): */
		case 5: /* insert bar */
		case 6:
			XDrawLine(x->display, x->cursor, gc, 0, 0, 0, h-1);
			break;
		case 7: /* outline */
		case 8:
			XDrawRectangle(x->display, x->cursor, gc, 0, 0, w-1, h-1);
			break;
		case 9: /* bottom half */
		case 10:
			XFillRectangle(x->display, x->cursor, gc, 0, h/2, w, (h+1)/2);
			break;
		case 11: /* left half */
		case 12:
			XFillRectangle(x->display, x->cursor, gc, 0, 0, w/2, h);
			break;
		/* what else? fuzz? L? */
		case 15: /* blank */
			break;
	}

	XFreeGC(x->display, gc);
}

static char *
font_name_set(const char *f, unsigned seg, const char *r)
{
	static char out[256];
	const char *s = f, *e;
	unsigned n;
	size_t sl, el, rl;

	if (!f || *f != '-')
		return NULL;
	for (n = 1; n < seg; n ++)
		if (!(s = strchr(s+1, '-')))
			return NULL;
	if (!(e = strchr(++s, '-')))
		return NULL;
	sl = s-f;
	el = strlen(e);
	rl = strlen(r);
	if ((size_t)(e-s) == rl && !strncasecmp(s, r, rl))
		return NULL; /* no change */
	if (sl+rl+el >= sizeof(out))
		return NULL;
	if (out == f)
	{
		memmove(out+sl+rl, e, el+1);
	}
	else
	{
		memcpy(out, f, sl);
		memcpy(out+sl+rl, e, el+1);
	}
	memcpy(out+sl, r, rl);
	return out;
}

static int
xt_load_font(struct xtmux *x, enum font_type type, const char *name)
{
	struct font *font = &x->font[type];
	XFontStruct *fs;
	unsigned long nameatom;
	wchar r, c, w = 0;
	unsigned i, n;

	if (!name)
		return -1;
	fs = XLoadQueryFont(x->display, name);
	if (!fs)
	{
		fprintf(stderr, "font not found: %s\n", name);
		return -1;
	}
	if (fs->fid == font->fid)
	{
		/* no change */
		XFreeFont(x->display, fs);
		return 0;
	}
	if (font->fid)
	{
		XUnloadFont(x->display, font->fid);
		font->fid = None;
		free(font->name);
		font->name = NULL;
		font->char_max = 0;
	}
	if (!type)
	{
		x->cw = fs->max_bounds.width;
		x->ch = fs->ascent + fs->descent;
	}
	else if (x->cw != fs->max_bounds.width || 
			x->ch != fs->ascent + fs->descent)
	{
		fprintf(stderr, "font extents mismatch: %s\n", name);
		XFreeFont(x->display, fs);
		return -1;
	}
	font->fid = fs->fid;
	if (XGetFontProperty(fs, XA_FONT, &nameatom))
	{
		char *fn = XGetAtomName(x->display, nameatom);
		font->name = xstrdup(fn);
		XFree(fn);
	}
	else
		font->name = xstrdup(name);
	font->ascent = fs->ascent;
	font->descent = fs->descent;
	font->char_max = (fs->max_byte1 << 8) + fs->max_char_or_byte2;
	font->char_mask = xreallocarray(font->char_mask, FONT_CHAR_OFF(font->char_max)+1, sizeof *font->char_mask);
	memset(font->char_mask, 0, (sizeof *font->char_mask)*(FONT_CHAR_OFF(font->char_max)+1));

	i = n = 0;
	for (r = fs->min_byte1; r <= fs->max_byte1; r ++)
		for (c = fs->min_char_or_byte2; c <= fs->max_char_or_byte2; c ++)
		{
			XCharStruct *cs = &fs->per_char[i++];
			if (!fs->per_char || 
					cs->lbearing != 0 || cs->rbearing != 0 || 
					cs->width != 0 || 
					cs->ascent != 0 || cs->descent != 0)
			{
				w = (r << 8) + c;
				font->char_mask[FONT_CHAR_OFF(w)] |= FONT_CHAR_BIT(w);
				n ++;
			}
		}

	/* trim */
	font->char_max = w;
	font->char_mask = xreallocarray(font->char_mask, FONT_CHAR_OFF(font->char_max)+1, sizeof *font->char_mask);

	log_debug("font loaded with %u/%u characters: %s", n, i, font->name);
	XFreeFontInfo(NULL, fs, 1);
	return 1;
}

static void
xt_free_font(struct xtmux *x, enum font_type type)
{
	struct font *font = &x->font[type];
	if (!x->ioerror && font->fid)
		XUnloadFont(x->display, font->fid);
	font->fid = None;
	free(font->name);
	font->name = NULL;
	font->char_max = 0;
	free(font->char_mask);
	font->char_mask = NULL;
}

static inline int
xt_font_has_char(const struct xtmux *x, enum font_type type, wchar c)
{
	const struct font *font = &x->font[type];
	if (!font)
		return 0;
	if (c > font->char_max)
		return 0;
	if (font->char_mask[FONT_CHAR_OFF(c)] & FONT_CHAR_BIT(c))
		return 1;
	return 0;
}

static void
xt_size_hints(struct xtmux *x, XSizeHints *sh)
{
	memset(sh, 0, sizeof(*sh));

	sh->min_width = x->cw;
	sh->min_height = x->ch;
	sh->width_inc = x->cw;
	sh->height_inc = x->ch;
	sh->flags = PMinSize | PResizeInc;
}

static void
xt_class_hints(struct tty *tty, XClassHint *ch)
{
	struct options *o = &tty->client->options;
	memset(ch, 0, sizeof(*ch));

	ch->res_name = options_get_string(o, "xtmux-name");
	ch->res_class = (char *)"Xtmux";
}

int
xtmux_setup(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;
	struct options *o = &tty->client->options;
	const char *font, *prefix;
	KeySym pkey = NoSymbol;
	XColor pfg, pbg;

	XENTRY(-1);
	
	if (x->window)
	{
		XClassHint class_hints;
		xt_class_hints(tty, &class_hints);
		/* UTF-8? */
		XSetClassHint(x->display, x->window, &class_hints);
	}

	font = options_get_string(o, "xtmux-font");
	if (xt_load_font(x, 0, font) > 0 || 
			(!x->font->fid && xt_load_font(x, 0, "fixed") > 0))
	{
		if (x->window)
		{
			Window root;
			int xpos, ypos, border, depth;
			u_int width, height;
			XSizeHints size_hints;

			XGetGeometry(x->display, x->window, &root, &xpos, &ypos, &width, &height, &border, &depth);
			tty_set_size(tty, width/x->cw, height/x->ch);
			XClearWindow(x->display, x->window);
			recalculate_sizes();

			xt_size_hints(x, &size_hints);
			XSetWMNormalHints(x->display, x->window, &size_hints);
		}

		if (x->cursor)
		{
			XFreePixmap(x->display, x->cursor);
			x->cursor = None;
		}
		xt_fill_cursor(x, tty->cstyle);
	}
	else if (!x->font->fid)
		XRETURN(0);

	if (*(font = options_get_string(o, "xtmux-bold-font")))
		xt_load_font(x, FONT_TYPE_BOLD, font);
	else
		xt_load_font(x, FONT_TYPE_BOLD, font_name_set(x->font->name, 3, "bold"));

	if (*(font = options_get_string(o, "xtmux-italic-font")))
		xt_load_font(x, FONT_TYPE_ITALIC, font);
	else if (xt_load_font(x, FONT_TYPE_ITALIC, font_name_set(x->font->name, 4, "o")) < 0)
		xt_load_font(x, FONT_TYPE_ITALIC, font_name_set(x->font->name, 4, "i"));

	if (*(font = options_get_string(o, "xtmux-bold-italic-font")))
		xt_load_font(x, FONT_TYPE_BOLD_ITALIC, font);
	else
		xt_load_font(x, FONT_TYPE_BOLD_ITALIC, font_name_set(x->font[FONT_TYPE_ITALIC].name, 3, "bold"));

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

	if (!x->pointer)
		x->pointer = XCreateFontCursor(x->display, XC_xterm);
	if (XParseColor(x->display, XCOLORMAP, options_get_string(o, "xtmux-pointer-fg"), &pfg) &&
			XParseColor(x->display, XCOLORMAP, options_get_string(o, "xtmux-pointer-bg"), &pbg))
		XRecolorCursor(x->display, x->pointer, &pfg, &pbg);

	XRETURN(0);
}

int
xtmux_open(struct tty *tty, char **cause)
{
	struct xtmux *x = tty->xtmux;
	XWMHints wm_hints;
	XClassHint class_hints;
	XSizeHints size_hints;
	XGCValues gc_values;
	char windowid[16];

	XSetErrorHandler(xdisplay_error);
	XSetIOErrorHandler(xdisplay_ioerror);

	XENTRY_CATCH {
		xasprintf(cause, "fatal error opening X display: %s", x->display_name);
		return -1;
	}

#define FAIL(...) ({ \
		xasprintf(cause, __VA_ARGS__); \
		XRETURN(-1); \
	})

	x->display = XOpenDisplay(x->display_name);
	if (!x->display)
		FAIL("could not open X display: %s", x->display_name);

	event_set(&x->event, ConnectionNumber(x->display), EV_READ|EV_PERSIST, xdisplay_callback, tty);
	if (event_add(&x->event, NULL) < 0)
		fatal("failed to add X display event");

	if (!XAddConnectionWatch(x->display, &xdisplay_connection_watch, (XPointer)tty))
		FAIL("could not get X display connection: %s", x->display_name);

	if (xtmux_setup(tty))
		FAIL("failed to setup X display");

	if (!x->font->fid)
		FAIL("could not load X font");

	if (!tty->sx)
		tty->sx = 80;
	if (!tty->sy)
		tty->sx = 24;

	x->window = XCreateSimpleWindow(x->display, DefaultRootWindow(x->display),
			0, 0, C2W(tty->sx), C2H(tty->sy),
			0, 0, x->bg);
	if (x->window == None)
		FAIL("could not create X window");

	snprintf(windowid, sizeof(windowid), "%u", (unsigned)x->window);
	environ_set(&tty->client->environ, "WINDOWID", windowid);

	x->xim = XOpenIM(x->display, NULL, NULL, NULL);
	if (x->xim)
		x->xic = XCreateIC(x->xim, XNInputStyle, XIMPreeditNone | XIMStatusNone,
				XNClientWindow, x->window,
				XNFocusWindow, x->window,
				NULL);
	if (!x->xic)
	{
		fprintf(stderr, "xtmux: failed to initialize input method\n");
		if (x->xim)
			XCloseIM(x->xim);
		x->xim = 0;
	}

	xt_size_hints(x, &size_hints);
	size_hints.win_gravity = NorthWestGravity;
	size_hints.flags |= PWinGravity;
	wm_hints.input = True;
	wm_hints.initial_state = NormalState;
	wm_hints.flags = InputHint | StateHint;
	xt_class_hints(tty, &class_hints);
	Xutf8SetWMProperties(x->display, x->window, class_hints.res_name, class_hints.res_name, NULL, 0, &size_hints, &wm_hints, &class_hints);

	gc_values.foreground = x->fg;
	gc_values.background = x->bg;
	gc_values.font = x->font->fid;
	gc_values.graphics_exposures = True;
	x->gc = XCreateGC(x->display, x->window, GCForeground | GCBackground | GCFont | GCGraphicsExposures, &gc_values);

	gc_values.foreground = WhitePixel(x->display, XSCREEN);
	gc_values.background = BlackPixel(x->display, XSCREEN);
	gc_values.function = GXxor; /* this'll be fine for TrueColor, etc, but we might want to avoid PseudoColor for this */
	gc_values.graphics_exposures = False;
	x->cursor_gc = XCreateGC(x->display, x->window, GCFunction | GCForeground | GCBackground | GCGraphicsExposures, &gc_values);
	
	XDefineCursor(x->display, x->window, x->pointer);

	XSelectInput(x->display, x->window, KeyPressMask | ExposureMask | FocusChangeMask | StructureNotifyMask | ButtonPressMask | ButtonReleaseMask | ButtonMotionMask);

	XMapWindow(x->display, x->window);

	tty->flags |= TTY_OPENED | TTY_UTF8;

#undef FAIL
	XUPDATE();
	XRETURN_(0);
}

void 
xtmux_close(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;
	enum font_type ft;

	tty->flags &= ~TTY_OPENED;

	event_del(&x->event);

	/* Must be careful here if we got an IO error: 
	 * want to free resources without using the connection */

	if (x->xic)
	{
		XDestroyIC(x->xic);
		x->xic = NULL;
	}

	if (x->xim)
	{
		XCloseIM(x->xim);
		x->xim = 0;
	}

	if (x->paste.sep)
	{
		free(x->paste.sep);
		x->paste.sep = NULL;
	}

	if (x->pointer)
	{
		if (!x->ioerror)
			XFreeCursor(x->display, x->pointer);
		x->pointer = None;
	}

	if (x->cursor)
	{
		XFreePixmap(x->display, x->cursor);
		x->cursor = None;
	}

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
		if (!x->ioerror)
			XFreeColors(x->display, XCOLORMAP, x->colors, XTMUX_NUM_COLORS, 0);
		XDestroyWindow(x->display, x->window);
		x->window = None;
	}

	for (ft = 0; ft < FONT_TYPE_COUNT; ft ++)
		xt_free_font(x, ft);

	if (x->display != NULL)
	{
		int fd = ConnectionNumber(x->display);
		XCloseDisplay(x->display);
		x->display = NULL;
		/* if ioerror, sometimes connection is left open; should be safe regardless */
		close(fd);
	}
}

void 
xtmux_free(struct tty *tty)
{
	free(tty->xtmux->display_name);
	free(tty->xtmux);
}

void
xtmux_set_title(struct tty *tty, const char *title)
{
	struct xtmux *x = tty->xtmux;

	if (!x->window)
		return;

	XENTRY();
	XChangeProperty(x->display, x->window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, title, strlen(title));
	XRETURN();
}

static inline int
grid_attr_cmp(const struct grid_cell *a, const struct grid_cell *b)
{
	/* could be more aggressive here with UTF8 and ' ', but good enough for now */
	return !(a->attr == b->attr && 
			a->flags == b->flags && 
			a->fg == b->fg && 
			a->bg == b->bg);
}

static u_int
grid_char(const struct grid_cell *gc)
{
	struct utf8_data ud;

	if (gc->flags & GRID_FLAG_PADDING)
		return ' ';
	grid_cell_get(gc, &ud);
	/* XXX does ud.width matter? 0-width characters seem messed up */
	if (ud.size != 1)
		return utf8_combine(&ud);
	return *ud.data;
}

void
xtmux_attributes(struct tty *tty, const struct grid_cell *gc)
{
	if (!grid_attr_cmp(&tty->cell, gc))
		return;

	XENTRY();
	tty->cell = *gc;
	XRETURN();
}

void
xtmux_reset(struct tty *tty)
{
	xtmux_attributes(tty, &grid_default_cell);
}

static inline int
xt_redraw_pending(struct xtmux *x, u_int cx, u_int cy)
{
	struct scroll *s = &x->scroll_buf;

	return s->n && INSIDE(cx, cy, s->x, s->y, s->w, s->h) &&
		(s->n < 0 ? (int)cy >= (int)(s->y+s->h)+s->n : cy < s->y+s->n);
}

static inline void
xt_flush(struct xtmux *x, u_int px, u_int py, u_int w, u_int h)
{
	struct putc *b = &x->putc_buf;

	if (b->n && INSIDE1(b->y, py, h) && OVERLAPS1(b->x, b->n, px, w))
		xt_putc_flush(x);
}

/* make sure the given region is up-to-date */
static int
xt_touch(struct xtmux *x, u_int px, u_int py, u_int w, u_int h)
{
	struct scroll *s = &x->scroll_buf;

	if (s->n && OVERLAPS(px, py, w, h, s->x, s->y, s->w, s->h))
	{
		if (s->n < 0 ? (int)py < (int)(s->y+s->h)+s->n : py+h > s->y+s->n)
			xt_scroll_flush(x); /* includes copy region */
		else if (WITHIN(px, py, w, h, s->x, s->y, s->w, s->h))
			return 0; /* within invalidated region */
	}

	xt_flush(x, px, py, w, h);
	return 1;
}

/* indicate an intention to completely overwrite a region */
static int
xt_write(struct xtmux *x, u_int px, u_int py, u_int w, u_int h, int c)
{
	struct putc *b = &x->putc_buf;
	struct scroll *s = &x->scroll_buf;

	if (b->n && WITHIN(b->x, b->y, b->n, 1, px, py, w, h))
		b->n = 0;
	if (s->n && WITHIN(s->x, s->y, s->w, s->h, px, py, w, h))
		s->n = 0;
	if (!xt_touch(x, px, py, w, h))
		return 0;
	if (!INSIDE(x->cx, x->cy, px, py, w, h))
		return 1;
	/* the cursor is a special, as it may be drawn/erased before exposure events */
	if (c)
		XClearArea(x->display, x->window, C2X(x->cx), C2Y(x->cy), C2W(1), C2H(1), False);
	x->cd = 0;
	return 2;
}

static int
xt_clear(struct xtmux *x, u_int cx, u_int cy, u_int w, u_int h)
{
	if (!xt_write(x, cx, cy, w, h, 0))
		return 0;
	XClearArea(x->display, x->window, C2X(cx), C2Y(cy), C2W(w), C2H(h), False);
	return 1;
}

static void
xt_redraw(struct xtmux *x, u_int cx, u_int cy, u_int w, u_int h)
{
	if (xt_clear(x, cx, cy, w, h))
		xtmux_redraw(x->client, cx, cy, cx+w, cy+h);
}

static inline int
xt_put_cursor(struct xtmux *x)
{
	if (!x->cd)
		return 0;

	XCopyPlane(x->display, x->cursor, x->window, x->cursor_gc, 0, 0, x->cw, x->ch, C2X(x->cx), C2Y(x->cy), 1);
	return 1;
}

static int
xt_clear_cursor(struct xtmux *x)
{
	int r = 0;

	if (!x->cd)
		return 0;

	r |= xt_put_cursor(x);
	x->cd = 0;
	return r;
}

static int
xt_move_cursor(struct xtmux *x, u_int cx, u_int cy)
{
	int p = 0, r = 0;

	p = xt_redraw_pending(x, cx, cy);
	if (!p && x->cd && x->cx == cx && x->cy == cy)
		return r;
	r |= xt_clear_cursor(x);
	if (p)
		return r;

	xt_flush(x, cx, cx, 1, 1);
	x->cx = cx; x->cy = cy;
	x->cd = 1;
	r |= xt_put_cursor(x);
	return r;
}

static inline int
xtmux_update_cursor(struct tty *tty)
{
	return (tty->mode & MODE_CURSOR) && xt_move_cursor(tty->xtmux, tty->cx, tty->cy);
}

static void
xt_do_copy(struct xtmux *x, u_int x1, u_int y1, u_int x2, u_int y2, u_int w, u_int h)
{
	while (x->copy_active)
	{
		XEvent xev;
		XSync(x->display, False);
		if (XCheckTypedWindowEvent(x->display, x->window, GraphicsExpose, &xev))
			xt_expose(x, &xev.xexpose);
		else if (XCheckTypedWindowEvent(x->display, x->window, NoExpose, &xev))
			x->copy_active --;
		else {
			/* this should not happen;
			 * however, if it does, we can't copy cleanly.
			 * instead, just clear and refresh (poorly), and hope
			 * the proper event comes in later if it matters
			 */
			fprintf(stderr, "didn't get expected expose event; redrawing\n");
			xt_redraw(x, x2, y2, w, h);
			return;
		}
	}

	if (x->cd)
	{
		if (INSIDE(x->cx, x->cy, x1, y1, w, h))
			xt_clear_cursor(x);
		else if (INSIDE(x->cx, x->cy, x2, y2, w, h))
			x->cd = 0;
	}
	x->copy_active ++;
	XCopyArea(x->display, x->window, x->window, x->gc,
			C2X(x1), C2Y(y1), C2W(w), C2H(h), C2X(x2), C2Y(y2));
}

static int
xt_scroll_flush(struct xtmux *x)
{
	struct scroll *s = &x->scroll_buf;
	int n = s->n;

	if (!n)
		return 0;

	s->n = 0;
	if (n < 0)
	{	/* up */
		n = -n;
		if (s->h > (u_int)n)
			xt_do_copy(x, s->x, s->y+n, s->x, s->y, s->w, s->h-n);
		else
			n = s->h;
		xt_redraw(x, s->x, s->y+s->h-n, s->w, n);
	}
	else
	{ 	/* down */
		if (s->h > (u_int)n)
			xt_do_copy(x, s->x, s->y, s->x, s->y+n, s->w, s->h-n);
		else
			n = s->h;
		xt_redraw(x, s->x, s->y, s->w, n);
	}

	return 1;
}

static void
xt_scroll(struct xtmux *x, u_int sx, u_int sy, u_int w, u_int h, int n)
{
	struct scroll *s = &x->scroll_buf;

	log_debug("xt_scroll %u,%u %ux%u %d", sx, sy, w, h, n);

	if (s->n && (s->x != sx || s->y != sy || s->w != w || s->h != h || (s->n ^ n) < 0))
		xt_scroll_flush(x);

	xt_flush(x, sx, sy, w, h);

	s->x = sx;
	s->y = sy;
	s->w = w;
	s->h = h;
	s->n += n;
}

static void
xt_copy(struct xtmux *x, u_int x1, u_int y1, u_int x2, u_int y2, u_int w, u_int h)
{
	if (!xt_touch(x, x1, y1, w, h))
		return;

	return xt_do_copy(x, x1, y1, x2, y2, w, h);
}

void
xtmux_cursor(struct tty *tty, u_int cx, u_int cy)
{
	XENTRY();

	tty->cx = cx;
	tty->cy = cy;

	if (xtmux_update_cursor(tty))
		XUPDATE();

	XRETURN();
}

void
xtmux_force_cursor_colour(struct tty *tty, const char *ccolour)
{
	struct xtmux *x = tty->xtmux;
	unsigned long c;
	
	XENTRY();
	/* We draw cursor with xor, so xor with background to get right color, defaulting to inverse */
	c = WhitePixel(x->display, XSCREEN);
	c = xt_parse_color(x, ccolour, x->bg ^ c);
	log_debug("setting cursor color to %s = %lx", ccolour, c);
	xt_clear_cursor(x);
	XSetForeground(x->display, x->cursor_gc, x->bg ^ c);
	XRETURN_();
}

void
xtmux_update_mode(struct tty *tty, int mode, struct screen *s)
{
	struct xtmux *x = tty->xtmux;
	int up = 0;

	XENTRY();

	if (s && tty->cstyle != s->cstyle)
	{
		xt_clear_cursor(x);
		tty->cstyle = s->cstyle;
		xt_fill_cursor(x, tty->cstyle);
	}

	if (mode & MODE_CURSOR)
		up = xt_move_cursor(x, tty->cx, tty->cy);
	else if (s && !(s->mode & MODE_CURSOR))
		up = xt_clear_cursor(x);

	tty->mode = mode;
	if (up) XUPDATE();
	XRETURN();
}

static enum font_type
xt_font_pick(const struct xtmux *x, enum font_type type, wchar c)
{
	if (c == ' ')
		return FONT_TYPE_NONE;
	if (!type || xt_font_has_char(x, type, c))
		return type;
	if (type == FONT_TYPE_BOLD_ITALIC)
	{
		if (xt_font_has_char(x, FONT_TYPE_ITALIC, c))
			return FONT_TYPE_ITALIC;
		if (xt_font_has_char(x, FONT_TYPE_BOLD, c))
			return FONT_TYPE_BOLD;
	}
	if (xt_font_has_char(x, FONT_TYPE_BASE, c))
		return FONT_TYPE_BASE;
	return FONT_TYPE_NONE;
}

static void
xt_draw_chars(struct xtmux *x, u_int cx, u_int cy, const wchar *cp, size_t n, const struct grid_cell *gc, int cleared)
{
	u_int			i, px = C2X(cx), py = C2Y(cy), wx = C2W(n), hy = C2H(1);
	u_char			fgc = gc->fg, bgc = gc->bg;
	unsigned long		fg, bg;
	enum font_type 		ft;

	if (gc->flags & GRID_FLAG_PADDING)
		return;

	switch (xt_write(x, cx, cy, n, 1, 0)) {
		case 0: return;
		case 2: cleared = 0;
	}

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
		XCHG(fg, bg);
		XCHG(fgc, bgc);
	}

	if (gc->attr & GRID_ATTR_ITALICS && gc->attr & GRID_ATTR_BRIGHT && 
			x->font[ft = FONT_TYPE_BOLD_ITALIC].fid);
	else if (gc->attr & GRID_ATTR_ITALICS && 
			x->font[ft = FONT_TYPE_ITALIC].fid);
	else if (gc->attr & GRID_ATTR_BRIGHT && 
			x->font[ft = FONT_TYPE_BOLD].fid);
	else ft = 0;

	/* TODO: configurable BRIGHT semantics */
	if (gc->attr & GRID_ATTR_BRIGHT && fgc < 8 && (fg == bg || !(ft & FONT_TYPE_BOLD)))
		fg = x->colors[fgc += 8];

	/* TODO: DIM, maybe only for TrueColor? */

	for (i = 0; i < n; i ++)
		if (cp[i] != ' ')
			break;

	if (i == n || gc->attr & GRID_ATTR_HIDDEN)
	{
		if (bg == x->bg)
		{
			if (!cleared)
				XClearArea(x->display, x->window, px, py, wx, hy, False);
		}
		else
		{
			XSetForeground(x->display, x->gc, bg);
			XFillRectangle(x->display, x->window, x->gc, px, py, wx, hy);
		}
	}
	else
	{
		XChar2b c2[n];
		u_int l = 0;
		enum font_type ftl = ft, ftc;
		
		if (gc->attr & GRID_ATTR_ITALICS && !(ft & FONT_TYPE_ITALIC))
			XCHG(fg, bg);
		XSetForeground(x->display, x->gc, fg);

		for (l = 0; l < n; l = i)
		{
			for (i = l; i < n; i ++)
			{
				wchar c = cp[i];

				if (gc->attr & GRID_ATTR_CHARSET) 
				{
					if (c >= '`' && c <= '~' && xt_font_pick(x, ft, c-('`'-1)) != FONT_TYPE_NONE)
						c -= '`'-1;
					else if (c < nitems(xtmux_acs) && xtmux_acs[c])
						c = xtmux_acs[c];
				}

				ftc = xt_font_pick(x, ft, c);
				if (ftc != FONT_TYPE_NONE && ftc != ftl)
				{
					if (i == l)
						ftl = ftc;
					else
						break;
				}

				c2[i].byte1 = c >> 8;
				c2[i].byte2 = c;
			}

			XSetFont(x->display, x->gc, x->font[ftl].fid);
			if (cleared && bg == x->bg)
				XDrawString16(x->display, x->window, x->gc, C2X(cx+l), py + x->font[ftl].ascent, &c2[l], i-l);
			else
			{
				XSetBackground(x->display, x->gc, bg);
				XDrawImageString16(x->display, x->window, x->gc, C2X(cx+l), py + x->font[ftl].ascent, &c2[l], i-l);
			}

			ftl = ftc;
		}
	}

	/* UNDERSCORE xor BLINK */
	if ((gc->attr & (GRID_ATTR_UNDERSCORE | GRID_ATTR_BLINK)) == GRID_ATTR_UNDERSCORE ||
			(gc->attr & (GRID_ATTR_UNDERSCORE | GRID_ATTR_BLINK)) == GRID_ATTR_BLINK)
	{
		u_int y = py + x->font[ft].ascent;
		if (x->font[ft].descent > 1)
			y ++;
		XSetForeground(x->display, x->gc, fg);
		XDrawLine(x->display, x->window, x->gc, 
				px, y,
				px + wx - 1, y);
	}
	if (gc->attr & GRID_ATTR_BLINK)
	{
		/* a little odd but blink is weird anyway */
		XDrawLine(x->display, x->window, x->gc,
				px, py,
				px + wx - 1, py);
	}
}

static void
xt_draw_cells(struct xtmux *x, u_int cx, u_int cy, const struct grid_cell *gc, size_t n, const struct grid_cell *ga)
{
	u_int i;
	wchar c[n];

	if (!n)
		return;

	for (i = 0; i < n; i ++)
		c[i] = grid_char(&gc[i]);

	xt_draw_chars(x, cx, cy, c, n, ga, !(x->cd && INSIDE(x->cx, x->cy, cx, cy, n, 1)));
}

static void
xt_putc_flush(struct xtmux *x)
{
	struct putc *b = &x->putc_buf;
	u_int n = b->n;

	if (!n)
		return;

	b->n = 0;
	xt_draw_chars(x, b->x, b->y, b->s, n, &b->cell, 0);
}

static void
xtmux_putwc(struct tty *tty, u_int c)
{
	struct xtmux *x = tty->xtmux;
	struct putc *b = &x->putc_buf;

	XENTRY();

	if (tty->cx >= tty->sx)
	{
		xt_putc_flush(x);
		tty->cx = 0;
		if (tty->cy != tty->rlower)
			tty->cy ++;
	}

	if (xt_redraw_pending(x, tty->cx, tty->cy))
		XRETURN();

	if (b->n &&
			(b->n == PUTC_BUF_LEN ||
			 b->x+b->n != tty->cx || 
			 b->y != tty->cy ||
			 grid_attr_cmp(&b->cell, &tty->cell)))
		xt_putc_flush(x);

	if (!b->n)
	{
		b->x = tty->cx;
		b->y = tty->cy;
		memcpy(&b->cell, &tty->cell, sizeof tty->cell);
	}
	b->s[b->n++] = c;

	XUPDATE();
	XRETURN();
}

void
xtmux_putc(struct tty *tty, u_char c)
{
	if (c >= 0x20 && c != 0x7f)
		xtmux_putwc(tty, c);
}

void
xtmux_pututf8(struct tty *tty, const struct utf8_data *gu)
{
	return xtmux_putwc(tty, utf8_combine(gu));
}

void
xtmux_cmd_insertcharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dx = ctx->ocx + ctx->num;

	XENTRY();

	xt_copy(x,
			PANE_CX, PANE_CY,
			PANE_X(dx), PANE_CY,
			screen_size_x(s) - dx, 1);
	xt_clear(x,
			PANE_CX, PANE_CY,
			ctx->num, 1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_deletecharacter(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int dx = ctx->ocx + ctx->num;

	XENTRY();

	xt_copy(x,
			PANE_X(dx), PANE_CY, 
			PANE_CX, PANE_CY,
			screen_size_x(s) - dx, 1);
	xt_clear(x,
			PANE_X(screen_size_x(s) - ctx->num), PANE_CY,
			ctx->num, 1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_insertline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	xt_scroll(x,
			PANE_X(0), PANE_CY,
			screen_size_x(s), ctx->orlower+1-ctx->ocy,
			ctx->num);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_deleteline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	xt_scroll(x,
			PANE_X(0), PANE_CY,
			screen_size_x(s), ctx->orlower+1-ctx->ocy,
			-ctx->num);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_clearline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	xt_clear(x,
			PANE_X(0), PANE_CY,
			screen_size_x(s), 1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_clearendofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	xt_clear(x,
			PANE_CX, PANE_CY,
			screen_size_x(s) - ctx->ocx, 1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_clearstartofline(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;

	XENTRY();

	xt_clear(x,
			PANE_X(0), PANE_CY,
			ctx->ocx + 1, 1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_reverseindex(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	/* same as insertline(1) at top */
	xt_scroll(x,
			PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), ctx->orlower+1-ctx->orupper,
			1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_linefeed(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	/* same as deleteline(1) at top */
	xt_scroll(x,
			PANE_X(0), PANE_Y(ctx->orupper),
			screen_size_x(s), ctx->orlower+1-ctx->orupper,
			-1);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_clearendofscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int y;

	XENTRY();

	y = ctx->ocy;
	if (ctx->ocx > 0)
	{
		if (ctx->ocx < screen_size_x(s))
			xt_clear(x,
					PANE_CX, PANE_CY,
					screen_size_x(s) - ctx->ocx, 1);
		y ++;
	}
	if (y < screen_size_y(s))
		xt_clear(x,
				PANE_X(0), PANE_Y(y),
				screen_size_x(s), screen_size_y(s) - y);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_clearstartofscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;
	u_int y;

	XENTRY();

	y = ctx->ocy;
	if (ctx->ocx < screen_size_x(s))
		xt_clear(x,
				PANE_X(0), PANE_CY,
				ctx->ocx + 1, 1);
	else
		y ++;
	if (y > 0)
		xt_clear(x,
				PANE_X(0), PANE_Y(0),
				screen_size_x(s), y);

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_clearscreen(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;
	struct screen		*s = ctx->wp->screen;

	XENTRY();

	xt_clear(x,
			PANE_X(0), PANE_Y(0),
			screen_size_x(s), screen_size_y(s));

	XUPDATE();
	XRETURN();
}

void
xtmux_cmd_setselection(struct tty *tty, const struct tty_ctx *ctx)
{
	struct xtmux 		*x = tty->xtmux;

	XENTRY();

	XSetSelectionOwner(x->display, XA_PRIMARY, x->window, CurrentTime /* XXX */);
	if (XGetSelectionOwner(x->display, XA_PRIMARY) != x->window)
		XRETURN();

	XChangeProperty(x->display, DefaultRootWindow(x->display),
			XA_CUT_BUFFER0, XA_STRING, 8, PropModeReplace, ctx->ptr, ctx->num);

	XRETURN();
}

static void 
xtmux_selection_request(struct tty *tty, XSelectionRequestEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	XSelectionEvent r;
	struct paste_buffer *pb;
	const char *pbdata = NULL;
	size_t pbsize;

	if (xev->owner != x->window || xev->selection != XA_PRIMARY)
		return;

	if (xev->property == None)
		xev->property = xev->target;

	r.type = SelectionNotify;
	r.display = xev->display;
	r.requestor = xev->requestor;
	r.selection = xev->selection;
	r.target = xev->target;
	r.time = xev->time;
	r.property = None;

	if ((pb = paste_get_top(NULL)))
		pbdata = paste_buffer_data(pb, &pbsize);

	if (xev->target == XA_STRING)
	{
		if (pbdata && XChangeProperty(x->display, r.requestor, xev->property, 
					XA_STRING, 8, PropModeReplace, pbdata, pbsize))
			r.property = xev->property;
	}
	else
	{
		char *target = XGetAtomName(x->display, xev->target);
		if (!strcmp(target, "TARGETS"))
		{
			/* silly, but easy enough */
			Atom targets[] = { XA_STRING, xev->target };

			if (XChangeProperty(x->display, r.requestor, xev->property, XA_ATOM, 32, PropModeReplace, 
						(unsigned char *)targets, nitems(targets)))
				r.property = xev->property;
		}
		else if (!strcmp(target, "TEXT"))
		{
			if (pbdata && XChangeProperty(x->display, r.requestor, xev->property, 
						XA_STRING, 8, PropModeReplace, pbdata, pbsize))
				r.property = xev->property;
		}
		XFree(target);
	}

	XSendEvent(x->display, r.requestor, False, 0, (XEvent *)&r);
}

/* cheat a little: */
extern void cmd_paste_buffer_filter(struct window_pane *, const char *, size_t, const char *, int bracket);

static void
do_paste(const struct paste_ctx *p, const char *data, size_t size)
{
	paste_send_pane(data, size, p->wp, p->sep, 0);
}

static int 
xt_paste_property(struct xtmux *x, Window w, Atom p)
{
	XTextProperty t;

	if (!XGetTextProperty(x->display, w, &t, p) || !t.value || t.format != 8)
	{
		fprintf(stderr, "could not get text property to paste\n");
		return -1;
	}

	log_debug("pasting %lu characters", t.nitems);
	do_paste(&x->paste, t.value, t.nitems);

	x->paste.time = 0;
	x->paste.wp = NULL;
	free(x->paste.sep);
	x->paste.sep = NULL;
	XFree(t.value);

	return 0;
}

enum cmd_retval
xtmux_paste(struct tty *tty, struct window_pane *wp, const char *which, const char *sep)
{
	struct xtmux *x = tty->xtmux;
	Atom s = None;

	XENTRY(CMD_RETURN_ERROR);

	if (!which || !strncasecmp(which, "primary", strlen(which)))
		s = XA_PRIMARY;
	else if (!strncasecmp(which, "secondary", strlen(which)))
		s = XA_SECONDARY;
	else if (!strncasecmp(which, "clipboard", strlen(which)))
		s = XInternAtom(x->display, "CLIPBOARD", True);
	else {
		const char *e;
		s = XA_CUT_BUFFER0 + strtonum(which, 0, 7, &e);
		if (e)
			s = None;
	}

	if (s == None)
		XRETURN(CMD_RETURN_ERROR);

	x->paste.time = x->last_time;
	x->paste.wp = wp;
	free(x->paste.sep);
	if (sep)
		x->paste.sep = xstrdup(sep);
	else
		x->paste.sep = NULL;

	if (s >= XA_CUT_BUFFER0 && s <= XA_CUT_BUFFER7) {
		if (xt_paste_property(x, DefaultRootWindow(x->display), s))
			XRETURN(CMD_RETURN_ERROR);
		XRETURN(CMD_RETURN_NORMAL);
	}

	if (XGetSelectionOwner(x->display, s) == x->window) 
	{
		/* short cut */
		struct paste_buffer *pb = paste_get_top(NULL);
		if (pb) {
			size_t size;
			const char *data = paste_buffer_data(pb, &size);
			do_paste(&x->paste, data, size);
		}

		x->paste.time = 0;
		x->paste.wp = NULL;
		free(x->paste.sep);
		x->paste.sep = NULL;
		XRETURN(CMD_RETURN_NORMAL);
	}

	if (XConvertSelection(x->display, s, XA_STRING, XA_STRING, x->window, x->paste.time))
		XRETURN(CMD_RETURN_ERROR);
	XRETURN(CMD_RETURN_NORMAL);
}

static void
xtmux_selection_notify(struct tty *tty, XSelectionEvent *xev)
{
	struct xtmux 		*x = tty->xtmux;
	struct session		*s;
	struct winlink		*wl;
	struct window_pane	*wp;

	if (!(xev->requestor == x->window && 
				x->paste.wp &&
				xev->time == x->paste.time &&
				xev->target == XA_STRING &&
				xev->property == XA_STRING))
		return;

	/* need to make sure pane is still valid */
	RB_FOREACH(s, sessions, &sessions)
		RB_FOREACH(wl, winlinks, &s->windows)
			TAILQ_FOREACH(wp, &wl->window->panes, entry)
				if (wp == x->paste.wp)
					goto found;

	log_debug("paste target pane disappeared");
	x->paste.wp = NULL;
	return;

found:
	if (xt_paste_property(x, xev->requestor, xev->property) == 0)
		XDeleteProperty(x->display, xev->requestor, xev->property);
}

void
xtmux_bell(struct tty *tty)
{
	XENTRY();
	XBell(tty->xtmux->display, 100);
	XUPDATE();
	XRETURN();
}

static void
xt_draw_line(struct xtmux *x, struct screen *s, u_int py, u_int left, u_int right, u_int ox, u_int oy)
{
	const struct grid_line *gl = &s->grid->linedata[s->grid->hsize+py];
	struct grid_cell ga;
	u_int sx = right;
	u_int i, b;

	if (sx > gl->cellsize)
		sx = gl->cellsize;
	for (i = left, b = left; i < sx; i ++)
	{
		struct grid_cell gc = gl->celldata[i];

		if (screen_check_selection(s, i, py)) {
			gc.attr = s->sel.cell.attr;
			gc.flags &= ~(GRID_FLAG_FG256|GRID_FLAG_BG256);
			gc.flags |= s->sel.cell.flags & (GRID_FLAG_FG256|GRID_FLAG_BG256);
			gc.fg = s->sel.cell.fg;
			gc.bg = s->sel.cell.bg;
		}

		if (i == b || grid_attr_cmp(&gc, &ga))
		{
			xt_draw_cells(x, ox+b, oy+py, &gl->celldata[b], i-b, &ga);
			b = i;
			ga = gc;
		}
	}
	xt_draw_cells(x, ox+b, oy+py, &gl->celldata[b], i-b, &ga);
}

void
xtmux_draw_line(struct tty *tty, struct screen *s, u_int py, u_int ox, u_int oy)
{
	u_int sx;

	XENTRY();

	sx = screen_size_x(s);
	if (sx > tty->sx)
		sx = tty->sx;

	if (xt_clear(tty->xtmux, ox, oy+py, sx, 1))
	{
		xt_draw_line(tty->xtmux, s, py, 0, sx, ox, oy);
		XUPDATE();
	}

	XRETURN();
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
	else if ((u_int)left >= wp->sx)
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
	struct window_pane *wp;

	if (!c->session)
		return;

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

	TAILQ_FOREACH(wp, &c->session->curw->window->panes, entry)
		xtmux_redraw_pane(tty, wp, left, top, right, bot);

	/* TODO: borders, numbers */

	if (INSIDE(tty->cx, tty->cy, left, top, right-left, bot-top))
		xtmux_update_cursor(tty);
}

static void
xtmux_key_press(struct tty *tty, XKeyEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	int r, i, key;
	static unsigned char buf[32];
	KeySym xks = 0;

	if (x->xic)
		r = Xutf8LookupString(x->xic, xev, buf, sizeof buf, &xks, NULL);
	else
		r = XLookupString(xev, buf, sizeof buf, &xks, &x->compose);
	if (r > (int)sizeof buf)
		log_fatalx("FIXME: xtmux LookupString result too large for buffer: %d", r);
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
		/*
		case XK_F13: 		key = KEYC_F13;  	break;
		case XK_F14: 		key = KEYC_F14;  	break;
		case XK_F15: 		key = KEYC_F15;  	break;
		case XK_F16: 		key = KEYC_F16;  	break;
		case XK_F17: 		key = KEYC_F17;  	break;
		case XK_F18: 		key = KEYC_F18;  	break;
		case XK_F19: 		key = KEYC_F19;  	break;
		case XK_F20: 		key = KEYC_F20;  	break;
		*/
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
		server_client_key_table(tty->client, "prefix");

	if (r < 0)
		server_client_handle_key(tty->client, key);
	else for (i = 0; i < r; i ++)
		server_client_handle_key(tty->client, key | buf[i]);
}

static void
xtmux_button_press(struct tty *tty, XButtonEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	struct mouse_event *m = &tty->mouse;
	int prefix;

	m->lx = m->x;
	m->ly = m->y;
	m->x = xev->x / x->cw;
	m->y = xev->y / x->ch;
	prefix = xev->state & (x->prefix_mod >= 0 ? 1<<x->prefix_mod : ShiftMask);

	switch (xev->type) {
		case ButtonPress:
			switch (xev->button)
			{
				case Button1: m->b = 0; break;
				case Button2: m->b = 1; break;
				case Button3: m->b = 2; break;
				case Button4: m->b = MOUSE_MASK_WHEEL|0; break;
				case Button5: m->b = MOUSE_MASK_WHEEL|1; break;
				default: return;
			}
			break;
		case ButtonRelease:
			m->b = 3;
			break;
		case MotionNotify:
			if (!prefix && !(tty->mode & (MODE_MOUSE_BUTTON /* | MODE_MOUSE_ANY */)))
				return;
			if (m->x == m->lx && m->y == m->ly)
				return;
			m->b = MOUSE_MASK_DRAG;
			if (xev->state & Button1Mask)
				m->b |= 0;
			else if (xev->state & Button2Mask)
				m->b |= 1;
			else if (xev->state & Button3Mask)
				m->b |= 2;
			else
			{
				/* if (!(tty->mode & MODE_MOUSE_ANY)) */
					return;
				m->b |= 3;
			}
			break;
		default:
			return;
	}

	if (xev->state & ShiftMask)
		m->b |= MOUSE_MASK_SHIFT;
	if (xev->state & Mod4Mask) /* META */
		m->b |= MOUSE_MASK_META;
	if (xev->state & ControlMask)
		m->b |= MOUSE_MASK_CTRL;

	if (prefix)
		server_client_key_table(tty->client, "prefix");

	server_client_handle_key(tty->client, KEYC_MOUSE);
}

static void 
xtmux_configure_notify(struct tty *tty, XConfigureEvent *xev)
{
	struct xtmux *x = tty->xtmux;
	u_int sx = xev->width  / x->cw;
	u_int sy = xev->height / x->ch;

	if (sx != tty->sx || sy != tty->sy)
	{
		tty->sx = sx;
		tty->sy = sy;
		xtmux_cursor(tty, 0, 0);
		recalculate_sizes();
	}
}

static void
xt_expose(struct xtmux *x, XExposeEvent *xev)
{
	int px1 = xev->x;
	int py1 = xev->y;
	int cx1 = px1 / x->cw;
	int cy1 = py1 / x->ch;
	int px2 = px1 + xev->width;
	int py2 = py1 + xev->height;
	int cx2 = (px2 + x->cw  - 1) / x->cw;
	int cy2 = (py2 + x->ch - 1) / x->ch;

	if (xev->type == GraphicsExpose && !xev->count && x->copy_active)
		x->copy_active --;

	if (!xt_write(x, cx1, cy1, cx2-cx1, cy2-cy1, 1))
		return;

	/* extend exposed area out to character borders so we can redraw */
	#define CLEAR(X1, X2, Y1, Y2) \
		XClearArea(x->display, x->window, X1, Y1, (X2)-(X1), (Y2)-(Y1), False)
	if  	     (C2X(cx1) < px1)
		CLEAR(C2X(cx1),  px1, C2Y(cy1), C2Y(cy2));
	if                           (C2Y(cy1) < py1)
		CLEAR(px1,  C2X(cx2), C2Y(cy1),  py1);
	if           (px2 < C2X(cx2))
		CLEAR(px2,  C2X(cx2), py1,  C2Y(cy2));
	if                           (py2 < C2Y(cy2))
		CLEAR(px1,  px2,      py2,  C2Y(cy2));
	#undef CLEAR
	xtmux_redraw(x->client, cx1, cy1, cx2, cy2);
}

static void
xtmux_focus(struct tty *tty, int focus)
{
	struct xtmux *x = tty->xtmux;

	if (x->focus_out == !focus)
		return;

	xt_clear_cursor(x);
	x->focus_out = !focus;
	xt_fill_cursor(x, tty->cstyle);

	if (x->xic)
	{
		if (focus)
			XSetICFocus(x->xic);
		else
			XUnsetICFocus(x->xic);
	}
}

static void
xtmux_main(struct tty *tty)
{
	struct xtmux *x = tty->xtmux;

	xt_putc_flush(x);
	xt_scroll_flush(x);

	while (XPending(x->display))
	{
		XEvent xev;
		XNextEvent(x->display, &xev);

		switch (xev.type)
		{
			case KeyPress:
				x->last_time = xev.xkey.time;
				xtmux_key_press(tty, &xev.xkey);
				break;

			case ButtonPress:
			case ButtonRelease:
			case MotionNotify: /* XMotionEvent looks enough like XButtonEvent */
				x->last_time = xev.xbutton.time;
				xtmux_button_press(tty, &xev.xbutton);
				break;

			case NoExpose:
				if (xev.xnoexpose.drawable == x->window && x->copy_active)
					x->copy_active --;
				break;

			case GraphicsExpose:
			case Expose:
				if (xev.xexpose.window == x->window)
					xt_expose(x, &xev.xexpose);
				break;

			case FocusIn:
			case FocusOut:
				if (xev.xfocus.window == x->window)
					xtmux_focus(tty, xev.type == FocusIn);
				break;

			case UnmapNotify:
				if (xev.xunmap.window == x->window)
					tty->flags |= TTY_UNMAPPED;
				break;

			case MapNotify:
				if (xev.xmap.window == x->window)
					tty->flags &= ~TTY_UNMAPPED;
				break;

			case ConfigureNotify:
				if (xev.xconfigure.window != x->window)
					break;
				while (XCheckTypedWindowEvent(x->display, x->window, ConfigureNotify, &xev));
				xtmux_configure_notify(tty, &xev.xconfigure);
				break;

			case MappingNotify:
				XRefreshKeyboardMapping(&xev.xmapping);
				break;

			case SelectionClear:
				x->last_time = xev.xselectionclear.time;
				/* could do paste_free_top or something, but probably shouldn't.
				 * might want to visually indicate X selection some other way, though */
				break;

			case SelectionRequest:
				xtmux_selection_request(tty, &xev.xselectionrequest);
				break;

			case SelectionNotify:
				xtmux_selection_notify(tty, &xev.xselection);
				break;

			case DestroyNotify:
				if (xev.xdestroywindow.window == x->window)
					tty->client->flags |= CLIENT_EXIT;
				break;

			default:
				fprintf(stderr, "unhandled x event %d\n", xev.type);
		}
	}
}
