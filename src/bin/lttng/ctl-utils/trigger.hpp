/*
 * Copyright (C) 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef LTTNG_CTL_TRIGGER_UTILS_H
#define LTTNG_CTL_TRIGGER_UTILS_H

#include <common/make-unique-wrapper.hpp>

#include <lttng/lttng.h>

#include <memory>

#ifndef CTL_UTILS_INCLUDED
#error "Include <ctl-util/utils.hpp> instead of including this file directly."
#endif

namespace lttng {
namespace ctl {

/*
 * The 'triggers' alias, based on `unique_ptr`, manages `lttng_triggers`
 * resources with automatic memory cleanup.
 */
using triggers = std::unique_ptr<
	lttng_triggers,
	lttng::memory::create_deleter_class<lttng_triggers, lttng_triggers_destroy>::deleter>;

} /* namespace ctl */
} /* namespace lttng */

#endif /* LTTNG_CTL_TRIGGER_UTILS_H */
