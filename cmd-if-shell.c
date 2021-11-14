/* $OpenBSD$ */

/*
 * Copyright (c) 2009 Tiago Cunha <me@tiagocunha.org>
 * Copyright (c) 2009 Nicholas Marriott <nicm@openbsd.org>
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
#include <sys/wait.h>

#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/*
 * Executes a tmux command if a shell command returns true or false.
 */

static enum cmd_retval	cmd_if_shell_exec(struct cmd *, struct cmdq_item *);

static void		cmd_if_shell_callback(struct job *);
static void		cmd_if_shell_free(void *);

const struct cmd_entry cmd_if_shell_entry = {
	.name = "if-shell",
	.alias = "if",

	.args = { "bFt:", 2, 3 },
	.usage = "[-bF] " CMD_TARGET_PANE_USAGE " shell-command command "
		 "[command]",

	.target = { 't', CMD_FIND_PANE, CMD_FIND_CANFAIL },

	.flags = 0,
	.exec = cmd_if_shell_exec
};

struct cmd_if_shell_data {
	struct cmd_parse_input	 input;

	char			*cmd_if;
	char			*cmd_else;

	struct client		*client;
	struct cmdq_item	*item;
};

static enum cmd_retval
cmd_if_shell_exec(struct cmd *self, struct cmdq_item *item)
{
	struct args			*args = cmd_get_args(self);
	struct cmd_find_state		*target = cmdq_get_target(item);
	struct cmdq_state		*state = cmdq_get_state(item);
	struct cmd_if_shell_data	*cdata;
	char				*shellcmd, *cmd, *error;
	const char			*file;
	struct client			*tc = cmdq_get_target_client(item);
	struct session			*s = target->s;
	struct cmd_parse_input		 pi;
	enum cmd_parse_status		 status;

	shellcmd = format_single_from_target(item, args->argv[0]);
	if (args_has(args, 'F')) {
		if (*shellcmd != '0' && *shellcmd != '\0')
			cmd = args->argv[1];
		else if (args->argc == 3)
			cmd = args->argv[2];
		else
			cmd = NULL;
		free(shellcmd);
		if (cmd == NULL)
			return (CMD_RETURN_NORMAL);

		memset(&pi, 0, sizeof pi);
		cmd_get_source(self, &pi.file, &pi.line);
		pi.item = item;
		pi.c = tc;
		cmd_find_copy_state(&pi.fs, target);

		status = cmd_parse_and_insert(cmd, &pi, item, state, &error);
		if (status == CMD_PARSE_ERROR) {
			cmdq_error(item, "%s", error);
			free(error);
			return (CMD_RETURN_ERROR);
		}
		return (CMD_RETURN_NORMAL);
	}

	cdata = xcalloc(1, sizeof *cdata);

	cdata->cmd_if = xstrdup(args->argv[1]);
	if (args->argc == 3)
		cdata->cmd_else = xstrdup(args->argv[2]);
	else
		cdata->cmd_else = NULL;

	if (!args_has(args, 'b'))
		cdata->client = cmdq_get_client(item);
	else
		cdata->client = tc;
	if (cdata->client != NULL)
		cdata->client->references++;

	if (!args_has(args, 'b'))
		cdata->item = item;
	else
		cdata->item = NULL;

	memset(&cdata->input, 0, sizeof cdata->input);
	cmd_get_source(self, &file, &cdata->input.line);
	if (file != NULL)
		cdata->input.file = xstrdup(file);
	cdata->input.c = tc;
	if (cdata->input.c != NULL)
		cdata->input.c->references++;
	cmd_find_copy_state(&cdata->input.fs, target);

	if (job_run(shellcmd, 0, NULL, s,
	    server_client_get_cwd(cmdq_get_client(item), s), NULL,
	    cmd_if_shell_callback, cmd_if_shell_free, cdata, 0, -1,
	    -1) == NULL) {
		cmdq_error(item, "failed to run command: %s", shellcmd);
		free(shellcmd);
		free(cdata);
		return (CMD_RETURN_ERROR);
	}
	free(shellcmd);

	if (args_has(args, 'b'))
		return (CMD_RETURN_NORMAL);
	return (CMD_RETURN_WAIT);
}

static void
cmd_if_shell_callback(struct job *job)
{
	struct cmd_if_shell_data	*cdata = job_get_data(job);
	struct client			*c = cdata->client;
	struct cmdq_item		*new_item = NULL;
	struct cmdq_state		*new_state = NULL;
	char				*cmd;
	int				 status;
	struct cmd_parse_result		*pr;

	status = job_get_status(job);
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		cmd = cdata->cmd_else;
	else
		cmd = cdata->cmd_if;
	if (cmd == NULL)
		goto out;

	pr = cmd_parse_from_string(cmd, &cdata->input);
	switch (pr->status) {
	case CMD_PARSE_EMPTY:
		break;
	case CMD_PARSE_ERROR:
		if (cdata->item != NULL)
		       cmdq_error(cdata->item, "%s", pr->error);
		free(pr->error);
		break;
	case CMD_PARSE_SUCCESS:
		if (cdata->item == NULL)
			new_state = cmdq_new_state(NULL, NULL, 0);
		else
			new_state = cmdq_get_state(cdata->item);
		new_item = cmdq_get_command(pr->cmdlist, new_state);
		if (cdata->item == NULL)
			cmdq_free_state(new_state);
		cmd_list_free(pr->cmdlist);
		break;
	}
	if (new_item != NULL) {
		if (cdata->item == NULL)
			cmdq_append(c, new_item);
		else
			cmdq_insert_after(cdata->item, new_item);
	}

out:
	if (cdata->item != NULL)
		cmdq_continue(cdata->item);
}

static void
cmd_if_shell_free(void *data)
{
	struct cmd_if_shell_data	*cdata = data;

	if (cdata->client != NULL)
		server_client_unref(cdata->client);

	free(cdata->cmd_else);
	free(cdata->cmd_if);

	if (cdata->input.c != NULL)
		server_client_unref(cdata->input.c);
	free((void *)cdata->input.file);

	free(cdata);
}
