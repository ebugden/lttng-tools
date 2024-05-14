/*
 * SPDX-FileCopyrightText: 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include "exception.hpp"

#include <common/format.hpp>
#include <common/utils.hpp>

#include <sstream>

lttng::cli::no_default_session_error::no_default_session_error(
	const lttng::source_location& location) :
	runtime_error(fmt::format("No default session found in `{}/.lttngrc`",
				  utils_get_home_dir() ?: "LTTNG_HOME"),
		      location)
{
}

lttng::cli::unexpected_type::unexpected_type(const char *object_name,
					     int type,
					     const lttng::source_location& location) :
	runtime_error(lttng::format("Unexpected {} type: {}", object_name, type), location)
{
}

lttng::cli::trigger_notification_subscription_error::trigger_notification_subscription_error(
	const char *trigger_name, const lttng::source_location& location) :
	runtime_error(lttng::format("Failed to subscribe to notifications of trigger {}.",
				    trigger_name),
		      location)
{
}
