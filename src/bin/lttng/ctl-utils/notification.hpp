/*
 * Copyright (C) 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 * Copyright (C) 2024 Erica Bugden <ebugden@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef LTTNG_CTL_NOTIFICATION_UTILS_H
#define LTTNG_CTL_NOTIFICATION_UTILS_H

#include <common/make-unique-wrapper.hpp>

#include <lttng/lttng.h>

#include <memory>

#ifndef CTL_UTILS_INCLUDED
#error "Include <ctl-util/utils.hpp> instead of including this file directly."
#endif

namespace lttng {
namespace ctl {

/*
 * The 'notification_channel' alias, based on `unique_ptr`, manages an
 * `lttng_notification_channel` resource with automatic memory cleanup.
 */
using notification_channel = std::unique_ptr<
	lttng_notification_channel,
	lttng::memory::create_deleter_class<lttng_notification_channel,
					    lttng_notification_channel_destroy>::deleter>;

/*
 * The 'notification' alias, based on `unique_ptr`, manages an
 * `lttng_notification` resource with automatic memory cleanup.
 */
using notification =
	std::unique_ptr<lttng_notification,
			lttng::memory::create_deleter_class<lttng_notification,
							    lttng_notification_destroy>::deleter>;

} /* namespace ctl */
} /* namespace lttng */

#endif /* LTTNG_CTL_NOTIFICATION_UTILS_H */
