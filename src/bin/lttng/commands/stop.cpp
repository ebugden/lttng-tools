/*
 * SPDX-FileCopyrightText: 2011 EfficiOS Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include "../command.hpp"
#include "../exception.hpp"
#include "../utils.hpp"

#include <common/exception.hpp>
#include <common/mi-lttng.hpp>
#include <common/sessiond-comm/sessiond-comm.hpp>

#include <popt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

enum {
	OPT_HELP = 1,
	OPT_LIST_OPTIONS,
	OPT_ENABLE_GLOB,
	OPT_ALL,
};

namespace {
int opt_no_wait;
struct mi_writer *writer;

#ifdef LTTNG_EMBED_HELP
const char help_msg[] =
#include <lttng-stop.1.h>
	;
#endif

struct poptOption long_options[] = {
	/* longName, shortName, argInfo, argPtr, value, descrip, argDesc */
	{ "help", 'h', POPT_ARG_NONE, nullptr, OPT_HELP, nullptr, nullptr },
	{ "list-options", 0, POPT_ARG_NONE, nullptr, OPT_LIST_OPTIONS, nullptr, nullptr },
	{ "no-wait", 'n', POPT_ARG_VAL, &opt_no_wait, 1, nullptr, nullptr },
	{ "glob", 'g', POPT_ARG_NONE, nullptr, OPT_ENABLE_GLOB, nullptr, nullptr },
	{ "all", 'a', POPT_ARG_NONE, nullptr, OPT_ALL, nullptr, nullptr },
	{ nullptr, 0, 0, nullptr, 0, nullptr, nullptr }
};

/*
 * Mi print of partial session
 */
int mi_print_session(const char *session_name, int enabled)
{
	int ret;
	LTTNG_ASSERT(writer);
	LTTNG_ASSERT(session_name);

	/* Open session element */
	ret = mi_lttng_writer_open_element(writer, config_element_session);
	if (ret) {
		goto end;
	}

	/* Print session name element */
	ret = mi_lttng_writer_write_element_string(writer, config_element_name, session_name);
	if (ret) {
		goto end;
	}

	/* Is enabled ? */
	ret = mi_lttng_writer_write_element_bool(writer, config_element_enabled, enabled);
	if (ret) {
		goto end;
	}

	/* Close session element */
	ret = mi_lttng_writer_close_element(writer);

end:
	return ret;
}

/*
 * Start tracing for all trace of the session.
 */
cmd_error_code stop_tracing(const char *session_name)
{
	int ret;

	ret = lttng_stop_tracing_no_wait(session_name);
	if (ret < 0) {
		LTTNG_THROW_CTL(lttng::format("Failed to stop session `{}`", session_name),
				static_cast<lttng_error_code>(-ret));
	}

	if (!opt_no_wait) {
		_MSG("Waiting for data availability");
		fflush(stdout);
		do {
			ret = lttng_data_pending(session_name);
			if (ret < 0) {
				/* Return the data available call error. */
				ERR_FMT("Failed to check pending data for session `{}` ({})",
					session_name,
					lttng_strerror(ret));
				return CMD_ERROR;
			}

			/*
			 * Data sleep time before retrying (in usec). Don't sleep if the call
			 * returned value indicates availability.
			 */
			if (ret) {
				usleep(DEFAULT_DATA_AVAILABILITY_WAIT_TIME_US);
				_MSG(".");
				fflush(stdout);
			}
		} while (ret != 0);
		MSG("");
	}

	print_session_stats(session_name);
	MSG("Tracing stopped for session `%s`", session_name);
	if (lttng_opt_mi) {
		if (mi_print_session(session_name, 0)) {
			return CMD_ERROR;
		}
	}

	return CMD_SUCCESS;
}

