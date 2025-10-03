/*
 * SPDX-FileCopyrightText: 2012 David Goulet <dgoulet@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef CMD_H
#define CMD_H

#include "context.hpp"
#include "ctl-utils.hpp"
#include "lttng-sessiond.hpp"
#include "lttng/tracker.h"
#include "session.hpp"
#include "snapshot-output.hpp"

#include <common/ctl/memory.hpp>
#include <common/trace-chunk.hpp>
#include <common/tracker.hpp>

#include <lttng/kernel.h>

struct notification_thread_handle;
struct lttng_dynamic_buffer;

/*
 * A callback (and associated user data) that should be run after a command
 * has been executed. No locks should be taken while executing this handler.
 *
 * The command's reply should not be sent until the handler has run and
 * completed successfully. On failure, the handler's return code should
 * be the only reply sent to the client.
 */
using completion_handler_function = enum lttng_error_code (*)(void *);
struct cmd_completion_handler {
	completion_handler_function run;
	void *data;
};

/*
 * Init the command subsystem. Must be called before using any of the functions
 * above. This is called in the main() of the session daemon.
 */
void cmd_init();

/* Session commands */
enum lttng_error_code cmd_create_session(struct command_ctx *cmd_ctx,
					 int sock,
					 struct lttng_session_descriptor **return_descriptor);
int cmd_destroy_session(const ltt_session::locked_ref& session, int *sock_fd);

/* Channel commands */
int cmd_disable_channel(const ltt_session::locked_ref& session,
			enum lttng_domain_type domain,
			char *channel_name);
int cmd_enable_channel(struct command_ctx *cmd_ctx,
		       ltt_session::locked_ref& session,
		       int sock,
		       int wpipe);

/* Process attribute tracker commands */
enum lttng_error_code
cmd_process_attr_tracker_get_tracking_policy(const ltt_session::locked_ref& session,
					     enum lttng_domain_type domain,
					     enum lttng_process_attr process_attr,
					     enum lttng_tracking_policy *policy);
enum lttng_error_code
cmd_process_attr_tracker_set_tracking_policy(const ltt_session::locked_ref& session,
					     enum lttng_domain_type domain,
					     enum lttng_process_attr process_attr,
					     enum lttng_tracking_policy policy);
enum lttng_error_code
cmd_process_attr_tracker_inclusion_set_add_value(const ltt_session::locked_ref& session,
						 enum lttng_domain_type domain,
						 enum lttng_process_attr process_attr,
						 const struct process_attr_value *value);
enum lttng_error_code
cmd_process_attr_tracker_inclusion_set_remove_value(const ltt_session::locked_ref& session,
						    enum lttng_domain_type domain,
						    enum lttng_process_attr process_attr,
						    const struct process_attr_value *value);
enum lttng_error_code
cmd_process_attr_tracker_get_inclusion_set(const ltt_session::locked_ref& session,
					   enum lttng_domain_type domain,
					   enum lttng_process_attr process_attr,
					   struct lttng_process_attr_values **values);

/* Event commands */
lttng_error_code cmd_disable_event(struct command_ctx *cmd_ctx,
				   ltt_session::locked_ref& locked_session,
				   struct lttng_event *event,
				   char *filter_expression,
				   struct lttng_bytecode *filter,
				   struct lttng_event_exclusion *exclusion,
				   lttng::event_rule_uptr event_rule);
int cmd_add_context(struct command_ctx *cmd_ctx,
		    ltt_session::locked_ref& locked_session,
		    const struct lttng_event_context *event_context,
		    int kwpipe);
int cmd_set_filter(struct ltt_session *session,
		   enum lttng_domain_type domain,
		   char *channel_name,
		   struct lttng_event *event,
		   struct lttng_bytecode *bytecode);
int cmd_enable_event(struct command_ctx *cmd_ctx,
		     ltt_session::locked_ref& session,
		     struct lttng_event *event,
		     char *filter_expression,
		     struct lttng_event_exclusion *exclusion,
		     struct lttng_bytecode *bytecode,
		     int wpipe,
		     lttng::event_rule_uptr event_rule);

