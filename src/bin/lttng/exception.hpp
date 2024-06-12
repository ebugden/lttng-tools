/*
 * Copyright (C) 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
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
	throw lttng::cli::no_default_session_error(__FILE__, __func__, __LINE__)

namespace lttng {
namespace cli {
class no_default_session_error : public runtime_error {
public:
	explicit no_default_session_error(const char *file_name,
					  const char *function_name,
					  unsigned int line_number); // Why are these aligned this way?
};

// TODO: Add a `listen` namespace? Or a trigger notification namespace?

class unexpected_type : public runtime_error {
public:
	// TODO: Clarify why I mean by "object_name" (i.e. notification, event expression). "object_type_name"? Just make two different exceptions?
	explicit unexpected_type(const char *object_name,
					  int type,
					  const char *file_name,
					  const char *function_name,
					  unsigned int line_number);
};

// TODO: This has more to do with trigger notification.
// TODO: Are there other places this new exception should be used?
class trigger_notification_subscription_error : public runtime_error {
public:
	explicit trigger_notification_subscription_error(const char *trigger_name,
					  const char *file_name,
					  const char *function_name,
					  unsigned int line_number);
};

// TODO: Should the following line have a `;`?
} /* namespace cli */
}; /* namespace lttng */

#endif /* LTTNG_CLI_EXCEPTION_H */
