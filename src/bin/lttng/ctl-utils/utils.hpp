/*
 * Copyright (C) 2024 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef LTTNG_CTL_UTILS_H
#define LTTNG_CTL_UTILS_H

/*
 * RAII Utilities for the liblttng-ctl C-based Interface
 *
 * This file is part of a set of utilities that provide RAII (Resource Acquisition Is
 * Initialization) wrappers for interacting with the liblttng-ctl C-based interface. The utilities
 * are designed to simplify the use of the liblttng-ctl API in idiomatic C++ code by providing
 * wrappers under the lttng::ctl namespace.
 *
 * Usage:
 * Only include <ctl-utils/utils.hpp> in your calling code. This header will transitively include
 * other utility files that handle specific parts of the API surface.
 *
 * Example:
 * #include <ctl-utils/utils.hpp>
 *
 * This approach ensures that all necessary utilities are available without the need to include
 * individual files separately.
 */
// TODO: Replace this and all related headers with C++ API for liblttng-ctl

#define CTL_UTILS_INCLUDED
#include "notification.hpp"
#include "trigger.hpp"

#endif /* LTTNG_CTL_UTILS_H */