/* Trace session action commands */
int cmd_start_trace(const ltt_session::locked_ref& session);
int cmd_stop_trace(const ltt_session::locked_ref& session);

/* Consumer commands */
int cmd_register_consumer(const ltt_session::locked_ref& session,
			  enum lttng_domain_type domain,
			  const char *sock_path,
			  struct consumer_data *cdata);
int cmd_set_consumer_uri(const ltt_session::locked_ref& session,
			 size_t nb_uri,
			 struct lttng_uri *uris);
int cmd_setup_relayd(const ltt_session::locked_ref& session);

/* Listing commands */
ssize_t cmd_list_domains(const ltt_session::locked_ref& session, struct lttng_domain **domains);
enum lttng_error_code cmd_list_events(enum lttng_domain_type domain,
				      const ltt_session::locked_ref& session,
				      char *channel_name,
				      struct lttng_payload *payload);
enum lttng_error_code cmd_list_channels(enum lttng_domain_type domain,
					const ltt_session::locked_ref& session,
					struct lttng_payload *payload);
void cmd_list_lttng_sessions(lttng_session *sessions, size_t session_count, uid_t uid, gid_t gid);
enum lttng_error_code cmd_list_tracepoint_fields(enum lttng_domain_type domain,
						 struct lttng_payload *reply);
enum lttng_error_code cmd_list_tracepoints(enum lttng_domain_type domain,
					   struct lttng_payload *reply_payload);
ssize_t cmd_snapshot_list_outputs(const ltt_session::locked_ref& session,
				  struct lttng_snapshot_output **outputs);
enum lttng_error_code cmd_list_syscalls(struct lttng_payload *reply_payload);

int cmd_data_pending(const ltt_session::locked_ref& session);
enum lttng_error_code cmd_kernel_tracer_status(enum lttng_kernel_tracer_status *status);

/* Snapshot */
int cmd_snapshot_add_output(const ltt_session::locked_ref& session,
			    const struct lttng_snapshot_output *output,
			    uint32_t *id);
int cmd_snapshot_del_output(const ltt_session::locked_ref& session,
			    const struct lttng_snapshot_output *output);
int cmd_snapshot_record(const ltt_session::locked_ref& session,
			const struct lttng_snapshot_output *output);

int cmd_set_session_shm_path(const ltt_session::locked_ref& session, const char *shm_path);
int cmd_regenerate_metadata(const ltt_session::locked_ref& session);
int cmd_regenerate_statedump(const ltt_session::locked_ref& session);

lttng::ctl::trigger
cmd_register_trigger(const struct lttng_credentials *cmd_creds,
		     struct lttng_trigger *trigger,
		     bool is_anonymous_trigger,
		     struct notification_thread_handle *notification_thread_handle);
enum lttng_error_code
cmd_unregister_trigger(const struct lttng_credentials *cmd_creds,
		       const struct lttng_trigger *trigger,
		       struct notification_thread_handle *notification_thread_handle);

enum lttng_error_code
cmd_list_triggers(struct command_ctx *cmd_ctx,
		  struct notification_thread_handle *notification_thread_handle,
		  struct lttng_triggers **return_triggers);
enum lttng_error_code
cmd_execute_error_query(const struct lttng_credentials *cmd_creds,
			const struct lttng_error_query *query,
			struct lttng_error_query_results **_results,
			struct notification_thread_handle *notification_thread);

int cmd_rotate_session(const ltt_session::locked_ref& session,
		       struct lttng_rotate_session_return *rotate_return,
		       bool quiet_rotation,
		       enum lttng_trace_chunk_command_type command);
int cmd_rotate_get_info(const ltt_session::locked_ref& session,
			struct lttng_rotation_get_info_return *info_return,
			uint64_t rotate_id);
int cmd_rotation_set_schedule(const ltt_session::locked_ref& session,
			      bool activate,
			      enum lttng_rotation_schedule_type schedule_type,
			      uint64_t value);

const struct cmd_completion_handler *cmd_pop_completion_handler();
int start_kernel_session(struct ltt_kernel_session *ksess);
int stop_kernel_session(struct ltt_kernel_session *ksess);

#endif /* CMD_H */
