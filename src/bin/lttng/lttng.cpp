/*
 * SPDX-FileCopyrightText: 2011 EfficiOS Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include "command.hpp"
#include "version.hpp"

#include <common/compat/getenv.hpp>
#include <common/error.hpp>
#include <common/utils.hpp>

#include <lttng/lttng.h>

#include <ctype.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *help_msg =
#ifdef LTTNG_EMBED_HELP
#include <lttng.1.h>
#else
	nullptr
#endif
	;

/* Variables */
static const char *progname;
int opt_no_sessiond;
char *opt_sessiond_path;

char *opt_relayd_path;

enum {
	OPT_RELAYD_PATH,
	OPT_SESSION_PATH,
	OPT_DUMP_OPTIONS,
	OPT_DUMP_COMMANDS,
};

/* Getopt options. No first level command. */
static struct option long_options[] = { { "version", 0, nullptr, 'V' },
					{ "help", 0, nullptr, 'h' },
					{ "group", 1, nullptr, 'g' },
					{ "verbose", 0, nullptr, 'v' },
					{ "quiet", 0, nullptr, 'q' },
					{ "mi", 1, nullptr, 'm' },
					{ "no-sessiond", 0, nullptr, 'n' },
					{ "sessiond-path", 1, nullptr, OPT_SESSION_PATH },
					{ "relayd-path", 1, nullptr, OPT_RELAYD_PATH },
					{ "list-options", 0, nullptr, OPT_DUMP_OPTIONS },
					{ "list-commands", 0, nullptr, OPT_DUMP_COMMANDS },
					{ nullptr, 0, nullptr, 0 } };

/* First level command */
static struct cmd_struct commands[] = {
	{ "add-context", cmd_add_context },
	{ "add-trigger", cmd_add_trigger },
	{ "create", cmd_create },
	{ "clear", cmd_clear },
	{ "destroy", cmd_destroy },
	{ "disable-channel", cmd_disable_channels },
	{ "disable-event", cmd_disable_events },
	{ "enable-channel", cmd_enable_channels },
	{ "enable-event", cmd_enable_events },
	{ "help", nullptr },
	{ "list", cmd_list },
	{ "list-triggers", cmd_list_triggers },
	{ "load", cmd_load },
	{ "metadata", cmd_metadata },
	{ "regenerate", cmd_regenerate },
	{ "remove-trigger", cmd_remove_trigger },
	{ "rotate", cmd_rotate },
	{ "enable-rotation", cmd_enable_rotation },
	{ "disable-rotation", cmd_disable_rotation },
	{ "save", cmd_save },
	{ "set-session", cmd_set_session },
	{ "snapshot", cmd_snapshot },
	{ "start", cmd_start },
	{ "status", cmd_status },
	{ "stop", cmd_stop },
	{ "track", cmd_track },
	{ "untrack", cmd_untrack },
	{ "version", cmd_version },
	{ "view", cmd_view },
	{ nullptr, nullptr } /* Array closure */
};

static void version(FILE *ofp)
{
	fprintf(ofp,
		"%s (LTTng Trace Control) " VERSION " - " VERSION_NAME "%s%s\n",
		progname,
		GIT_VERSION[0] == '\0' ? "" : " - " GIT_VERSION,
		EXTRA_VERSION_NAME[0] == '\0' ? "" : " - " EXTRA_VERSION_NAME);
}

/*
 * Find the MI output type enum from a string. This function is for the support
 * of machine interface output.
 */
static int mi_output_type(const char *output_type)
{
	int ret = 0;

	if (!strncasecmp("xml", output_type, 3)) {
		ret = LTTNG_MI_XML;
	} else {
		/* Invalid output format */
		ERR("MI output format not supported");
		ret = -LTTNG_ERR_MI_OUTPUT_TYPE;
	}

	return ret;
}

/*
 *  list_options
 *
 *  List options line by line. This is mostly for bash auto completion and to
 *  avoid difficult parsing.
 */
static void list_options(FILE *ofp)
{
	int i = 0;
	struct option *option = nullptr;

	option = &long_options[i];
	while (option->name != nullptr) {
		fprintf(ofp, "--%s\n", option->name);

		if (isprint(option->val)) {
			fprintf(ofp, "-%c\n", option->val);
		}

		i++;
		option = &long_options[i];
	}
}

