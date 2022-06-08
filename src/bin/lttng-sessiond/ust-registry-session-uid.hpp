/*
 * Copyright (C) 2022 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef LTTNG_UST_REGISTRY_SESSION_UID_H
#define LTTNG_UST_REGISTRY_SESSION_UID_H

#include "trace-class.hpp"
#include "ust-registry-session.hpp"

#include <cstdint>
#include <lttng/lttng.h>
#include <unistd.h>

namespace lttng {
namespace sessiond {
namespace ust {

class registry_session_per_uid : public registry_session {
public:
	registry_session_per_uid(const struct lttng::sessiond::trace::abi& trace_abi,
			uint32_t major,
			uint32_t minor,
			const char *root_shm_path,
			const char *shm_path,
			uid_t euid,
			gid_t egid,
			uint64_t tracing_id,
			uid_t tracing_uid);

	virtual lttng_buffer_type get_buffering_scheme() const noexcept override final;

private:
	virtual void _visit_environment(
			lttng::sessiond::trace::trace_class_visitor& trace_class_visitor)
			const override final;

	const uid_t _tracing_uid;
};

} /* namespace ust */
} /* namespace sessiond */
} /* namespace lttng */

#endif /* LTTNG_UST_REGISTRY_SESSION_UID_H */
