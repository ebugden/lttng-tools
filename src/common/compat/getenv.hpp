#ifndef _COMPAT_GETENV_H
#define _COMPAT_GETENV_H

/*
 * Copyright (C) 2015 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 *
 * SPDX-License-Identifier: LGPL-2.1-only
 *
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <common/error.hpp>

static inline
int lttng_is_setuid_setgid()
{
	return geteuid() != getuid() || getegid() != getgid();
}

static inline
char *lttng_secure_getenv(const char *name)
{
	if (lttng_is_setuid_setgid()) {
		WARN("Getting environment variable '%s' from setuid/setgid binary refused for security reasons.",
			name);
		return nullptr;
	}
	return getenv(name);
}

#endif /* _COMPAT_GETENV_H */