/*
 *  handle_command
 *
 *  Handle the full argv list of a first level command. Will find the command
 *  in the global commands array and call the function callback associated.
 *
 *  If command not found, return -1
 *  else, return function command error code.
 */
static int handle_command(int argc, char **argv)
{
	int i = 0, ret;
	struct cmd_struct *cmd;

	if (*argv == nullptr) {
		ret = CMD_SUCCESS;
		goto end;
	}

	/* Special case for help command which needs the commands array */
	if (strcmp(argv[0], "help") == 0) {
		ret = cmd_help(argc, (const char **) argv, commands);
		goto end;
	}

	cmd = &commands[i];
	while (cmd->name != nullptr) {
		/* Find command */
		if (strcmp(argv[0], cmd->name) == 0) {
			try {
				ret = cmd->func(argc, (const char **) argv);
			} catch (const std::exception& e) {
				ERR_FMT("{}", e.what());
				ret = CMD_ERROR;
			}

			goto end;
		}
		i++;
		cmd = &commands[i];
	}

	/* Command not found */
	ret = CMD_UNDEFINED;

end:
	return ret;
}

static bool command_exists(const char *command)
{
	const struct cmd_struct *cmd = commands;
	bool exists = false;

	while (cmd->name != nullptr) {
		if (!strcmp(command, cmd->name)) {
			exists = true;
			goto end;
		}
		cmd++;
	}

end:
	return exists;
}

static void show_basic_help()
{
	puts("Usage: lttng [--group=GROUP] [--mi=TYPE] [--no-sessiond | --sessiond-path=PATH]");
	puts("             [--quiet | -v | -vv | -vvv] COMMAND [COMMAND OPTIONS]");
	puts("");
	puts("Available commands:");
	puts("");
	puts("Recording sessions:");
	puts("  create            " CONFIG_CMD_DESCR_CREATE);
	puts("  clear             " CONFIG_CMD_DESCR_CLEAR);
	puts("  destroy           " CONFIG_CMD_DESCR_DESTROY);
	puts("  load              " CONFIG_CMD_DESCR_LOAD);
	puts("  regenerate        " CONFIG_CMD_DESCR_REGENERATE);
	puts("  save              " CONFIG_CMD_DESCR_SAVE);
	puts("  set-session       " CONFIG_CMD_DESCR_SET_SESSION);
	puts("");
	puts("Channels:");
	puts("  add-context       " CONFIG_CMD_DESCR_ADD_CONTEXT);
	puts("  disable-channel   " CONFIG_CMD_DESCR_DISABLE_CHANNEL);
	puts("  enable-channel    " CONFIG_CMD_DESCR_ENABLE_CHANNEL);
	puts("");
	puts("Recording event rules:");
	puts("  disable-event     " CONFIG_CMD_DESCR_DISABLE_EVENT);
	puts("  enable-event      " CONFIG_CMD_DESCR_ENABLE_EVENT);
	puts("");
	puts("Status:");
	puts("  list              " CONFIG_CMD_DESCR_LIST);
	puts("  status            " CONFIG_CMD_DESCR_STATUS);
	puts("");
	puts("Control:");
	puts("  snapshot          " CONFIG_CMD_DESCR_SNAPSHOT);
	puts("  start             " CONFIG_CMD_DESCR_START);
	puts("  stop              " CONFIG_CMD_DESCR_STOP);
	puts("");
	puts("Recording session rotation:");
	puts("  disable-rotation  " CONFIG_CMD_DESCR_DISABLE_ROTATION);
	puts("  enable-rotation   " CONFIG_CMD_DESCR_ENABLE_ROTATION);
	puts("  rotate            " CONFIG_CMD_DESCR_ROTATE);
	puts("");
	puts("Resource tracking:");
	puts("  track             " CONFIG_CMD_DESCR_TRACK);
	puts("  untrack           " CONFIG_CMD_DESCR_UNTRACK);
	puts("");
	puts("Triggers:");
	puts("  add-trigger       " CONFIG_CMD_DESCR_ADD_TRIGGER);
	puts("  remove-trigger    " CONFIG_CMD_DESCR_REMOVE_TRIGGER);
	puts("  list-triggers     " CONFIG_CMD_DESCR_LIST_TRIGGERS);
	puts("");
	puts("Miscellaneous:");
	puts("  help              " CONFIG_CMD_DESCR_HELP);
	puts("  version           " CONFIG_CMD_DESCR_VERSION);
	puts("  view              " CONFIG_CMD_DESCR_VIEW);
	puts("");
	puts("Run `lttng help COMMAND` or `lttng COMMAND --help` to get help with");
	puts("command COMMAND.");
	puts("");
	puts("See `man lttng` for more help with the lttng command.");
}