cmd_error_code stop_tracing(const lttng::cli::session_spec& spec)
{
	bool had_warning = false;
	bool had_error = false;
	bool listing_failed = false;

	const auto sessions = [&listing_failed, &spec]() -> lttng::ctl::session_list {
		try {
			return list_sessions(spec);
		} catch (const lttng::ctl::error& ctl_exception) {
			ERR_FMT("Failed to list sessions ({})",
				lttng_strerror(-ctl_exception.code()));
			listing_failed = true;
			return {};
		} catch (const lttng::cli::no_default_session_error& cli_exception) {
			/*
			 * The retrieval of the default session name already logs
			 * an error when it fails. There is no value in printing
			 * anything about this exception.
			 */
			listing_failed = true;
			return {};
		}
	}();

	if (!listing_failed && sessions.size() == 0 &&
	    spec.type_ == lttng::cli::session_spec::type::NAME) {
		ERR_FMT("Session `{}` not found", spec.value);
		return CMD_ERROR;
	}

	if (listing_failed) {
		return CMD_FATAL;
	}

	for (const auto& session : sessions) {
		cmd_error_code sub_ret;

		try {
			sub_ret = stop_tracing(session.name);
		} catch (const lttng::ctl::error& ctl_exception) {
			switch (ctl_exception.code()) {
			case LTTNG_ERR_TRACE_ALREADY_STOPPED:
				WARN_FMT("Tracing already stopped for session `{}`", session.name);
				sub_ret = CMD_SUCCESS;
				break;
			case LTTNG_ERR_NO_SESSION:
				if (spec.type_ != lttng::cli::session_spec::type::NAME) {
					/* Session destroyed during command, ignore and carry-on. */
					sub_ret = CMD_SUCCESS;
					break;
				} else {
					sub_ret = CMD_ERROR;
					break;
				}
			case LTTNG_ERR_NO_SESSIOND:
				/* Don't keep going on a fatal error. */
				return CMD_FATAL;
			default:
				/* Generic error. */
				sub_ret = CMD_ERROR;
				ERR_FMT("Failed to stop session `{}` ({})",
					session.name,
					lttng_strerror(-ctl_exception.code()));
				break;
			}
		}

		/* Keep going, but report the most serious state. */
		had_warning |= sub_ret == CMD_WARNING;
		had_error |= sub_ret == CMD_ERROR;
	}

	if (had_error) {
		return CMD_ERROR;
	} else if (had_warning) {
		return CMD_WARNING;
	} else {
		return CMD_SUCCESS;
	}
}
} /* namespace */

/*
 *  cmd_stop
 *
 *  The 'stop <options>' first level command
 */
int cmd_stop(int argc, const char **argv)
{
	int opt;
	cmd_error_code command_ret = CMD_SUCCESS;
	bool success = true;
	static poptContext pc;
	const char *leftover = nullptr;
	lttng::cli::session_spec session_spec(lttng::cli::session_spec::type::NAME);

	pc = poptGetContext(nullptr, argc, argv, long_options, 0);
	poptReadDefaultConfig(pc, 0);

	while ((opt = poptGetNextOpt(pc)) != -1) {
		switch (opt) {
		case OPT_HELP:
		{
			int ret;

			SHOW_HELP();
			command_ret = static_cast<cmd_error_code>(ret);
			goto end;
		}
		case OPT_LIST_OPTIONS:
			list_cmd_options(stdout, long_options);
			goto end;
		case OPT_ENABLE_GLOB:
			session_spec.type_ = lttng::cli::session_spec::type::GLOB_PATTERN;
			break;
		case OPT_ALL:
			session_spec.type_ = lttng::cli::session_spec::type::ALL;
			break;
		default:
			command_ret = CMD_UNDEFINED;
			goto end;
		}
	}

	/* Mi check */
	if (lttng_opt_mi) {
		writer = mi_lttng_writer_create(fileno(stdout), lttng_opt_mi);
		if (!writer) {
			command_ret = CMD_ERROR;
			goto end;
		}

		/* Open command element */
		if (mi_lttng_writer_command_open(writer, mi_lttng_element_command_stop)) {
			command_ret = CMD_ERROR;
			goto end;
		}

		/* Open output element */
		if (mi_lttng_writer_open_element(writer, mi_lttng_element_command_output)) {
			command_ret = CMD_ERROR;
			goto end;
		}

		/*
		 * Open sessions element
		 * For validation
		 */
		if (mi_lttng_writer_open_element(writer, config_element_sessions)) {
			command_ret = CMD_ERROR;
			goto end;
		}
	}

	session_spec.value = poptGetArg(pc);

	leftover = poptGetArg(pc);
	if (leftover) {
		ERR("Unknown argument: %s", leftover);
		command_ret = CMD_ERROR;
		goto end;
	}

	command_ret = stop_tracing(session_spec);
	if (command_ret) {
		success = false;
	}

	/* Mi closing */
	if (lttng_opt_mi) {
		/* Close sessions and  output element */
		if (mi_lttng_close_multi_element(writer, 2)) {
			command_ret = CMD_ERROR;
			goto end;
		}

		/* Success ? */
		if (mi_lttng_writer_write_element_bool(
			    writer, mi_lttng_element_command_success, success)) {
			command_ret = CMD_ERROR;
			goto end;
		}

		/* Command element close */
		if (mi_lttng_writer_command_close(writer)) {
			command_ret = CMD_ERROR;
			goto end;
		}
	}

end:
	/* Mi clean-up */
	if (writer && mi_lttng_writer_destroy(writer)) {
		command_ret = CMD_ERROR;
	}

	poptFreeContext(pc);
	return command_ret;
}
