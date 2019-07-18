/* $OpenBSD$ */

/*
 * Copyright (c) 2009 Nicholas Marriott <nicholas.marriott@gmail.com>
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

static u_int cmd_list_next_group = 1;

struct cmd_list *
cmd_list_new(void)
{
	struct cmd_list	*cmdlist;

	cmdlist = xcalloc(1, sizeof *cmdlist);
	cmdlist->references = 1;
	cmdlist->group = cmd_list_next_group++;
	TAILQ_INIT(&cmdlist->list);
	return (cmdlist);
}

void
cmd_list_append(struct cmd_list *cmdlist, struct cmd *cmd)
{
	cmd->group = cmdlist->group;
	TAILQ_INSERT_TAIL(&cmdlist->list, cmd, qentry);
}

void
cmd_list_move(struct cmd_list *cmdlist, struct cmd_list *from)
{
	struct cmd	*cmd, *cmd1;

	TAILQ_FOREACH_SAFE(cmd, &from->list, qentry, cmd1) {
		TAILQ_REMOVE(&from->list, cmd, qentry);
		TAILQ_INSERT_TAIL(&cmdlist->list, cmd, qentry);
	}
	cmdlist->group = cmd_list_next_group++;
}

void
cmd_list_free(struct cmd_list *cmdlist)
{
	struct cmd	*cmd, *cmd1;

	if (--cmdlist->references != 0)
		return;

	TAILQ_FOREACH_SAFE(cmd, &cmdlist->list, qentry, cmd1) {
		TAILQ_REMOVE(&cmdlist->list, cmd, qentry);
		cmd_free(cmd);
	}

	free(cmdlist);
}

char *
cmd_list_print(struct cmd_list *cmdlist, int escaped)
{
	struct cmd	*cmd;
	char		*buf, *this;
	size_t		 len;

	len = 1;
	buf = xcalloc(1, len);

	TAILQ_FOREACH(cmd, &cmdlist->list, qentry) {
		this = cmd_print(cmd);

		len += strlen(this) + 4;
		buf = xrealloc(buf, len);

		strlcat(buf, this, len);
		if (TAILQ_NEXT(cmd, qentry) != NULL) {
			if (escaped)
				strlcat(buf, " \\; ", len);
			else
				strlcat(buf, " ; ", len);
		}

		free(this);
	}

	return (buf);
}
