/*
 * SPDX-FileCopyrightText: 2011 EfficiOS Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTTNG_UTILS_H
#define _LTTNG_UTILS_H

#include <common/container-wrapper.hpp>
#include <common/ctl/memory.hpp>
#include <common/dynamic-array.hpp>
#include <common/make-unique-wrapper.hpp>

#include <lttng/lttng.h>
#include <lttng/session-internal.hpp>

#include <vendor/argpar/argpar.h>

#include <iterator>
#include <memory>
#include <popt.h>

extern char *opt_relayd_path;
extern int opt_no_sessiond;
extern char *opt_sessiond_path;
extern pid_t sessiond_pid;

struct cmd_struct;

namespace lttng {
namespace cli {

struct session_spec {
	enum class type {
		NAME,
		GLOB_PATTERN,
		ALL,
	};

	explicit session_spec(type spec_type, const char *name_or_pattern = nullptr) noexcept :
		type_(spec_type), value(name_or_pattern)
	{
	}

	/* Disambiguate type enum from the member for buggy g++ versions. */
	type type_;
	const char *value;
};

lttng::ctl::session_list list_sessions(const struct session_spec& spec);
} /* namespace cli */
} /* namespace lttng */

char *get_session_name();
char *get_session_name_quiet();
void list_commands(struct cmd_struct *commands, FILE *ofp);
void list_cmd_options(FILE *ofp, struct poptOption *options);
void list_cmd_options_argpar(FILE *ofp, const struct argpar_opt_descr *options);

/*
 * Return the minimum order for which x <= (1UL << order).
 * Return -1 if x is 0.
 */
int get_count_order_u32(uint32_t x);

/*
 * Return the minimum order for which x <= (1UL << order).
 * Return -1 if x is 0.
 */
int get_count_order_u64(uint64_t x);

/*
 * Return the minimum order for which x <= (1UL << order).
 * Return -1 if x is 0.
 */
int get_count_order_ulong(unsigned long x);

const char *get_event_type_str(enum lttng_event_type event_type);

int print_missing_or_multiple_domains(unsigned int domain_count, bool include_agent_domains);

int spawn_relayd(const char *pathname, int port);
int check_relayd();
void print_session_stats(const char *session_name);
int get_session_stats_str(const char *session_name, char **str);
int show_cmd_help(const char *cmd_name, const char *help_msg);

int print_trace_archive_location(const struct lttng_trace_archive_location *location,
				 const char *session_name);

int validate_exclusion_list(const char *event_name,
			    const struct lttng_dynamic_pointer_array *exclusions);

/*
 * Ask the sessiond for the more details on the status of the kernel tracer and
 * print it to stderr.
 */
void print_kernel_tracer_status_error();

#endif /* _LTTNG_UTILS_H */