/*
 * Parse command line arguments.
 *
 * Return 0 if OK, else -1
 */
static int parse_args(int argc, char **argv)
{
	int opt, ret;

	if (lttng_is_setuid_setgid()) {
		ERR("'%s' is not allowed to be executed as a setuid/setgid binary for security reasons. Aborting.",
		    argv[0]);
		return EXIT_FAILURE;
	}

	if (argc < 2) {
		show_basic_help();
		return EXIT_FAILURE;
	}

	while ((opt = getopt_long(argc, argv, "+Vhnvqg:m:", long_options, nullptr)) != -1) {
		switch (opt) {
		case 'V':
			version(stdout);
			ret = 0;
			goto end;
		case 'h':
			ret = utils_show_help(1, "lttng", help_msg);
			if (ret) {
				ERR("Cannot show --help for `lttng`");
				perror("exec");
			}
			goto end;
		case 'v':
			/* There are only 3 possible levels of verbosity. (-vvv) */
			if (lttng_opt_verbose < 3) {
				lttng_opt_verbose += 1;
			}
			break;
		case 'q':
			lttng_opt_quiet = 1;
			break;
		case 'm':
			/* Machine interface */
			lttng_opt_mi = mi_output_type(optarg);
			if (lttng_opt_mi < 0) {
				ret = lttng_opt_mi;
				goto error;
			}
			break;
		case 'g':
			lttng_set_tracing_group(optarg);
			break;
		case 'n':
			opt_no_sessiond = 1;
			break;
		case OPT_SESSION_PATH:
			free(opt_sessiond_path);
			opt_sessiond_path = strdup(optarg);
			if (!opt_sessiond_path) {
				ret = -1;
				goto error;
			}
			break;
		case OPT_RELAYD_PATH:
			free(opt_relayd_path);
			opt_relayd_path = strdup(optarg);
			if (!opt_relayd_path) {
				ret = -1;
				goto error;
			}
			break;
		case OPT_DUMP_OPTIONS:
			list_options(stdout);
			ret = 0;
			goto end;
		case OPT_DUMP_COMMANDS:
			list_commands(commands, stdout);
			ret = 0;
			goto end;
		default:
			ret = 1;
			goto error;
		}
	}

	/* If both options are specified, quiet wins */
	if (lttng_opt_verbose && lttng_opt_quiet) {
		lttng_opt_verbose = 0;
	}

	/* No leftovers, quit */
	if ((argc - optind) == 0) {
		ret = 1;
		goto error;
	}

	/*
	 * Handle leftovers which is a first level command with the trailing
	 * options.
	 */
	ret = handle_command(argc - optind, argv + optind);
	switch (ret) {
	case CMD_WARNING:
	case CMD_ERROR:
		break;
	case CMD_UNDEFINED:
		if (!command_exists(*(argv + optind))) {
			MSG("lttng: %s is not an lttng command. See 'lttng --help'.",
			    *(argv + optind));
		} else {
			ERR("Unrecognized argument used with \'%s\' command", *(argv + optind));
		}
		break;
	case CMD_FATAL:
	case CMD_UNSUPPORTED:
		break;
	case -1:
		ret = 1;
		break;
	case 0:
		break;
	default:
		if (ret < 0) {
			ret = -ret;
		}
		break;
	}

end:
error:
	return ret;
}

/*
 *  main
 */
static int _main(int argc, char *argv[])
{
	progname = argv[0] ? argv[0] : "lttng";
	return parse_args(argc, argv);
}

int main(int argc, char **argv)
{
	try {
		return _main(argc, argv);
	} catch (const std::exception& e) {
		ERR_FMT("Unhandled exception caught by client: {}", e.what());
		abort();
	}
}
