/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
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

#include <sys/types.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Paste paste buffer if present.
 */

static enum cmd_retval	cmd_paste_buffer_exec(struct cmd *, struct cmdq_item *);

#ifdef XTMUX
#define X_OPT	"x"
#else
#define X_OPT
#endif

const struct cmd_entry cmd_paste_buffer_entry = {
	.name = "paste-buffer",
	.alias = "pasteb",

	.args = { "db:prs:t:" X_OPT, 0, 0 },
	.usage = "[-dpr" X_OPT "] [-s separator] " CMD_BUFFER_USAGE " "
		 CMD_TARGET_PANE_USAGE,

	.target = { 't', CMD_FIND_PANE, 0 },

	.flags = CMD_AFTERHOOK,
	.exec = cmd_paste_buffer_exec
};

static enum cmd_retval
cmd_paste_buffer_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args		*args = self->args;
	struct window_pane	*wp = item->target.wp;
	struct paste_buffer	*pb;
	const char		*sepstr, *bufname, *bufdata;
	size_t			 bufsize;
	int			 bracket = args_has(args, 'p');

	sepstr = args_get(args, 's');
	if (sepstr == NULL) {
		if (args_has(args, 'r'))
			sepstr = "\n";
		else
			sepstr = "\r";
	}

	bufname = NULL;
	if (args_has(args, 'b'))
		bufname = args_get(args, 'b');

#ifdef XTMUX
	if (args_has(args, 'x'))
	{
		if (!(item->client && item->client->tty.xtmux))
		{
			cmdq_error(item, "not xtmux");
			return (CMD_RETURN_ERROR);
		}
		return xtmux_paste(&item->client->tty, wp, bufname, sepstr);
	}
#endif

	if (bufname == NULL)
		pb = paste_get_top(NULL);
	else {
		pb = paste_get_name(bufname);
		if (pb == NULL) {
			cmdq_error(item, "no buffer %s", bufname);
			return (CMD_RETURN_ERROR);
		}
	}

	if (pb != NULL) {
		bufdata = paste_buffer_data(pb, &bufsize);
		paste_send_pane(bufdata, bufsize, wp, sepstr, bracket);
	}

	if (pb != NULL && args_has(args, 'd'))
		paste_free(pb);

	return (CMD_RETURN_NORMAL);
}
