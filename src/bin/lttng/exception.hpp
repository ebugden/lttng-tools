/*
 * SPDX-FileCopyrightText: 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#ifndef LTTNG_CLI_EXCEPTION_H
#define LTTNG_CLI_EXCEPTION_H

#include <common/exception.hpp>

#include <lttng/lttng-error.h>

#include <stdexcept>
#include <string>

#define LTTNG_THROW_CLI_NO_DEFAULT_SESSION() \
	throw lttng::cli::no_default_session_error(LTTNG_SOURCE_LOCATION())
// Could the following exceptions be in the common exceptions?
// Include source location in the following exceptions? Does the location make sense here?
#define LTTNG_THROW_CLI_UNEXPECTED_TYPE(object_name, type) \
	throw lttng::cli::unexpected_type(object_name, type, LTTNG_SOURCE_LOCATION())
#define LTTNG_THROW_CLI_TRIGGER_NOTIFICATION_SUBSCRIPTION_ERROR(trigger_name)   \
	throw lttng::cli::trigger_notification_subscription_error(trigger_name, \
								  LTTNG_SOURCE_LOCATION())

namespace lttng {
namespace cli {
class no_default_session_error : public runtime_error {
public:
	explicit no_default_session_error(const lttng::source_location& source_location);
};

// TODO: Add a `listen` namespace? Or a trigger notification namespace?

class unexpected_type : public runtime_error {
public:
	// TODO: Clarify why I mean by "object_name" (i.e. notification, event expression). "object_type_name"? Just make two different exceptions?
	explicit unexpected_type(const char *object_name,
				 int type,
				 const lttng::source_location& source_location);
};

// TODO: This has more to do with trigger notification.
// TODO: Are there other places this new exception should be used?
class trigger_notification_subscription_error : public runtime_error {
public:
	explicit trigger_notification_subscription_error(
		const char *trigger_name, const lttng::source_location& source_location);
};

// TODO: Should the following line have a `;`?
} /* namespace cli */
}; /* namespace lttng */

#endif /* LTTNG_CLI_EXCEPTION_H */
