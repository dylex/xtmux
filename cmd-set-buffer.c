/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicm@users.sourceforge.net>
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
 * Add, set, or append to a paste buffer.
 */

enum cmd_retval	 cmd_set_buffer_exec(struct cmd *, struct cmd_q *);

const struct cmd_entry cmd_set_buffer_entry = {
	"set-buffer", "setb",
	"ab:n:", 0, 1,
	"[-a] " CMD_BUFFER_USAGE " [-n new-buffer-name] data",
	0,
	cmd_set_buffer_exec
};

enum cmd_retval
cmd_set_buffer_exec(struct cmd *self, struct cmd_q *cmdq)
{
	struct args		*args = self->args;
	struct paste_buffer	*pb;
	char			*pdata, *cause;
	const char		*bufname;
	size_t			 psize, newsize;

	bufname = NULL;

	if (args_has(args, 'n')) {
		if (args->argc > 0) {
			cmdq_error(cmdq, "don't provide data with n flag");
			return (CMD_RETURN_ERROR);
		}

		if (args_has(args, 'b'))
			bufname = args_get(args, 'b');

		if (bufname == NULL) {
			pb = paste_get_top();
			if (pb == NULL) {
				cmdq_error(cmdq, "no buffer");
				return (CMD_RETURN_ERROR);
			}
			bufname = pb->name;
		}

		if (paste_rename(bufname, args_get(args, 'n'), &cause) != 0) {
			cmdq_error(cmdq, "%s", cause);
			free(cause);
			return (CMD_RETURN_ERROR);
		}

		return (CMD_RETURN_NORMAL);
	}

	if (args->argc != 1) {
		cmdq_error(cmdq, "no data specified");
		return (CMD_RETURN_ERROR);
	}

	psize = 0;
	pdata = NULL;

	pb = NULL;

	if ((newsize = strlen(args->argv[0])) == 0)
		return (CMD_RETURN_NORMAL);

	if (args_has(args, 'b')) {
		bufname = args_get(args, 'b');
		pb = paste_get_name(bufname);
	} else if (args_has(args, 'a')) {
		pb = paste_get_top();
		if (pb != NULL)
			bufname = pb->name;
	}

	if (args_has(args, 'a') && pb != NULL) {
		psize = pb->size;
		pdata = xmalloc(psize);
		memcpy(pdata, pb->data, psize);
	}

	pdata = xrealloc(pdata, psize + newsize);
	memcpy(pdata + psize, args->argv[0], newsize);
	psize += newsize;

	if (paste_set(pdata, psize, bufname, &cause) != 0) {
		cmdq_error(cmdq, "%s", cause);
		free(pdata);
		free(cause);
		return (CMD_RETURN_ERROR);
	}

	return (CMD_RETURN_NORMAL);
}
