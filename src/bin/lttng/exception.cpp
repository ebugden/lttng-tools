/*
 * Copyright (C) 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include "exception.hpp"

#include <common/format.hpp>
#include <common/utils.hpp>

#include <sstream>

// TODO: Why are the arguments aligned this way?
lttng::cli::no_default_session_error::no_default_session_error(const char *file_name,
							       const char *function_name,
							       unsigned int line_number) :
	runtime_error(lttng::format("No default session found in `{}/.lttngrc`",
				    utils_get_home_dir() ?: "LTTNG_HOME"),
		      file_name,
		      function_name,
		      line_number)
{
}

// What's up with this manual spacing? What's the rationale? Is it some automatic formatting?
lttng::cli::unexpected_type::unexpected_type(const char *object_name,
								   int type,
								   const char *file_name,
								   const char *function_name,
								   unsigned int line_number) :
	runtime_error(lttng::format("Unexpected {} type: {}", object_name, type),
		      file_name,
		      function_name,
		      line_number)
{
}

lttng::cli::trigger_notification_subscription_error::trigger_notification_subscription_error(
								   const char *trigger_name,
								   const char *file_name,
								   const char *function_name,
								   unsigned int line_number) :
	runtime_error(lttng::format("Failed to subscribe to notifications of trigger {}.", trigger_name),
			  file_name,
			  function_name,
			  line_number)
{
}
