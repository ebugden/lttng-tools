/*
 * SPDX-FileCopyrightText: 2012 David Goulet <dgoulet@efficios.com>
 * SPDX-FileCopyrightText: 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#define _LGPL_SOURCE
#include "agent-thread.hpp"
#include "agent.hpp"
#include "buffer-registry.hpp"
#include "channel.hpp"
#include "cmd.hpp"
#include "commands/get-channel-memory-usage.hpp"
#include "commands/reclaim-channel-memory.hpp"
#include "consumer-output.hpp"
#include "consumer.hpp"
#include "event-notifier-error-accounting.hpp"
#include "event.hpp"
#include "health-sessiond.hpp"
#include "kernel-consumer.hpp"
#include "kernel.hpp"
#include "lttng-sessiond.hpp"
#include "lttng-syscall.hpp"
#include "notification-thread-commands.hpp"
#include "notification-thread.hpp"
#include "recording-channel-configuration.hpp"
#include "rotation-thread.hpp"
#include "session.hpp"
#include "timer.hpp"
#include "tracker.hpp"
#include "utils.hpp"

#include <common/buffer-view.hpp>
#include <common/common.hpp>
#include <common/compat/string.hpp>
#include <common/ctl/format.hpp>
#include <common/defaults.hpp>
#include <common/dynamic-buffer.hpp>
#include <common/exception.hpp>
#include <common/kernel-ctl/kernel-ctl.hpp>
#include <common/make-unique-wrapper.hpp>
#include <common/math.hpp>
#include <common/payload-view.hpp>
#include <common/payload.hpp>
#include <common/relayd/relayd.hpp>
#include <common/scope-exit.hpp>
#include <common/sessiond-comm/sessiond-comm.hpp>
#include <common/string-utils/string-utils.hpp>
#include <common/trace-chunk.hpp>
#include <common/urcu.hpp>
#include <common/utils.hpp>

#include <lttng/action/action-internal.hpp>
#include <lttng/action/action.h>
#include <lttng/channel-internal.hpp>
#include <lttng/channel.h>
#include <lttng/condition/condition-internal.hpp>
#include <lttng/condition/condition.h>
#include <lttng/condition/event-rule-matches-internal.hpp>
#include <lttng/condition/event-rule-matches.h>
#include <lttng/error-query-internal.hpp>
#include <lttng/event-internal.hpp>
#include <lttng/event-rule/event-rule-internal.hpp>
#include <lttng/event-rule/event-rule.h>
#include <lttng/kernel.h>
#include <lttng/location-internal.hpp>
#include <lttng/lttng-error.h>
#include <lttng/rotate-internal.hpp>
#include <lttng/session-descriptor-internal.hpp>
#include <lttng/session-internal.hpp>
#include <lttng/tracker.h>
#include <lttng/trigger/trigger-internal.hpp>
#include <lttng/userspace-probe-internal.hpp>

#include <algorithm>
#include <inttypes.h>
#include <stdio.h>
#include <sys/stat.h>
#include <urcu/list.h>
#include <urcu/uatomic.h>

/* Sleep for 100ms between each check for the shm path's deletion. */
#define SESSION_DESTROY_SHM_PATH_CHECK_DELAY_US 100000

namespace ls = lttng::sessiond;
namespace lsu = lttng::sessiond::ust;

static enum lttng_error_code wait_on_path(void *path);

namespace {
struct cmd_destroy_session_reply_context {
	int reply_sock_fd;
	bool implicit_rotation_on_destroy;
	/*
	 * Indicates whether or not an error occurred while launching the
	 * destruction of a session.
	 */
	enum lttng_error_code destruction_status;
};

/*
 * Command completion handler that is used by the destroy command
 * when a session that has a non-default shm_path is being destroyed.
 *
 * See comment in cmd_destroy_session() for the rationale.
 */
struct destroy_completion_handler {
	struct cmd_completion_handler handler;
	char shm_path[member_sizeof(struct ltt_session, shm_path)];
} destroy_completion_handler = {
	.handler = { .run = wait_on_path, .data = destroy_completion_handler.shm_path },
	.shm_path = { 0 },
};

/*
 * Used to keep a unique index for each relayd socket created where this value
 * is associated with streams on the consumer so it can match the right relayd
 * to send to. It must be accessed with the relayd_net_seq_idx_lock
 * held.
 */
pthread_mutex_t relayd_net_seq_idx_lock = PTHREAD_MUTEX_INITIALIZER;
uint64_t relayd_net_seq_idx;
} /* namespace */

static struct cmd_completion_handler *current_completion_handler;
static int validate_ust_event_name(const char *);
static int cmd_enable_event_internal(ltt_session::locked_ref& session,
				     const struct lttng_domain *domain,
				     char *channel_name,
				     struct lttng_event *event,
				     char *filter_expression,
				     struct lttng_bytecode *filter,
				     struct lttng_event_exclusion *exclusion,
				     int wpipe,
				     lttng::event_rule_uptr event_rule);
static enum lttng_error_code cmd_enable_channel_internal(ltt_session::locked_ref& session,
							 const struct lttng_domain *domain,
							 const struct lttng_channel& channel_attr,
							 int wpipe);

/*
 * Create a session path used by list_lttng_sessions for the case that the
 * session consumer is on the network.
 */
static int
build_network_session_path(char *dst, size_t size, const ltt_session::locked_ref& session)
{
	int ret, kdata_port, udata_port;
	struct lttng_uri *kuri = nullptr, *uuri = nullptr, *uri = nullptr;
	char tmp_uurl[PATH_MAX], tmp_urls[PATH_MAX];

	LTTNG_ASSERT(dst);

	memset(tmp_urls, 0, sizeof(tmp_urls));
	memset(tmp_uurl, 0, sizeof(tmp_uurl));

	kdata_port = udata_port = DEFAULT_NETWORK_DATA_PORT;

	if (session->kernel_session && session->kernel_session->consumer) {
		kuri = &session->kernel_session->consumer->dst.net.control;
		kdata_port = session->kernel_session->consumer->dst.net.data.port;
	}

	if (session->ust_session && session->ust_session->consumer) {
		uuri = &session->ust_session->consumer->dst.net.control;
		udata_port = session->ust_session->consumer->dst.net.data.port;
	}

	if (uuri == nullptr && kuri == nullptr) {
		uri = &session->consumer->dst.net.control;
		kdata_port = session->consumer->dst.net.data.port;
	} else if (kuri && uuri) {
		ret = uri_compare(kuri, uuri);
		if (ret) {
			/* Not Equal */
			uri = kuri;
			/* Build uuri URL string */
			ret = uri_to_str_url(uuri, tmp_uurl, sizeof(tmp_uurl));
			if (ret < 0) {
				goto error;
			}
		} else {
			uri = kuri;
		}
	} else if (kuri && uuri == nullptr) {
		uri = kuri;
	} else if (uuri && kuri == nullptr) {
		uri = uuri;
	}

	ret = uri_to_str_url(uri, tmp_urls, sizeof(tmp_urls));
	if (ret < 0) {
		goto error;
	}

	/*
	 * Do we have a UST url set. If yes, this means we have both kernel and UST
	 * to print.
	 */
	if (*tmp_uurl != '\0') {
		ret = snprintf(dst,
			       size,
			       "[K]: %s [data: %d] -- [U]: %s [data: %d]",
			       tmp_urls,
			       kdata_port,
			       tmp_uurl,
			       udata_port);
	} else {
		int dport;
		if (kuri || (!kuri && !uuri)) {
			dport = kdata_port;
		} else {
			/* No kernel URI, use the UST port. */
			dport = udata_port;
		}
		ret = snprintf(dst, size, "%s [data: %d]", tmp_urls, dport);
	}

error:
	return ret;
}

/*
 * Get run-time attributes if the session has been started (discarded events,
 * lost packets).
 */
static int get_kernel_runtime_stats(const ltt_session::locked_ref& session,
				    struct ltt_kernel_channel *kchan,
				    uint64_t *discarded_events,
				    uint64_t *lost_packets)
{
	int ret;

	if (!session->has_been_started) {
		ret = 0;
		*discarded_events = 0;
		*lost_packets = 0;
		goto end;
	}

	ret = consumer_get_discarded_events(
		session->id, kchan->key, session->kernel_session->consumer, discarded_events);
	if (ret < 0) {
		goto end;
	}

	ret = consumer_get_lost_packets(
		session->id, kchan->key, session->kernel_session->consumer, lost_packets);
	if (ret < 0) {
		goto end;
	}

end:
	return ret;
}

/*
 * Get run-time attributes if the session has been started (discarded events,
 * lost packets).
 */
static int get_ust_runtime_stats(const ltt_session::locked_ref& session,
				 struct ltt_ust_channel *uchan,
				 uint64_t *discarded_events,
				 uint64_t *lost_packets)
{
	int ret;
	struct ltt_ust_session *usess;

	if (!discarded_events || !lost_packets) {
		ret = -1;
		goto end;
	}

	usess = session->ust_session;
	LTTNG_ASSERT(discarded_events);
	LTTNG_ASSERT(lost_packets);

	if (!usess || !session->has_been_started) {
		*discarded_events = 0;
		*lost_packets = 0;
		ret = 0;
		goto end;
	}

	if (usess->buffer_type == LTTNG_BUFFER_PER_UID) {
		ret = ust_app_uid_get_channel_runtime_stats(usess->id,
							    &usess->buffer_reg_uid_list,
							    usess->consumer,
							    uchan->id,
							    uchan->attr.overwrite,
							    discarded_events,
							    lost_packets);
	} else if (usess->buffer_type == LTTNG_BUFFER_PER_PID) {
		ret = ust_app_pid_get_channel_runtime_stats(usess,
							    uchan,
							    usess->consumer,
							    uchan->attr.overwrite,
							    discarded_events,
							    lost_packets);
		if (ret < 0) {
			goto end;
		}
		*discarded_events += uchan->per_pid_closed_app_discarded;
		*lost_packets += uchan->per_pid_closed_app_lost;
	} else {
		ERR("Unsupported buffer ownership");
		abort();
		ret = -1;
		goto end;
	}

end:
	return ret;
}

/*
 * Create a list of agent domain events.
 *
 * Return number of events in list on success or else a negative value.
 */
static enum lttng_error_code list_lttng_agent_events(struct agent *agt,
						     struct lttng_payload *reply_payload,
						     unsigned int *nb_events)
{
	enum lttng_error_code ret_code;
	int ret = 0;
	unsigned int local_nb_events = 0;
	unsigned long agent_event_count;

	assert(agt);
	assert(reply_payload);

	DBG3("Listing agent events");

	agent_event_count = lttng_ht_get_count(agt->events);
	if (agent_event_count == 0) {
		/* Early exit. */
		goto end;
	}

	if (agent_event_count > UINT_MAX) {
		ret_code = LTTNG_ERR_OVERFLOW;
		goto error;
	}

	local_nb_events = (unsigned int) agent_event_count;

	for (auto *event :
	     lttng::urcu::lfht_iteration_adapter<agent_event,
						 decltype(agent_event::node),
						 &agent_event::node>(*agt->events->ht)) {
		struct lttng_event *tmp_event = lttng_event_create();

		if (!tmp_event) {
			ret_code = LTTNG_ERR_NOMEM;
			goto error;
		}

		if (lttng_strncpy(tmp_event->name, event->name, sizeof(tmp_event->name))) {
			lttng_event_destroy(tmp_event);
			ret_code = LTTNG_ERR_FATAL;
			goto error;
		}

		tmp_event->name[sizeof(tmp_event->name) - 1] = '\0';
		tmp_event->enabled = !!event->enabled_count;
		tmp_event->loglevel = event->loglevel_value;
		tmp_event->loglevel_type = event->loglevel_type;

		ret = lttng_event_serialize(
			tmp_event, 0, nullptr, event->filter_expression, 0, nullptr, reply_payload);
		lttng_event_destroy(tmp_event);
		if (ret) {
			ret_code = LTTNG_ERR_FATAL;
			goto error;
		}
	}
end:
	ret_code = LTTNG_OK;
	*nb_events = local_nb_events;
error:
	return ret_code;
}

/*
 * Create a list of ust global domain events.
 */
static enum lttng_error_code list_lttng_ust_global_events(char *channel_name,
							  struct ltt_ust_domain_global *ust_global,
							  struct lttng_payload *reply_payload,
							  unsigned int *nb_events)
{
	enum lttng_error_code ret_code;
	int ret;
	struct lttng_ht_iter iter;
	struct lttng_ht_node_str *node;
	struct ltt_ust_channel *uchan;
	unsigned long channel_event_count;
	unsigned int local_nb_events = 0;

	assert(reply_payload);
	assert(nb_events);

	DBG("Listing UST global events for channel %s", channel_name);

	const lttng::urcu::read_lock_guard read_lock;

	lttng_ht_lookup(ust_global->channels, (void *) channel_name, &iter);
	node = lttng_ht_iter_get_node<lttng_ht_node_str>(&iter);
	if (node == nullptr) {
		ret_code = LTTNG_ERR_UST_CHAN_NOT_FOUND;
		goto error;
	}

	uchan = lttng::utils::container_of(node, &ltt_ust_channel::node);

	channel_event_count = lttng_ht_get_count(uchan->events);
	if (channel_event_count == 0) {
		/* Early exit. */
		ret_code = LTTNG_OK;
		goto end;
	}

	if (channel_event_count > UINT_MAX) {
		ret_code = LTTNG_ERR_OVERFLOW;
		goto error;
	}

	local_nb_events = (unsigned int) channel_event_count;

	DBG3("Listing UST global %d events", *nb_events);

	for (auto *uevent :
	     lttng::urcu::lfht_iteration_adapter<ltt_ust_event,
						 decltype(ltt_ust_event::node),
						 &ltt_ust_event::node>(*uchan->events->ht)) {
		struct lttng_event *tmp_event = nullptr;

		if (uevent->internal) {
			/* This event should remain hidden from clients */
			local_nb_events--;
			continue;
		}

		tmp_event = lttng_event_create();
		if (!tmp_event) {
			ret_code = LTTNG_ERR_NOMEM;
			goto error;
		}

		if (lttng_strncpy(tmp_event->name, uevent->attr.name, LTTNG_SYMBOL_NAME_LEN)) {
			ret_code = LTTNG_ERR_FATAL;
			lttng_event_destroy(tmp_event);
			goto error;
		}

		tmp_event->name[LTTNG_SYMBOL_NAME_LEN - 1] = '\0';
		tmp_event->enabled = uevent->enabled;

		switch (uevent->attr.instrumentation) {
		case LTTNG_UST_ABI_TRACEPOINT:
			tmp_event->type = LTTNG_EVENT_TRACEPOINT;
			break;
		case LTTNG_UST_ABI_PROBE:
			tmp_event->type = LTTNG_EVENT_PROBE;
			break;
		case LTTNG_UST_ABI_FUNCTION:
			tmp_event->type = LTTNG_EVENT_FUNCTION;
			break;
		}

		tmp_event->loglevel = uevent->attr.loglevel;
		switch (uevent->attr.loglevel_type) {
		case LTTNG_UST_ABI_LOGLEVEL_ALL:
			tmp_event->loglevel_type = LTTNG_EVENT_LOGLEVEL_ALL;
			break;
		case LTTNG_UST_ABI_LOGLEVEL_RANGE:
			tmp_event->loglevel_type = LTTNG_EVENT_LOGLEVEL_RANGE;
			break;
		case LTTNG_UST_ABI_LOGLEVEL_SINGLE:
			tmp_event->loglevel_type = LTTNG_EVENT_LOGLEVEL_SINGLE;
			break;
		}
		if (uevent->filter) {
			tmp_event->filter = 1;
		}
		if (uevent->exclusion) {
			tmp_event->exclusion = 1;
		}

		std::vector<const char *> exclusion_names;
		if (uevent->exclusion) {
			for (int i = 0; i < uevent->exclusion->count; i++) {
				exclusion_names.emplace_back(
					LTTNG_EVENT_EXCLUSION_NAME_AT(uevent->exclusion, i));
			}
		}

		/*
		 * We do not care about the filter bytecode and the fd from the
		 * userspace_probe_location.
		 */
		ret = lttng_event_serialize(tmp_event,
					    exclusion_names.size(),
					    exclusion_names.size() ? exclusion_names.data() :
								     nullptr,
					    uevent->filter_expression,
					    0,
					    nullptr,
					    reply_payload);
		lttng_event_destroy(tmp_event);
		if (ret) {
			ret_code = LTTNG_ERR_FATAL;
			goto error;
		}
	}

end:
	/* nb_events is already set at this point. */
	ret_code = LTTNG_OK;
	*nb_events = local_nb_events;
error:
	return ret_code;
}

/*
 * Fill lttng_event array of all kernel events in the channel.
 */
static enum lttng_error_code list_lttng_kernel_events(char *channel_name,
						      struct ltt_kernel_session *kernel_session,
						      struct lttng_payload *reply_payload,
						      unsigned int *nb_events)
{
	enum lttng_error_code ret_code;
	int ret;
	struct ltt_kernel_channel *kchan;

	assert(reply_payload);

	kchan = trace_kernel_get_channel_by_name(channel_name, kernel_session);
	if (kchan == nullptr) {
		ret_code = LTTNG_ERR_KERN_CHAN_NOT_FOUND;
		goto end;
	}

	*nb_events = kchan->event_count;

	DBG("Listing events for channel %s", kchan->channel->name);

	if (*nb_events == 0) {
		ret_code = LTTNG_OK;
		goto end;
	}

	/* Kernel channels */
	for (auto event :
	     lttng::urcu::list_iteration_adapter<ltt_kernel_event, &ltt_kernel_event::list>(
		     kchan->events_list.head)) {
		struct lttng_event *tmp_event = lttng_event_create();

		if (!tmp_event) {
			ret_code = LTTNG_ERR_NOMEM;
			goto end;
		}

		if (lttng_strncpy(tmp_event->name, event->event->name, LTTNG_SYMBOL_NAME_LEN)) {
			lttng_event_destroy(tmp_event);
			ret_code = LTTNG_ERR_FATAL;
			goto end;
		}

		tmp_event->name[LTTNG_SYMBOL_NAME_LEN - 1] = '\0';
		tmp_event->enabled = event->enabled;
		tmp_event->filter = (unsigned char) !!event->filter_expression;

		switch (event->event->instrumentation) {
		case LTTNG_KERNEL_ABI_TRACEPOINT:
			tmp_event->type = LTTNG_EVENT_TRACEPOINT;
			break;
		case LTTNG_KERNEL_ABI_KRETPROBE:
			tmp_event->type = LTTNG_EVENT_FUNCTION;
			memcpy(&tmp_event->attr.probe,
			       &event->event->u.kprobe,
			       sizeof(struct lttng_kernel_abi_kprobe));
			break;
		case LTTNG_KERNEL_ABI_KPROBE:
			tmp_event->type = LTTNG_EVENT_PROBE;
			memcpy(&tmp_event->attr.probe,
			       &event->event->u.kprobe,
			       sizeof(struct lttng_kernel_abi_kprobe));
			break;
		case LTTNG_KERNEL_ABI_UPROBE:
			tmp_event->type = LTTNG_EVENT_USERSPACE_PROBE;
			break;
		case LTTNG_KERNEL_ABI_FUNCTION:
			tmp_event->type = LTTNG_EVENT_FUNCTION;
			memcpy(&(tmp_event->attr.ftrace),
			       &event->event->u.ftrace,
			       sizeof(struct lttng_kernel_abi_function));
			break;
		case LTTNG_KERNEL_ABI_NOOP:
			tmp_event->type = LTTNG_EVENT_NOOP;
			break;
		case LTTNG_KERNEL_ABI_SYSCALL:
			tmp_event->type = LTTNG_EVENT_SYSCALL;
			break;
		case LTTNG_KERNEL_ABI_ALL:
			/* fall-through. */
		default:
			abort();
			break;
		}

		if (event->userspace_probe_location) {
			struct lttng_userspace_probe_location *location_copy =
				lttng_userspace_probe_location_copy(
					event->userspace_probe_location);

			if (!location_copy) {
				lttng_event_destroy(tmp_event);
				ret_code = LTTNG_ERR_NOMEM;
				goto end;
			}

			ret = lttng_event_set_userspace_probe_location(tmp_event, location_copy);
			if (ret) {
				lttng_event_destroy(tmp_event);
				lttng_userspace_probe_location_destroy(location_copy);
				ret_code = LTTNG_ERR_INVALID;
				goto end;
			}
		}

		ret = lttng_event_serialize(
			tmp_event, 0, nullptr, event->filter_expression, 0, nullptr, reply_payload);
		lttng_event_destroy(tmp_event);
		if (ret) {
			ret_code = LTTNG_ERR_FATAL;
			goto end;
		}
	}

	ret_code = LTTNG_OK;
end:
	return ret_code;
}

/*
 * Add URI so the consumer output object. Set the correct path depending on the
 * domain adding the default trace directory.
 */
static enum lttng_error_code add_uri_to_consumer(const ltt_session::locked_ref& session,
						 struct consumer_output *consumer,
						 struct lttng_uri *uri,
						 enum lttng_domain_type domain)
{
	int ret;
	enum lttng_error_code ret_code = LTTNG_OK;

	LTTNG_ASSERT(uri);

	if (consumer == nullptr) {
		DBG("No consumer detected. Don't add URI. Stopping.");
		ret_code = LTTNG_ERR_NO_CONSUMER;
		goto error;
	}

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		ret = lttng_strncpy(consumer->domain_subdir,
				    DEFAULT_KERNEL_TRACE_DIR,
				    sizeof(consumer->domain_subdir));
		break;
	case LTTNG_DOMAIN_UST:
		ret = lttng_strncpy(consumer->domain_subdir,
				    DEFAULT_UST_TRACE_DIR,
				    sizeof(consumer->domain_subdir));
		break;
	default:
		/*
		 * This case is possible is we try to add the URI to the global
		 * tracing session consumer object which in this case there is
		 * no subdir.
		 */
		memset(consumer->domain_subdir, 0, sizeof(consumer->domain_subdir));
		ret = 0;
	}
	if (ret) {
		ERR("Failed to initialize consumer output domain subdirectory");
		ret_code = LTTNG_ERR_FATAL;
		goto error;
	}

	switch (uri->dtype) {
	case LTTNG_DST_IPV4:
	case LTTNG_DST_IPV6:
		DBG2("Setting network URI to consumer");

		if (consumer->type == CONSUMER_DST_NET) {
			if ((uri->stype == LTTNG_STREAM_CONTROL &&
			     consumer->dst.net.control_isset) ||
			    (uri->stype == LTTNG_STREAM_DATA && consumer->dst.net.data_isset)) {
				ret_code = LTTNG_ERR_URL_EXIST;
				goto error;
			}
		} else {
			memset(&consumer->dst, 0, sizeof(consumer->dst));
		}

		/* Set URI into consumer output object */
		ret = consumer_set_network_uri(session, consumer, uri);
		if (ret < 0) {
			ret_code = (lttng_error_code) -ret;
			goto error;
		} else if (ret == 1) {
			/*
			 * URI was the same in the consumer so we do not append the subdir
			 * again so to not duplicate output dir.
			 */
			ret_code = LTTNG_OK;
			goto error;
		}
		break;
	case LTTNG_DST_PATH:
		if (*uri->dst.path != '/' || strstr(uri->dst.path, "../")) {
			ret_code = LTTNG_ERR_INVALID;
			goto error;
		}
		DBG2("Setting trace directory path from URI to %s", uri->dst.path);
		memset(&consumer->dst, 0, sizeof(consumer->dst));

		ret = lttng_strncpy(consumer->dst.session_root_path,
				    uri->dst.path,
				    sizeof(consumer->dst.session_root_path));
		if (ret) {
			ret_code = LTTNG_ERR_FATAL;
			goto error;
		}
		consumer->type = CONSUMER_DST_LOCAL;
		break;
	}

	ret_code = LTTNG_OK;
error:
	return ret_code;
}

/*
 * Init tracing by creating trace directory and sending fds kernel consumer.
 */
static int init_kernel_tracing(struct ltt_kernel_session *session)
{
	int ret = 0;

	LTTNG_ASSERT(session);

	if (session->consumer_fds_sent == 0 && session->consumer != nullptr) {
		for (auto *socket :
		     lttng::urcu::lfht_iteration_adapter<consumer_socket,
							 decltype(consumer_socket::node),
							 &consumer_socket::node>(
			     *session->consumer->socks->ht)) {
			pthread_mutex_lock(socket->lock);
			ret = kernel_consumer_send_session(socket, session);
			pthread_mutex_unlock(socket->lock);
			if (ret < 0) {
				ret = LTTNG_ERR_KERN_CONSUMER_FAIL;
				goto error;
			}
		}
	}

error:
	return ret;
}

/*
 * Create a socket to the relayd using the URI.
 *
 * On success, the relayd_sock pointer is set to the created socket.
 * Else, it remains untouched and an LTTng error code is returned.
 */
static enum lttng_error_code create_connect_relayd(struct lttng_uri *uri,
						   struct lttcomm_relayd_sock **relayd_sock,
						   struct consumer_output *consumer)
{
	int ret;
	enum lttng_error_code status = LTTNG_OK;
	struct lttcomm_relayd_sock *rsock;

	rsock = lttcomm_alloc_relayd_sock(
		uri, RELAYD_VERSION_COMM_MAJOR, RELAYD_VERSION_COMM_MINOR);
	if (!rsock) {
		status = LTTNG_ERR_FATAL;
		goto error;
	}

	/*
	 * Connect to relayd so we can proceed with a session creation. This call
	 * can possibly block for an arbitrary amount of time to set the health
	 * state to be in poll execution.
	 */
	health_poll_entry();
	ret = relayd_connect(rsock);
	health_poll_exit();
	if (ret < 0) {
		ERR("Unable to reach lttng-relayd");
		status = LTTNG_ERR_RELAYD_CONNECT_FAIL;
		goto free_sock;
	}

	/* Create socket for control stream. */
	if (uri->stype == LTTNG_STREAM_CONTROL) {
		uint64_t result_flags;

		DBG3("Creating relayd stream socket from URI");

		/* Check relayd version */
		ret = relayd_version_check(rsock);
		if (ret == LTTNG_ERR_RELAYD_VERSION_FAIL) {
			status = LTTNG_ERR_RELAYD_VERSION_FAIL;
			goto close_sock;
		} else if (ret < 0) {
			ERR("Unable to reach lttng-relayd");
			status = LTTNG_ERR_RELAYD_CONNECT_FAIL;
			goto close_sock;
		}
		consumer->relay_major_version = rsock->major;
		consumer->relay_minor_version = rsock->minor;
		ret = relayd_get_configuration(rsock, 0, &result_flags);
		if (ret < 0) {
			ERR("Unable to get relayd configuration");
			status = LTTNG_ERR_RELAYD_CONNECT_FAIL;
			goto close_sock;
		}
		if (result_flags & LTTCOMM_RELAYD_CONFIGURATION_FLAG_CLEAR_ALLOWED) {
			consumer->relay_allows_clear = true;
		}
	} else if (uri->stype == LTTNG_STREAM_DATA) {
		DBG3("Creating relayd data socket from URI");
	} else {
		/* Command is not valid */
		ERR("Relayd invalid stream type: %d", uri->stype);
		status = LTTNG_ERR_INVALID;
		goto close_sock;
	}

	*relayd_sock = rsock;

	return status;

close_sock:
	/* The returned value is not useful since we are on an error path. */
	(void) relayd_close(rsock);
free_sock:
	free(rsock);
error:
	return status;
}

/*
 * Connect to the relayd using URI and send the socket to the right consumer.
 *
 * The consumer socket lock must be held by the caller.
 *
 * Returns LTTNG_OK on success or an LTTng error code on failure.
 */
static enum lttng_error_code send_consumer_relayd_socket(unsigned int session_id,
							 struct lttng_uri *relayd_uri,
							 struct consumer_output *consumer,
							 struct consumer_socket *consumer_sock,
							 const char *session_name,
							 const char *hostname,
							 const char *base_path,
							 int session_live_timer,
							 const uint64_t *current_chunk_id,
							 time_t session_creation_time,
							 bool session_name_contains_creation_time)
{
	int ret;
	struct lttcomm_relayd_sock *rsock = nullptr;
	enum lttng_error_code status;

	/* Connect to relayd and make version check if uri is the control. */
	status = create_connect_relayd(relayd_uri, &rsock, consumer);
	if (status != LTTNG_OK) {
		goto relayd_comm_error;
	}
	LTTNG_ASSERT(rsock);

	/* Set the network sequence index if not set. */
	if (consumer->net_seq_index == (uint64_t) -1ULL) {
		pthread_mutex_lock(&relayd_net_seq_idx_lock);
		/*
		 * Increment net_seq_idx because we are about to transfer the
		 * new relayd socket to the consumer.
		 * Assign unique key so the consumer can match streams.
		 */
		consumer->net_seq_index = ++relayd_net_seq_idx;
		pthread_mutex_unlock(&relayd_net_seq_idx_lock);
	}

	/* Send relayd socket to consumer. */
	ret = consumer_send_relayd_socket(consumer_sock,
					  rsock,
					  consumer,
					  relayd_uri->stype,
					  session_id,
					  session_name,
					  hostname,
					  base_path,
					  session_live_timer,
					  current_chunk_id,
					  session_creation_time,
					  session_name_contains_creation_time);
	if (ret < 0) {
		status = LTTNG_ERR_ENABLE_CONSUMER_FAIL;
		goto close_sock;
	}

	/* Flag that the corresponding socket was sent. */
	if (relayd_uri->stype == LTTNG_STREAM_CONTROL) {
		consumer_sock->control_sock_sent = 1;
	} else if (relayd_uri->stype == LTTNG_STREAM_DATA) {
		consumer_sock->data_sock_sent = 1;
	}

	/*
	 * Close socket which was dup on the consumer side. The session daemon does
	 * NOT keep track of the relayd socket(s) once transfer to the consumer.
	 */

close_sock:
	if (status != LTTNG_OK) {
		/*
		 * The consumer output for this session should not be used anymore
		 * since the relayd connection failed thus making any tracing or/and
		 * streaming not usable.
		 */
		consumer->enabled = false;
	}
	(void) relayd_close(rsock);
	free(rsock);

relayd_comm_error:
	return status;
}

/*
 * Send both relayd sockets to a specific consumer and domain.  This is a
 * helper function to facilitate sending the information to the consumer for a
 * session.
 *
 * The consumer socket lock must be held by the caller.
 *
 * Returns LTTNG_OK, or an LTTng error code on failure.
 */
static enum lttng_error_code send_consumer_relayd_sockets(unsigned int session_id,
							  struct consumer_output *consumer,
							  struct consumer_socket *sock,
							  const char *session_name,
							  const char *hostname,
							  const char *base_path,
							  int session_live_timer,
							  const uint64_t *current_chunk_id,
							  time_t session_creation_time,
							  bool session_name_contains_creation_time)
{
	enum lttng_error_code status = LTTNG_OK;

	LTTNG_ASSERT(consumer);
	LTTNG_ASSERT(sock);

	/* Sending control relayd socket. */
	if (!sock->control_sock_sent) {
		status = send_consumer_relayd_socket(session_id,
						     &consumer->dst.net.control,
						     consumer,
						     sock,
						     session_name,
						     hostname,
						     base_path,
						     session_live_timer,
						     current_chunk_id,
						     session_creation_time,
						     session_name_contains_creation_time);
		if (status != LTTNG_OK) {
			goto error;
		}
	}

	/* Sending data relayd socket. */
	if (!sock->data_sock_sent) {
		status = send_consumer_relayd_socket(session_id,
						     &consumer->dst.net.data,
						     consumer,
						     sock,
						     session_name,
						     hostname,
						     base_path,
						     session_live_timer,
						     current_chunk_id,
						     session_creation_time,
						     session_name_contains_creation_time);
		if (status != LTTNG_OK) {
			goto error;
		}
	}

error:
	return status;
}

/*
 * Setup relayd connections for a tracing session. First creates the socket to
 * the relayd and send them to the right domain consumer. Consumer type MUST be
 * network.
 */
int cmd_setup_relayd(const ltt_session::locked_ref& session)
{
	int ret = LTTNG_OK;
	struct ltt_ust_session *usess;
	struct ltt_kernel_session *ksess;
	LTTNG_OPTIONAL(uint64_t) current_chunk_id = {};

	usess = session->ust_session;
	ksess = session->kernel_session;

	DBG("Setting relayd for session %s", session->name);

	if (session->current_trace_chunk) {
		const lttng_trace_chunk_status status = lttng_trace_chunk_get_id(
			session->current_trace_chunk, &current_chunk_id.value);

		if (status == LTTNG_TRACE_CHUNK_STATUS_OK) {
			current_chunk_id.is_set = true;
		} else {
			ERR("Failed to get current trace chunk id");
			ret = LTTNG_ERR_UNK;
			goto error;
		}
	}

	if (usess && usess->consumer && usess->consumer->type == CONSUMER_DST_NET &&
	    usess->consumer->enabled) {
		/* For each consumer socket, send relayd sockets */
		for (auto *socket :
		     lttng::urcu::lfht_iteration_adapter<consumer_socket,
							 decltype(consumer_socket::node),
							 &consumer_socket::node>(
			     *usess->consumer->socks->ht)) {
			pthread_mutex_lock(socket->lock);
			ret = send_consumer_relayd_sockets(
				session->id,
				usess->consumer,
				socket,
				session->name,
				session->hostname,
				session->base_path,
				session->live_timer,
				current_chunk_id.is_set ? &current_chunk_id.value : nullptr,
				session->creation_time,
				session->name_contains_creation_time);
			pthread_mutex_unlock(socket->lock);
			if (ret != LTTNG_OK) {
				goto error;
			}
			/* Session is now ready for network streaming. */
			session->net_handle = 1;
		}

		session->consumer->relay_major_version = usess->consumer->relay_major_version;
		session->consumer->relay_minor_version = usess->consumer->relay_minor_version;
		session->consumer->relay_allows_clear = usess->consumer->relay_allows_clear;
	}

	if (ksess && ksess->consumer && ksess->consumer->type == CONSUMER_DST_NET &&
	    ksess->consumer->enabled) {
		const lttng::urcu::read_lock_guard read_lock;

		for (auto *socket :
		     lttng::urcu::lfht_iteration_adapter<consumer_socket,
							 decltype(consumer_socket::node),
							 &consumer_socket::node>(
			     *ksess->consumer->socks->ht)) {
			pthread_mutex_lock(socket->lock);
			ret = send_consumer_relayd_sockets(
				session->id,
				ksess->consumer,
				socket,
				session->name,
				session->hostname,
				session->base_path,
				session->live_timer,
				current_chunk_id.is_set ? &current_chunk_id.value : nullptr,
				session->creation_time,
				session->name_contains_creation_time);
			pthread_mutex_unlock(socket->lock);
			if (ret != LTTNG_OK) {
				goto error;
			}
			/* Session is now ready for network streaming. */
			session->net_handle = 1;
		}

		session->consumer->relay_major_version = ksess->consumer->relay_major_version;
		session->consumer->relay_minor_version = ksess->consumer->relay_minor_version;
		session->consumer->relay_allows_clear = ksess->consumer->relay_allows_clear;
	}

error:
	return ret;
}

/*
 * Start a kernel session by opening all necessary streams.
 */
int start_kernel_session(struct ltt_kernel_session *ksess)
{
	int ret;

	/* Open kernel metadata */
	if (ksess->metadata == nullptr && ksess->output_traces) {
		ret = kernel_open_metadata(ksess);
		if (ret < 0) {
			ret = LTTNG_ERR_KERN_META_FAIL;
			goto error;
		}
	}

	/* Open kernel metadata stream */
	if (ksess->metadata && ksess->metadata_stream_fd < 0) {
		ret = kernel_open_metadata_stream(ksess);
		if (ret < 0) {
			ERR("Kernel create metadata stream failed");
			ret = LTTNG_ERR_KERN_STREAM_FAIL;
			goto error;
		}
	}

	/* For each channel */
	for (auto kchan :
	     lttng::urcu::list_iteration_adapter<ltt_kernel_channel, &ltt_kernel_channel::list>(
		     ksess->channel_list.head)) {
		if (kchan->stream_count == 0) {
			ret = kernel_open_channel_stream(kchan);
			if (ret < 0) {
				ret = LTTNG_ERR_KERN_STREAM_FAIL;
				goto error;
			}
			/* Update the stream global counter */
			ksess->stream_count_global += ret;
		}
	}

	/* Setup kernel consumer socket and send fds to it */
	ret = init_kernel_tracing(ksess);
	if (ret != 0) {
		ret = LTTNG_ERR_KERN_START_FAIL;
		goto error;
	}

	/* This start the kernel tracing */
	ret = kernel_start_session(ksess);
	if (ret < 0) {
		ret = LTTNG_ERR_KERN_START_FAIL;
		goto error;
	}

	/* Quiescent wait after starting trace */
	kernel_wait_quiescent();

	ksess->active = true;

	ret = LTTNG_OK;

error:
	return ret;
}

int stop_kernel_session(struct ltt_kernel_session *ksess)
{
	bool error_occurred = false;
	int ret;

	if (!ksess || !ksess->active) {
		return LTTNG_OK;
	}
	DBG("Stopping kernel tracing");

	ret = kernel_stop_session(ksess);
	if (ret < 0) {
		ret = LTTNG_ERR_KERN_STOP_FAIL;
		goto error;
	}

	kernel_wait_quiescent();

	/* Flush metadata after stopping (if exists) */
	if (ksess->metadata_stream_fd >= 0) {
		ret = kernel_metadata_flush_buffer(ksess->metadata_stream_fd);
		if (ret < 0) {
			ERR("Kernel metadata flush failed");
			error_occurred = true;
		}
	}

	/* Flush all buffers after stopping */
	for (auto kchan :
	     lttng::urcu::list_iteration_adapter<ltt_kernel_channel, &ltt_kernel_channel::list>(
		     ksess->channel_list.head)) {
		ret = kernel_flush_buffer(kchan);
		if (ret < 0) {
			ERR("Kernel flush buffer error");
			error_occurred = true;
		}
	}

	ksess->active = false;
	if (error_occurred) {
		ret = LTTNG_ERR_UNK;
	} else {
		ret = LTTNG_OK;
	}
error:
	return ret;
}

namespace {
lttng::sessiond::domain_class
get_domain_class_from_ctl_domain_type(enum lttng_domain_type domain_type)
{
	switch (domain_type) {
	case LTTNG_DOMAIN_KERNEL:
		return lttng::sessiond::domain_class::KERNEL_SPACE;
	case LTTNG_DOMAIN_UST:
		return lttng::sessiond::domain_class::USER_SPACE;
	case LTTNG_DOMAIN_JUL:
		return lttng::sessiond::domain_class::JAVA_UTIL_LOGGING;
	case LTTNG_DOMAIN_LOG4J:
		return lttng::sessiond::domain_class::LOG4J;
	case LTTNG_DOMAIN_PYTHON:
		return lttng::sessiond::domain_class::PYTHON_LOGGING;
	case LTTNG_DOMAIN_LOG4J2:
		return lttng::sessiond::domain_class::LOG4J2;
	default:
		LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
			"No suitable conversion exists from lttng_domain_type enum to lttng::sessiond::domain_class: domain={}",
			domain_type));
	}
}
} /* namespace */

/*
 * Command LTTNG_DISABLE_CHANNEL processed by the client thread.
 */
int cmd_disable_channel(const ltt_session::locked_ref& session,
			enum lttng_domain_type domain,
			char *channel_name)
{
	ls::domain& target_domain =
		session->get_domain(get_domain_class_from_ctl_domain_type(domain));

	/* Throws if not found. */
	auto& channel_config = target_domain.get_channel(channel_name);

	const lttng::urcu::read_lock_guard read_lock;

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
	{
		const auto disable_ret =
			channel_kernel_disable(session->kernel_session, channel_name);
		if (disable_ret != LTTNG_OK) {
			return disable_ret;
		}

		kernel_wait_quiescent();
		break;
	}
	case LTTNG_DOMAIN_UST:
	{
		const auto usess = session->ust_session;
		const auto chan_ht = usess->domain_global.channels;
		const auto uchan = trace_ust_find_channel_by_name(chan_ht, channel_name);

		if (uchan == nullptr) {
			return LTTNG_ERR_UST_CHAN_NOT_FOUND;
		}

		const auto disable_ret = channel_ust_disable(usess, uchan);
		if (disable_ret != LTTNG_OK) {
			return disable_ret;
		}

		break;
	}
	default:
		return LTTNG_ERR_UNKNOWN_DOMAIN;
	}

	channel_config.disable();

	return LTTNG_OK;
}

/*
 * Command LTTNG_ENABLE_CHANNEL processed by the client thread.
 *
 * The wpipe arguments is used as a notifier for the kernel thread.
 */
int cmd_enable_channel(command_ctx *cmd_ctx, ltt_session::locked_ref& session, int sock, int wpipe)
{
	const struct lttng_domain command_domain = cmd_ctx->lsm.domain;

	if (command_domain.type == LTTNG_DOMAIN_NONE) {
		return LTTNG_ERR_INVALID;
	}

	/* Free buffer contents on function exit using a scope_exit. */
	lttng_dynamic_buffer channel_buffer;
	lttng_dynamic_buffer_init(&channel_buffer);
	auto destroy_buffer = lttng::make_scope_exit(
		[&channel_buffer]() noexcept { lttng_dynamic_buffer_reset(&channel_buffer); });

	const size_t channel_len = (size_t) cmd_ctx->lsm.u.channel.length;

	const int buffer_alloc_ret = lttng_dynamic_buffer_set_size(&channel_buffer, channel_len);
	if (buffer_alloc_ret) {
		return LTTNG_ERR_NOMEM;
	}

	const auto sock_recv_len = lttcomm_recv_unix_sock(sock, channel_buffer.data, channel_len);
	if (sock_recv_len < 0 || sock_recv_len != channel_len) {
		ERR("Failed to receive \"enable channel\" command payload");
		return LTTNG_ERR_INVALID;
	}

	auto view = lttng_buffer_view_from_dynamic_buffer(&channel_buffer, 0, channel_len);
	if (!lttng_buffer_view_is_valid(&view)) {
		/* lttng_buffer_view_from_dynamic_buffer already logs on error. */
		return LTTNG_ERR_INVALID;
	}

	auto channel = lttng::make_unique_wrapper<lttng_channel,
						  lttng_channel_destroy>([&view, channel_len]() {
		lttng_channel *raw_channel = nullptr;

		if (lttng_channel_create_from_buffer(&view, &raw_channel) != channel_len) {
			LTTNG_THROW_PROTOCOL_ERROR(
				"Invalid channel payload received in \"enable channel\" command");
		}

		return raw_channel;
	}());

	const auto cmd_ret = cmd_enable_channel_internal(session, &command_domain, *channel, wpipe);
	if (cmd_ret != LTTNG_OK) {
		return cmd_ret;
	}

	return LTTNG_OK;
}

static enum lttng_error_code cmd_enable_channel_internal(ltt_session::locked_ref& session,
							 const struct lttng_domain *domain,
							 const struct lttng_channel& channel_attr,
							 int wpipe)
{
	enum lttng_error_code ret_code;
	struct ltt_ust_session *usess = session->ust_session;
	struct lttng_ht *chan_ht;
	size_t len;

	LTTNG_ASSERT(domain);

	const lttng::urcu::read_lock_guard read_lock;

	auto new_channel_attr = lttng::make_unique_wrapper<lttng_channel, lttng_channel_destroy>(
		lttng_channel_copy(&channel_attr));
	if (!new_channel_attr) {
		return LTTNG_ERR_NOMEM;
	}

	len = lttng_strnlen(new_channel_attr->name, sizeof(new_channel_attr->name));

	/* Validate channel name */
	if (new_channel_attr->name[0] == '.' ||
	    memchr(new_channel_attr->name, '/', len) != nullptr) {
		return LTTNG_ERR_INVALID_CHANNEL_NAME;
	}

	DBG("Enabling channel %s for session %s", new_channel_attr->name, session->name);

	/*
	 * If the session is a live session, remove the switch timer, the
	 * live timer does the same thing but sends also synchronisation
	 * beacons for inactive streams.
	 */
	if (session->live_timer > 0) {
		new_channel_attr->attr.live_timer_interval = session->live_timer;
		new_channel_attr->attr.switch_timer_interval = 0;
	}

	/* Check for feature support */
	const auto extended = reinterpret_cast<const struct lttng_channel_extended *>(
		new_channel_attr->attr.extended.ptr);

	if (extended->watchdog_timer_interval.is_set) {
		if (domain->type != LTTNG_DOMAIN_UST) {
			WARN_FMT("Watchdog timer is only supported by UST domain: "
				 "session_name=`{}` channel_name=`{}` domain_type={}",
				 session->name,
				 new_channel_attr->name,
				 domain->type);
			return LTTNG_ERR_UNSUPPORTED_DOMAIN;
		}

		switch (domain->buf_type) {
		case LTTNG_BUFFER_PER_UID:
			break;
		default:
			WARN_FMT(
				"Userspace domain only support watchdog timer for `user` buffer-ownership: "
				"session_name=`{}` channel_name=`{}` buffer_ownership=`{}`",
				session->name,
				new_channel_attr->name,
				domain->buf_type == LTTNG_BUFFER_PER_PID	? "process" :
					domain->buf_type == LTTNG_BUFFER_GLOBAL ? "global" :
										  "unknown");
			return LTTNG_ERR_UNSUPPORTED_DOMAIN;
		}
	}

	switch (domain->type) {
	case LTTNG_DOMAIN_KERNEL:
	{
		if (kernel_supports_ring_buffer_snapshot_sample_positions() != 1) {
			/* Sampling position of buffer is not supported */
			WARN("Kernel tracer does not support buffer monitoring. "
			     "Setting the monitor interval timer to 0 "
			     "(disabled) for channel '%s' of session '%s'",
			     new_channel_attr->name,
			     session->name);
			lttng_channel_set_monitor_timer_interval(new_channel_attr.get(), 0);
		}

		break;
	}
	case LTTNG_DOMAIN_UST:
		break;
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_PYTHON:
		if (!agent_tracing_is_enabled()) {
			DBG("Attempted to enable a channel in an agent domain but the agent thread is not running");
			return LTTNG_ERR_AGENT_TRACING_DISABLED;
		}

		break;
	default:
		return LTTNG_ERR_UNKNOWN_DOMAIN;
	}

	switch (domain->type) {
	case LTTNG_DOMAIN_KERNEL:
	{
		struct ltt_kernel_channel *kchan;

		kchan = trace_kernel_get_channel_by_name(new_channel_attr->name,
							 session->kernel_session);
		if (kchan == nullptr) {
			/*
			 * Don't try to create a channel if the session has been started at
			 * some point in time before. The tracer does not allow it.
			 */
			if (session->has_been_started) {
				return LTTNG_ERR_TRACE_ALREADY_STARTED;
			}

			if (session->snapshot.nb_output > 0 || session->snapshot_mode) {
				/* Enforce mmap output for snapshot sessions. */
				new_channel_attr->attr.output = LTTNG_EVENT_MMAP;
			}

			ret_code = channel_kernel_create(
				session->kernel_session, new_channel_attr.get(), wpipe);
			if (new_channel_attr->name[0] != '\0') {
				session->kernel_session->has_non_default_channel = 1;
			}
		} else {
			ret_code = channel_kernel_enable(session->kernel_session, kchan);
		}

		if (ret_code != LTTNG_OK) {
			return ret_code;
		}

		kernel_wait_quiescent();
		break;
	}
	case LTTNG_DOMAIN_UST:
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_PYTHON:
	{
		struct ltt_ust_channel *uchan;

		/*
		 * FIXME
		 *
		 * Current agent implementation limitations force us to allow
		 * only one channel at once in "agent" subdomains. Each
		 * subdomain has a default channel name which must be strictly
		 * adhered to.
		 */
		if (domain->type == LTTNG_DOMAIN_JUL) {
			if (strncmp(new_channel_attr->name,
				    DEFAULT_JUL_CHANNEL_NAME,
				    LTTNG_SYMBOL_NAME_LEN - 1) != 0) {
				return LTTNG_ERR_INVALID_CHANNEL_NAME;
			}
		} else if (domain->type == LTTNG_DOMAIN_LOG4J) {
			if (strncmp(new_channel_attr->name,
				    DEFAULT_LOG4J_CHANNEL_NAME,
				    LTTNG_SYMBOL_NAME_LEN - 1) != 0) {
				return LTTNG_ERR_INVALID_CHANNEL_NAME;
			}
		} else if (domain->type == LTTNG_DOMAIN_LOG4J2) {
			if (strncmp(new_channel_attr->name,
				    DEFAULT_LOG4J2_CHANNEL_NAME,
				    LTTNG_SYMBOL_NAME_LEN - 1) != 0) {
				return LTTNG_ERR_INVALID_CHANNEL_NAME;
			}
		} else if (domain->type == LTTNG_DOMAIN_PYTHON) {
			if (strncmp(new_channel_attr->name,
				    DEFAULT_PYTHON_CHANNEL_NAME,
				    LTTNG_SYMBOL_NAME_LEN - 1) != 0) {
				return LTTNG_ERR_INVALID_CHANNEL_NAME;
			}
		}

		chan_ht = usess->domain_global.channels;

		uchan = trace_ust_find_channel_by_name(chan_ht, new_channel_attr->name);
		if (uchan == nullptr) {
			/*
			 * Don't try to create a channel if the session has been started at
			 * some point in time before. The tracer does not allow it.
			 */
			if (session->has_been_started) {
				return LTTNG_ERR_TRACE_ALREADY_STARTED;
			}

			ret_code =
				channel_ust_create(usess, new_channel_attr.get(), domain->buf_type);
			if (new_channel_attr->name[0] != '\0') {
				usess->has_non_default_channel = 1;
			}

			/*
			 * Implicitly add "cpu_id" context to UST domain channels with the
			 * "per-cpu" allocation policy on creation.
			 */
			if (ret_code != LTTNG_OK) {
				return ret_code;
			}

			enum lttng_channel_allocation_policy allocation_policy;
			enum lttng_error_code err = lttng_channel_get_allocation_policy(
				new_channel_attr.get(), &allocation_policy);

			if ((err == LTTNG_OK) &&
			    (allocation_policy == LTTNG_CHANNEL_ALLOCATION_POLICY_PER_CPU)) {
				struct lttng_event_context cpu_id_ctx;
				cpu_id_ctx.ctx = LTTNG_EVENT_CONTEXT_CPU_ID;

				err = static_cast<enum lttng_error_code>(context_ust_add(
					usess, domain->type, &cpu_id_ctx, new_channel_attr->name));
				if (err != LTTNG_OK) {
					ret_code = err;
				}
			}
		} else {
			ret_code = channel_ust_enable(usess, uchan);
		}

		break;
	}
	default:
		return LTTNG_ERR_UNKNOWN_DOMAIN;
	}

	if (ret_code == LTTNG_OK && new_channel_attr->attr.output != LTTNG_EVENT_MMAP) {
		session->has_non_mmap_channel = true;
	}

	const auto subbuffer_count = channel_attr.attr.num_subbuf;

	const auto switch_timer_period_us = [&channel_attr]() {
		return channel_attr.attr.switch_timer_interval > 0 ?
			decltype(ls::recording_channel_configuration::switch_timer_period_us)(
				channel_attr.attr.switch_timer_interval) :
			nonstd::nullopt;
	}();

	const auto read_timer_period_us = [&channel_attr]() {
		return channel_attr.attr.read_timer_interval > 0 ?
			decltype(ls::recording_channel_configuration::read_timer_period_us)(
				channel_attr.attr.read_timer_interval) :
			nonstd::nullopt;
	}();

	const auto live_timer_period_us = [&channel_attr]() {
		return channel_attr.attr.live_timer_interval > 0 ?
			decltype(ls::recording_channel_configuration::live_timer_period_us)(
				channel_attr.attr.live_timer_interval) :
			nonstd::nullopt;
	}();

	const auto monitor_timer_period_us = [&channel_attr]() {
		std::uint64_t period;
		const int ret = lttng_channel_get_monitor_timer_interval(&channel_attr, &period);

		if (ret) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to retrieve monitor timer period from channel: channel_name=`{}`",
				channel_attr.name));
		}

		return period > 0 ?
			decltype(ls::recording_channel_configuration::monitor_timer_period_us)(
				period) :
			nonstd::nullopt;
	}();

	const auto watchdog_timer_period_us = [&channel_attr]() {
		std::uint64_t period;
		const lttng_channel_get_watchdog_timer_interval_status status =
			lttng_channel_get_watchdog_timer_interval(&channel_attr, &period);

		if (status == LTTNG_CHANNEL_GET_WATCHDOG_TIMER_INTERVAL_STATUS_INVALID) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to retrieve watchdog timer period from channel: channel_name=`{}`",
				channel_attr.name));
		}

		return status == LTTNG_CHANNEL_GET_WATCHDOG_TIMER_INTERVAL_STATUS_OK ?
			decltype(ls::recording_channel_configuration::watchdog_timer_period_us)(
				period) :
			nonstd::nullopt;
	}();

	auto blocking_policy = [&channel_attr]() {
		std::int64_t timeout_us;
		const int ret = lttng_channel_get_blocking_timeout(&channel_attr, &timeout_us);

		if (ret) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to retrieve blocking timeout from channel: channel_name=`{}`",
				channel_attr.name));
		}

		switch (timeout_us) {
		case 0:
			return ls::recording_channel_configuration::consumption_blocking_policy(
				ls::recording_channel_configuration::consumption_blocking_policy::
					mode::NONE);
		case -1:
			return ls::recording_channel_configuration::consumption_blocking_policy(
				ls::recording_channel_configuration::consumption_blocking_policy::
					mode::UNBOUNDED);
		default:
			if (timeout_us < 0) {
				/* Negative, but not -1. */
				LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
					"Invalid buffer consumption blocking policy timeout value: timeout_us={}",
					timeout_us));
			}

			return ls::recording_channel_configuration::consumption_blocking_policy(
				ls::recording_channel_configuration::consumption_blocking_policy::
					mode::TIMED,
				timeout_us);
		}
	}();

	auto allocation_policy = [&channel_attr, &domain]() {
		switch (get_domain_class_from_ctl_domain_type(domain->type)) {
		case lttng::sessiond::domain_class::KERNEL_SPACE:
			return ls::recording_channel_configuration::buffer_allocation_policy_t::
				PER_CPU;
		default:
		{
			lttng_channel_allocation_policy policy;
			const auto get_allocation_policy_ret =
				lttng_channel_get_allocation_policy(&channel_attr, &policy);
			LTTNG_ASSERT(get_allocation_policy_ret == LTTNG_OK);

			switch (policy) {
			case LTTNG_CHANNEL_ALLOCATION_POLICY_PER_CPU:
				return ls::recording_channel_configuration::
					buffer_allocation_policy_t::PER_CPU;
			case LTTNG_CHANNEL_ALLOCATION_POLICY_PER_CHANNEL:
				return ls::recording_channel_configuration::
					buffer_allocation_policy_t::PER_CHANNEL;
			default:
				LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
					"Invalid channel allocation policy value received: value={}",
					static_cast<std::underlying_type<
						lttng_channel_allocation_policy>::type>(policy)));
			}
		}
		}
	}();

	ls::domain& target_domain =
		session->get_domain(get_domain_class_from_ctl_domain_type(domain->type));

	/* Extract all channel properties needed to initialize the channel. */
	auto name = std::string(channel_attr.name[0] ? channel_attr.name : DEFAULT_CHANNEL_NAME);
	const auto is_enabled = !!channel_attr.enabled;
	const auto buffer_full_policy = [&session](int overwrite_value) {
		switch (overwrite_value) {
		case -1:
			/* Session default. */
			return session->snapshot_mode ?
				ls::recording_channel_configuration::buffer_full_policy_t::
					OVERWRITE_OLDEST_PACKET :
				ls::recording_channel_configuration::buffer_full_policy_t::
					DISCARD_EVENT;
		case 0:
			return ls::recording_channel_configuration::buffer_full_policy_t::
				DISCARD_EVENT;
		case 1:
			return ls::recording_channel_configuration::buffer_full_policy_t::
				OVERWRITE_OLDEST_PACKET;
		default:
			LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
				"Invalid channel overwrite property value received: value={}",
				overwrite_value));
		}
	}(channel_attr.attr.overwrite);
	const auto trace_file_size_limit_bytes = channel_attr.attr.tracefile_size ?
		decltype(ls::recording_channel_configuration::trace_file_size_limit_bytes)(
			channel_attr.attr.tracefile_size) :
		nonstd::nullopt;

	const auto trace_file_count_limit = channel_attr.attr.tracefile_count ?
		decltype(ls::recording_channel_configuration::trace_file_count_limit)(
			channel_attr.attr.tracefile_count) :
		nonstd::nullopt;

	/* Validate consumption backend (mmap or splice). */
	if (target_domain.domain_class_ != ls::domain_class::KERNEL_SPACE &&
	    channel_attr.attr.output != LTTNG_EVENT_MMAP) {
		LTTNG_THROW_UNSUPPORTED_ERROR(fmt::format(
			"Buffer consumption back-end is unsupported by this domain: domain={}, backend=SPLICE",
			target_domain.domain_class_));
	}

	const auto buffer_consumption_backend = channel_attr.attr.output == LTTNG_EVENT_MMAP ?
		ls::recording_channel_configuration::buffer_consumption_backend_t::MMAP :
		ls::recording_channel_configuration::buffer_consumption_backend_t::SPLICE;

	/* Validate sub-buffer size. */
	if (!lttng::math::is_power_of_two(channel_attr.attr.subbuf_size) ||
	    channel_attr.attr.subbuf_size < the_page_size) {
		LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
			"Invalid subbuffer size (must be a power of two and equal or larger than the page size): subbuffer_size={}, page_size={}",
			channel_attr.attr.subbuf_size,
			the_page_size));
	}

	const auto subbuffer_size_bytes = channel_attr.attr.subbuf_size;

	/* Validate sub-buffer count. */
	if (!lttng::math::is_power_of_two(channel_attr.attr.num_subbuf)) {
		LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
			"Invalid subbuffer count (must be a power of two): subbuffer_count={}",
			channel_attr.attr.num_subbuf));
	}

	try {
		target_domain.get_channel(name).enable();
	} catch (const lttng::sessiond::exceptions::channel_not_found_error& ex) {
		/* Channel doesn't exist, create it. */
		target_domain.add_channel(is_enabled,
					  std::move(name),
					  buffer_full_policy,
					  buffer_consumption_backend,
					  allocation_policy,
					  subbuffer_size_bytes,
					  subbuffer_count,
					  switch_timer_period_us,
					  read_timer_period_us,
					  live_timer_period_us,
					  monitor_timer_period_us,
					  watchdog_timer_period_us,
					  std::move(blocking_policy),
					  trace_file_size_limit_bytes,
					  trace_file_count_limit);
	}

	return ret_code;
}

enum lttng_error_code
cmd_process_attr_tracker_get_tracking_policy(const ltt_session::locked_ref& session,
					     enum lttng_domain_type domain,
					     enum lttng_process_attr process_attr,
					     enum lttng_tracking_policy *policy)
{
	enum lttng_error_code ret_code = LTTNG_OK;
	const struct process_attr_tracker *tracker;

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		if (!session->kernel_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		tracker = kernel_get_process_attr_tracker(session->kernel_session, process_attr);
		break;
	case LTTNG_DOMAIN_UST:
		if (!session->ust_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		tracker = trace_ust_get_process_attr_tracker(session->ust_session, process_attr);
		break;
	default:
		ret_code = LTTNG_ERR_UNSUPPORTED_DOMAIN;
		goto end;
	}
	if (tracker) {
		*policy = process_attr_tracker_get_tracking_policy(tracker);
	} else {
		ret_code = LTTNG_ERR_INVALID;
	}
end:
	return ret_code;
}

enum lttng_error_code
cmd_process_attr_tracker_set_tracking_policy(const ltt_session::locked_ref& session,
					     enum lttng_domain_type domain,
					     enum lttng_process_attr process_attr,
					     enum lttng_tracking_policy policy)
{
	enum lttng_error_code ret_code = LTTNG_OK;

	switch (policy) {
	case LTTNG_TRACKING_POLICY_INCLUDE_SET:
	case LTTNG_TRACKING_POLICY_EXCLUDE_ALL:
	case LTTNG_TRACKING_POLICY_INCLUDE_ALL:
		break;
	default:
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		if (!session->kernel_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		ret_code = kernel_process_attr_tracker_set_tracking_policy(
			session->kernel_session, process_attr, policy);
		break;
	case LTTNG_DOMAIN_UST:
		if (!session->ust_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		ret_code = trace_ust_process_attr_tracker_set_tracking_policy(
			session->ust_session, process_attr, policy);
		break;
	default:
		ret_code = LTTNG_ERR_UNSUPPORTED_DOMAIN;
		break;
	}
end:
	return ret_code;
}

enum lttng_error_code
cmd_process_attr_tracker_inclusion_set_add_value(const ltt_session::locked_ref& session,
						 enum lttng_domain_type domain,
						 enum lttng_process_attr process_attr,
						 const struct process_attr_value *value)
{
	enum lttng_error_code ret_code = LTTNG_OK;

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		if (!session->kernel_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		ret_code = kernel_process_attr_tracker_inclusion_set_add_value(
			session->kernel_session, process_attr, value);
		break;
	case LTTNG_DOMAIN_UST:
		if (!session->ust_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		ret_code = trace_ust_process_attr_tracker_inclusion_set_add_value(
			session->ust_session, process_attr, value);
		break;
	default:
		ret_code = LTTNG_ERR_UNSUPPORTED_DOMAIN;
		break;
	}
end:
	return ret_code;
}

enum lttng_error_code
cmd_process_attr_tracker_inclusion_set_remove_value(const ltt_session::locked_ref& session,
						    enum lttng_domain_type domain,
						    enum lttng_process_attr process_attr,
						    const struct process_attr_value *value)
{
	enum lttng_error_code ret_code = LTTNG_OK;

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		if (!session->kernel_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		ret_code = kernel_process_attr_tracker_inclusion_set_remove_value(
			session->kernel_session, process_attr, value);
		break;
	case LTTNG_DOMAIN_UST:
		if (!session->ust_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		ret_code = trace_ust_process_attr_tracker_inclusion_set_remove_value(
			session->ust_session, process_attr, value);
		break;
	default:
		ret_code = LTTNG_ERR_UNSUPPORTED_DOMAIN;
		break;
	}
end:
	return ret_code;
}

enum lttng_error_code
cmd_process_attr_tracker_get_inclusion_set(const ltt_session::locked_ref& session,
					   enum lttng_domain_type domain,
					   enum lttng_process_attr process_attr,
					   struct lttng_process_attr_values **values)
{
	enum lttng_error_code ret_code = LTTNG_OK;
	const struct process_attr_tracker *tracker;
	enum process_attr_tracker_status status;

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		if (!session->kernel_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		tracker = kernel_get_process_attr_tracker(session->kernel_session, process_attr);
		break;
	case LTTNG_DOMAIN_UST:
		if (!session->ust_session) {
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
		tracker = trace_ust_get_process_attr_tracker(session->ust_session, process_attr);
		break;
	default:
		ret_code = LTTNG_ERR_UNSUPPORTED_DOMAIN;
		goto end;
	}

	if (!tracker) {
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}

	status = process_attr_tracker_get_inclusion_set(tracker, values);
	switch (status) {
	case PROCESS_ATTR_TRACKER_STATUS_OK:
		ret_code = LTTNG_OK;
		break;
	case PROCESS_ATTR_TRACKER_STATUS_INVALID_TRACKING_POLICY:
		ret_code = LTTNG_ERR_PROCESS_ATTR_TRACKER_INVALID_TRACKING_POLICY;
		break;
	case PROCESS_ATTR_TRACKER_STATUS_ERROR:
		ret_code = LTTNG_ERR_NOMEM;
		break;
	default:
		ret_code = LTTNG_ERR_UNK;
		break;
	}

end:
	return ret_code;
}

namespace {
/* An unset value means 'match all'. */
nonstd::optional<lttng::c_string_view>
get_event_rule_pattern_or_name(const lttng_event_rule& event_rule)
{
	lttng_event_rule_status status;
	const char *pattern_or_name;

	switch (lttng_event_rule_get_type(&event_rule)) {
	case LTTNG_EVENT_RULE_TYPE_KERNEL_SYSCALL:
		status = lttng_event_rule_kernel_syscall_get_name_pattern(&event_rule,
									  &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_KERNEL_KPROBE:
		status = lttng_event_rule_kernel_kprobe_get_event_name(&event_rule,
								       &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_KERNEL_TRACEPOINT:
		status = lttng_event_rule_kernel_tracepoint_get_name_pattern(&event_rule,
									     &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_KERNEL_UPROBE:
		status = lttng_event_rule_kernel_uprobe_get_event_name(&event_rule,
								       &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_USER_TRACEPOINT:
		status = lttng_event_rule_user_tracepoint_get_name_pattern(&event_rule,
									   &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_JUL_LOGGING:
		status = lttng_event_rule_jul_logging_get_name_pattern(&event_rule,
								       &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_LOG4J_LOGGING:
		status = lttng_event_rule_log4j_logging_get_name_pattern(&event_rule,
									 &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_PYTHON_LOGGING:
		status = lttng_event_rule_python_logging_get_name_pattern(&event_rule,
									  &pattern_or_name);
		break;
	case LTTNG_EVENT_RULE_TYPE_LOG4J2_LOGGING:
		status = lttng_event_rule_log4j2_logging_get_name_pattern(&event_rule,
									  &pattern_or_name);
		break;
	default:
		std::abort();
	}

	if (status == LTTNG_EVENT_RULE_STATUS_UNSET) {
		return nonstd::nullopt;
	}

	LTTNG_ASSERT(status == LTTNG_EVENT_RULE_STATUS_OK);
	LTTNG_ASSERT(pattern_or_name);

	return pattern_or_name;
}
} /* namespace */

/*
 * Command LTTNG_DISABLE_EVENT processed by the client thread.
 *
 * Filter and exclusions are simply not handled by the disable event command
 * at this time.
 */
lttng_error_code cmd_disable_event(struct command_ctx *cmd_ctx,
				   ltt_session::locked_ref& locked_session,
				   struct lttng_event *event,
				   char *raw_filter_expression,
				   struct lttng_bytecode *raw_bytecode,
				   struct lttng_event_exclusion *raw_exclusion,
				   lttng::event_rule_uptr event_rule)
{
	int ret;
	const ltt_session& session = *locked_session;
	const char *event_name = event->name;
	const char *channel_name = cmd_ctx->lsm.u.disable.channel_name;
	const enum lttng_domain_type domain = cmd_ctx->lsm.domain.type;

	DBG("Disable event command for event \'%s\'", event->name);

	const auto filter_expression =
		lttng::make_unique_wrapper<char, lttng::memory::free>(raw_filter_expression);
	const auto bytecode =
		lttng::make_unique_wrapper<lttng_bytecode, lttng::memory::free>(raw_bytecode);
	const auto exclusion =
		lttng::make_unique_wrapper<lttng_event_exclusion, lttng::memory::free>(
			raw_exclusion);

	if (!event_rule && event->type != LTTNG_EVENT_ALL) {
		/* LTTNG_EVENT_ALL is the only case where we don't expect an event rule. */
		return LTTNG_ERR_INVALID_PROTOCOL;
	}

	/* Ignore the presence of filter or exclusion for the event */
	event->filter = 0;
	event->exclusion = 0;

	if (channel_name[0] == '\0') {
		switch (cmd_ctx->lsm.domain.type) {
		case LTTNG_DOMAIN_LOG4J:
			channel_name = DEFAULT_LOG4J_CHANNEL_NAME;
			break;
		case LTTNG_DOMAIN_LOG4J2:
			channel_name = DEFAULT_LOG4J2_CHANNEL_NAME;
			break;
		case LTTNG_DOMAIN_JUL:
			channel_name = DEFAULT_JUL_CHANNEL_NAME;
			break;
		case LTTNG_DOMAIN_PYTHON:
			channel_name = DEFAULT_PYTHON_CHANNEL_NAME;
			break;
		default:
			channel_name = DEFAULT_CHANNEL_NAME;
			break;
		}
	}

	const lttng::urcu::read_lock_guard read_lock;

	/* Error out on unhandled search criteria */
	if (event->loglevel_type || event->loglevel != -1 || event->enabled || event->pid ||
	    event->filter || event->exclusion) {
		return LTTNG_ERR_UNK;
	}

	const bool pattern_disables_all = lttng::c_string_view(event_name) == "";

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
	{
		struct ltt_kernel_channel *kchan;
		struct ltt_kernel_session *ksess;

		ksess = session.kernel_session;

		/*
		 * If a non-default channel has been created in the
		 * session, explicitely require that -c chan_name needs
		 * to be provided.
		 */
		if (ksess->has_non_default_channel && channel_name[0] == '\0') {
			return LTTNG_ERR_NEED_CHANNEL_NAME;
		}

		kchan = trace_kernel_get_channel_by_name(channel_name, ksess);
		if (kchan == nullptr) {
			return LTTNG_ERR_KERN_CHAN_NOT_FOUND;
		}

		switch (event->type) {
		case LTTNG_EVENT_ALL:
		case LTTNG_EVENT_TRACEPOINT:
		case LTTNG_EVENT_SYSCALL:
		case LTTNG_EVENT_PROBE:
		case LTTNG_EVENT_FUNCTION:
		case LTTNG_EVENT_FUNCTION_ENTRY: /* fall-through */
			if (pattern_disables_all) {
				ret = event_kernel_disable_event(kchan, nullptr, event->type);
			} else {
				ret = event_kernel_disable_event(kchan, event_name, event->type);
			}
			if (ret != LTTNG_OK) {
				return static_cast<lttng_error_code>(ret);
			}
			break;
		default:
			return LTTNG_ERR_UNK;
		}

		kernel_wait_quiescent();
		break;
	}
	case LTTNG_DOMAIN_UST:
	{
		struct ltt_ust_channel *uchan;
		struct ltt_ust_session *usess;

		usess = session.ust_session;

		if (validate_ust_event_name(event_name)) {
			return LTTNG_ERR_INVALID_EVENT_NAME;
		}

		/*
		 * If a non-default channel has been created in the
		 * session, explicitly require that -c chan_name needs
		 * to be provided.
		 */
		if (usess->has_non_default_channel && channel_name[0] == '\0') {
			return LTTNG_ERR_NEED_CHANNEL_NAME;
		}

		uchan = trace_ust_find_channel_by_name(usess->domain_global.channels, channel_name);
		if (uchan == nullptr) {
			return LTTNG_ERR_UST_CHAN_NOT_FOUND;
		}

		switch (event->type) {
		case LTTNG_EVENT_ALL:
			if (pattern_disables_all) {
				ret = event_ust_disable_all_tracepoints(usess, uchan);
			} else {
				ret = event_ust_disable_tracepoint(usess, uchan, event_name);
			}

			if (ret != LTTNG_OK) {
				return static_cast<lttng_error_code>(ret);
			}
			break;
		default:
			return LTTNG_ERR_UNK;
		}

		DBG3("Disable UST event %s in channel %s completed", event_name, channel_name);
		break;
	}
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_PYTHON:
	{
		struct agent *agt;
		struct ltt_ust_session *usess = session.ust_session;

		LTTNG_ASSERT(usess);

		switch (event->type) {
		case LTTNG_EVENT_ALL:
			break;
		default:
			return LTTNG_ERR_UNK;
		}

		agt = trace_ust_find_agent(usess, domain);
		if (!agt) {
			return LTTNG_ERR_UST_EVENT_NOT_FOUND;
		}

		if (pattern_disables_all) {
			ret = event_agent_disable_all(usess, agt);
		} else {
			ret = event_agent_disable(usess, agt, event_name);
		}

		if (ret != LTTNG_OK) {
			return static_cast<lttng_error_code>(ret);
		}

		break;
	}
	default:
		return LTTNG_ERR_UND;
	}

	for (const auto& pair : session.get_domain(get_domain_class_from_ctl_domain_type(domain))
					.get_channel(channel_name)
					.event_rules) {
		auto& event_rule_cfg = *pair.second;

		const auto this_pattern_or_name =
			get_event_rule_pattern_or_name(*event_rule_cfg.event_rule);
		if (pattern_disables_all || (*this_pattern_or_name == event_name)) {
			event_rule_cfg.disable();
		}
	}

	return LTTNG_OK;
}

/*
 * Command LTTNG_ADD_CONTEXT processed by the client thread.
 */
int cmd_add_context(struct command_ctx *cmd_ctx,
		    ltt_session::locked_ref& locked_session,
		    const struct lttng_event_context *event_context,
		    int kwpipe)
{
	int ret, chan_kern_created = 0, chan_ust_created = 0;
	const enum lttng_domain_type domain = cmd_ctx->lsm.domain.type;
	const struct ltt_session& session = *locked_session;
	const char *channel_name = cmd_ctx->lsm.u.context.channel_name;

	/*
	 * Don't try to add a context if the session has been started at
	 * some point in time before. The tracer does not allow it and would
	 * result in a corrupted trace.
	 */
	if (session.has_been_started) {
		ret = LTTNG_ERR_TRACE_ALREADY_STARTED;
		goto end;
	}

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		LTTNG_ASSERT(session.kernel_session);

		if (session.kernel_session->channel_count == 0) {
			/* Create default channel */
			ret = channel_kernel_create(session.kernel_session, nullptr, kwpipe);
			if (ret != LTTNG_OK) {
				goto error;
			}
			chan_kern_created = 1;
		}
		/* Add kernel context to kernel tracer */
		ret = context_kernel_add(session.kernel_session, event_context, channel_name);
		if (ret != LTTNG_OK) {
			goto error;
		}
		break;
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	{
		/*
		 * Validate channel name.
		 * If no channel name is given and the domain is JUL or LOG4J,
		 * set it to the appropriate domain-specific channel name. If
		 * a name is provided but does not match the expexted channel
		 * name, return an error.
		 */
		if (domain == LTTNG_DOMAIN_JUL && *channel_name &&
		    strcmp(channel_name, DEFAULT_JUL_CHANNEL_NAME) != 0) {
			ret = LTTNG_ERR_UST_CHAN_NOT_FOUND;
			goto error;
		} else if (domain == LTTNG_DOMAIN_LOG4J && *channel_name &&
			   strcmp(channel_name, DEFAULT_LOG4J_CHANNEL_NAME) != 0) {
			ret = LTTNG_ERR_UST_CHAN_NOT_FOUND;
			goto error;
		} else if (domain == LTTNG_DOMAIN_LOG4J2 && *channel_name &&
			   strcmp(channel_name, DEFAULT_LOG4J2_CHANNEL_NAME) != 0) {
			ret = LTTNG_ERR_UST_CHAN_NOT_FOUND;
			goto error;
		}
	}
	/* fall through */
	case LTTNG_DOMAIN_UST:
	{
		struct ltt_ust_session *usess = session.ust_session;
		unsigned int chan_count;

		LTTNG_ASSERT(usess);

		chan_count = lttng_ht_get_count(usess->domain_global.channels);
		if (chan_count == 0) {
			/* Create default channel */
			auto attr = lttng::make_unique_wrapper<lttng_channel, channel_attr_destroy>(
				channel_new_default_attr(domain, usess->buffer_type));

			if (!attr) {
				ret = LTTNG_ERR_FATAL;
				goto error;
			}

			ret = channel_ust_create(usess, attr.get(), usess->buffer_type);
			if (ret != LTTNG_OK) {
				goto error;
			}

			chan_ust_created = 1;
		}

		ret = context_ust_add(usess, domain, event_context, channel_name);
		if (ret != LTTNG_OK) {
			goto error;
		}
		break;
	}
	default:
		ret = LTTNG_ERR_UND;
		goto error;
	}

	ret = LTTNG_OK;
	goto end;

error:
	if (chan_kern_created) {
		struct ltt_kernel_channel *kchan = trace_kernel_get_channel_by_name(
			DEFAULT_CHANNEL_NAME, session.kernel_session);
		/* Created previously, this should NOT fail. */
		LTTNG_ASSERT(kchan);
		kernel_destroy_channel(kchan);
	}

	if (chan_ust_created) {
		struct ltt_ust_channel *uchan = trace_ust_find_channel_by_name(
			session.ust_session->domain_global.channels, DEFAULT_CHANNEL_NAME);
		/* Created previously, this should NOT fail. */
		LTTNG_ASSERT(uchan);
		/* Remove from the channel list of the session. */
		trace_ust_delete_channel(session.ust_session->domain_global.channels, uchan);
		trace_ust_destroy_channel(uchan);
	}
end:
	return ret;
}

static inline bool name_starts_with(const char *name, const char *prefix)
{
	const size_t max_cmp_len = std::min(strlen(prefix), (size_t) LTTNG_SYMBOL_NAME_LEN);

	return !strncmp(name, prefix, max_cmp_len);
}

/* Perform userspace-specific event name validation */
static int validate_ust_event_name(const char *name)
{
	int ret = 0;

	if (!name) {
		ret = -1;
		goto end;
	}

	/*
	 * Check name against all internal UST event component namespaces used
	 * by the agents.
	 */
	if (name_starts_with(name, DEFAULT_JUL_EVENT_COMPONENT) ||
	    name_starts_with(name, DEFAULT_LOG4J_EVENT_COMPONENT) ||
	    name_starts_with(name, DEFAULT_LOG4J2_EVENT_COMPONENT) ||
	    name_starts_with(name, DEFAULT_PYTHON_EVENT_COMPONENT)) {
		ret = -1;
	}

end:
	return ret;
}

namespace {
lttng::event_rule_uptr create_agent_internal_event_rule(lttng_domain_type agent_domain,
							lttng::c_string_view filter_expression)
{
	lttng::event_rule_uptr event_rule(lttng_event_rule_user_tracepoint_create());

	if (!event_rule) {
		LTTNG_THROW_POSIX("Failed to allocate agent internal user space tracer event rule",
				  ENOMEM);
	}

	const auto set_pattern_ret = lttng_event_rule_user_tracepoint_set_name_pattern(
		event_rule.get(), event_get_default_agent_ust_name(agent_domain));
	if (set_pattern_ret != LTTNG_EVENT_RULE_STATUS_OK) {
		LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
			"Failed to set agent LTTng-UST event name as event-rule name pattern: domain={}",
			agent_domain));
	}

	if (filter_expression) {
		const auto set_filter_ret = lttng_event_rule_user_tracepoint_set_filter(
			event_rule.get(), filter_expression.data());
		if (set_filter_ret != LTTNG_EVENT_RULE_STATUS_OK) {
			LTTNG_THROW_INVALID_ARGUMENT_ERROR(fmt::format(
				"Failed to set agent LTTng-UST event filter expression as event-rule filter: domain={}",
				filter_expression));
		}
	}

	return event_rule;
}
} /* namespace */

/*
 * Internal version of cmd_enable_event() with a supplemental
 * "internal_event" flag which is used to enable internal events which should
 * be hidden from clients. Such events are used in the agent implementation to
 * enable the events through which all "agent" events are funeled.
 */
static lttng_error_code _cmd_enable_event(ltt_session::locked_ref& locked_session,
					  const struct lttng_domain *domain,
					  char *channel_name,
					  struct lttng_event *event,
					  char *raw_filter_expression,
					  struct lttng_bytecode *raw_bytecode,
					  struct lttng_event_exclusion *raw_exclusion,
					  int wpipe,
					  bool internal_event,
					  lttng::event_rule_uptr event_rule)
{
	auto filter_expression =
		lttng::make_unique_wrapper<char, lttng::memory::free>(raw_filter_expression);
	auto bytecode =
		lttng::make_unique_wrapper<lttng_bytecode, lttng::memory::free>(raw_bytecode);
	auto exclusion = lttng::make_unique_wrapper<lttng_event_exclusion, lttng::memory::free>(
		raw_exclusion);

	if (!event_rule) {
		return LTTNG_ERR_INVALID_PROTOCOL;
	}

	int ret = 0, channel_created = 0;
	struct lttng_channel *attr = nullptr;
	ltt_session& session = *locked_session;

	LTTNG_ASSERT(event);
	LTTNG_ASSERT(channel_name);

	/* Normalize event name as a globbing pattern */
	strutils_normalize_star_glob_pattern(event->name);

	/* Normalize exclusion names as globbing patterns */
	if (exclusion) {
		size_t i;

		for (i = 0; i < exclusion->count; i++) {
			char *name = LTTNG_EVENT_EXCLUSION_NAME_AT(exclusion, i);

			strutils_normalize_star_glob_pattern(name);
		}
	}

	const lttng::urcu::read_lock_guard read_lock;
	const auto *user_visible_channel_name = channel_name[0] == '\0' ? DEFAULT_CHANNEL_NAME :
									  channel_name;

	/* If we have a filter, we must have its filter expression. */
	if (!!filter_expression ^ !!bytecode) {
		DBG("Refusing to enable recording event rule as it has an inconsistent filter expression and bytecode specification");
		return LTTNG_ERR_INVALID;
	}

	switch (domain->type) {
	case LTTNG_DOMAIN_KERNEL:
	{
		struct ltt_kernel_channel *kchan;

		/*
		 * If a non-default channel has been created in the
		 * session, explicitely require that -c chan_name needs
		 * to be provided.
		 */
		if (session.kernel_session->has_non_default_channel && channel_name[0] == '\0') {
			return LTTNG_ERR_NEED_CHANNEL_NAME;
		}

		kchan = trace_kernel_get_channel_by_name(channel_name, session.kernel_session);
		if (kchan == nullptr) {
			attr = channel_new_default_attr(LTTNG_DOMAIN_KERNEL, LTTNG_BUFFER_GLOBAL);
			if (attr == nullptr) {
				return LTTNG_ERR_FATAL;
			}

			if (lttng_strncpy(attr->name, channel_name, sizeof(attr->name))) {
				return LTTNG_ERR_INVALID;
			}

			ret = cmd_enable_channel_internal(locked_session, domain, *attr, wpipe);
			if (ret != LTTNG_OK) {
				return static_cast<lttng_error_code>(ret);
			}

			channel_created = 1;
		}

		/* Get the newly created kernel channel pointer */
		kchan = trace_kernel_get_channel_by_name(channel_name, session.kernel_session);
		if (kchan == nullptr) {
			/* This sould not happen... */
			return LTTNG_ERR_FATAL;
		}

		switch (event->type) {
		case LTTNG_EVENT_ALL:
		{
			char *filter_expression_a = nullptr;
			struct lttng_bytecode *filter_a = nullptr;

			/*
			 * We need to duplicate filter_expression and filter,
			 * because ownership is passed to first enable
			 * event.
			 */
			if (filter_expression) {
				filter_expression_a = strdup(filter_expression.get());
				if (!filter_expression_a) {
					return LTTNG_ERR_FATAL;
				}
			}

			if (bytecode) {
				filter_a =
					zmalloc<lttng_bytecode>(sizeof(*filter_a) + bytecode->len);
				if (!filter_a) {
					free(filter_expression_a);
					return LTTNG_ERR_FATAL;
				}

				memcpy(filter_a, bytecode.get(), sizeof(*filter_a) + bytecode->len);
			}

			event->type = LTTNG_EVENT_TRACEPOINT; /* Hack */
			ret = event_kernel_enable_event(
				kchan, event, filter_expression.release(), bytecode.release());
			if (ret != LTTNG_OK) {
				if (channel_created) {
					/* Let's not leak a useless channel. */
					kernel_destroy_channel(kchan);
				}

				free(filter_expression_a);
				free(filter_a);
				return static_cast<lttng_error_code>(ret);
			}

			event->type = LTTNG_EVENT_SYSCALL; /* Hack */
			ret = event_kernel_enable_event(
				kchan, event, filter_expression_a, filter_a);
			/* We have passed ownership */
			filter_expression_a = nullptr;
			filter_a = nullptr;
			if (ret != LTTNG_OK) {
				return static_cast<lttng_error_code>(ret);
			}

			break;
		}
		case LTTNG_EVENT_PROBE:
		case LTTNG_EVENT_USERSPACE_PROBE:
		case LTTNG_EVENT_FUNCTION:
		case LTTNG_EVENT_FUNCTION_ENTRY:
		case LTTNG_EVENT_TRACEPOINT:
			ret = event_kernel_enable_event(
				kchan, event, filter_expression.release(), bytecode.release());
			if (ret != LTTNG_OK) {
				if (channel_created) {
					/* Let's not leak a useless channel. */
					kernel_destroy_channel(kchan);
				}

				return static_cast<lttng_error_code>(ret);
			}

			break;
		case LTTNG_EVENT_SYSCALL:
			ret = event_kernel_enable_event(
				kchan, event, filter_expression.release(), bytecode.release());
			if (ret != LTTNG_OK) {
				return static_cast<lttng_error_code>(ret);
			}

			break;
		default:
			return LTTNG_ERR_UNK;
		}

		kernel_wait_quiescent();
		break;
	}
	case LTTNG_DOMAIN_UST:
	{
		struct ltt_ust_channel *uchan;
		struct ltt_ust_session *usess = session.ust_session;

		LTTNG_ASSERT(usess);

		/*
		 * If a non-default channel has been created in the
		 * session, explicitely require that -c chan_name needs
		 * to be provided.
		 */
		if (usess->has_non_default_channel && channel_name[0] == '\0') {
			return LTTNG_ERR_NEED_CHANNEL_NAME;
		}

		/* Get channel from global UST domain */
		uchan = trace_ust_find_channel_by_name(usess->domain_global.channels, channel_name);
		if (uchan == nullptr) {
			/* Create default channel */
			attr = channel_new_default_attr(LTTNG_DOMAIN_UST, usess->buffer_type);
			if (attr == nullptr) {
				return LTTNG_ERR_FATAL;
			}

			if (lttng_strncpy(attr->name, channel_name, sizeof(attr->name))) {
				return LTTNG_ERR_INVALID;
			}

			ret = cmd_enable_channel_internal(locked_session, domain, *attr, wpipe);
			if (ret != LTTNG_OK) {
				return static_cast<lttng_error_code>(ret);
			}

			/* Get the newly created channel reference back */
			uchan = trace_ust_find_channel_by_name(usess->domain_global.channels,
							       channel_name);
			LTTNG_ASSERT(uchan);
		}

		if (uchan->domain != LTTNG_DOMAIN_UST && !internal_event) {
			/*
			 * Don't allow users to add UST events to channels which
			 * are assigned to a userspace subdomain (JUL, Log4J,
			 * Python, etc.).
			 */
			return LTTNG_ERR_INVALID_CHANNEL_DOMAIN;
		}

		if (!internal_event) {
			/*
			 * Ensure the event name is not reserved for internal
			 * use.
			 */
			ret = validate_ust_event_name(event->name);
			if (ret) {
				WARN("Userspace event name %s failed validation.", event->name);
				return LTTNG_ERR_INVALID_EVENT_NAME;
			}
		}

		/* At this point, the session and channel exist on the tracer */
		ret = event_ust_enable_tracepoint(usess,
						  uchan,
						  event,
						  filter_expression.release(),
						  bytecode.release(),
						  exclusion.release(),
						  internal_event);
		if (ret != LTTNG_OK) {
			return static_cast<lttng_error_code>(ret);
		}

		break;
	}
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_PYTHON:
	{
		const char *default_event_name, *default_chan_name;
		struct agent *agt;
		struct lttng_event uevent;
		struct lttng_domain tmp_dom;
		struct ltt_ust_session *usess = session.ust_session;

		lttng::event_rule_uptr internal_event_rule =
			create_agent_internal_event_rule(domain->type, filter_expression.get());

		LTTNG_ASSERT(usess);

		if (!agent_tracing_is_enabled()) {
			DBG("Attempted to enable an event in an agent domain but the agent thread is not running");
			return LTTNG_ERR_AGENT_TRACING_DISABLED;
		}

		agt = trace_ust_find_agent(usess, domain->type);
		if (!agt) {
			agt = agent_create(domain->type);
			if (!agt) {
				return LTTNG_ERR_NOMEM;
			}

			/* Ownership of agt is transferred. */
			agent_add(agt, usess->agents);
		}

		/* Create the default tracepoint. */
		memset(&uevent, 0, sizeof(uevent));
		uevent.type = LTTNG_EVENT_TRACEPOINT;
		uevent.loglevel_type = LTTNG_EVENT_LOGLEVEL_ALL;
		uevent.loglevel = -1;
		default_event_name = event_get_default_agent_ust_name(domain->type);
		if (!default_event_name) {
			return LTTNG_ERR_FATAL;
		}

		strncpy(uevent.name, default_event_name, sizeof(uevent.name));
		uevent.name[sizeof(uevent.name) - 1] = '\0';

		/*
		 * The domain type is changed because we are about to enable the
		 * default channel and event for the JUL domain that are hardcoded.
		 * This happens in the UST domain.
		 */
		memcpy(&tmp_dom, domain, sizeof(tmp_dom));
		tmp_dom.type = LTTNG_DOMAIN_UST;

		switch (domain->type) {
		case LTTNG_DOMAIN_LOG4J:
			default_chan_name = DEFAULT_LOG4J_CHANNEL_NAME;
			break;
		case LTTNG_DOMAIN_LOG4J2:
			default_chan_name = DEFAULT_LOG4J2_CHANNEL_NAME;
			break;
		case LTTNG_DOMAIN_JUL:
			default_chan_name = DEFAULT_JUL_CHANNEL_NAME;
			break;
		case LTTNG_DOMAIN_PYTHON:
			default_chan_name = DEFAULT_PYTHON_CHANNEL_NAME;
			break;
		default:
			/* The switch/case we are in makes this impossible */
			abort();
		}

		user_visible_channel_name = default_chan_name;

		{
			auto filter_expression_copy =
				lttng::make_unique_wrapper<char, lttng::memory::free>();
			auto bytecode_copy =
				lttng::make_unique_wrapper<lttng_bytecode, lttng::memory::free>();

			if (bytecode) {
				const size_t filter_size =
					sizeof(struct lttng_bytecode) + bytecode->len;

				bytecode_copy.reset(zmalloc<lttng_bytecode>(filter_size));
				if (!bytecode_copy) {
					return LTTNG_ERR_NOMEM;
				}

				memcpy(bytecode_copy.get(), bytecode.get(), filter_size);

				filter_expression_copy.reset(strdup(filter_expression.get()));
				if (!filter_expression_copy) {
					return LTTNG_ERR_NOMEM;
				}
			}

			ret = cmd_enable_event_internal(locked_session,
							&tmp_dom,
							(char *) default_chan_name,
							&uevent,
							filter_expression_copy.release(),
							bytecode_copy.release(),
							nullptr,
							wpipe,
							std::move(internal_event_rule));
		}

		if (ret != LTTNG_OK) {
			return static_cast<lttng_error_code>(ret);
		}

		/* The wild card * means that everything should be enabled. */
		if (strncmp(event->name, "*", 1) == 0 && strlen(event->name) == 1) {
			ret = event_agent_enable_all(
				usess, agt, event, bytecode.release(), filter_expression.release());
		} else {
			ret = event_agent_enable(
				usess, agt, event, bytecode.release(), filter_expression.release());
		}

		if (ret != LTTNG_OK) {
			return static_cast<lttng_error_code>(ret);
		}

		break;
	}
	default:
		return LTTNG_ERR_UND;
	}

	auto& channel_cfg = session.get_domain(get_domain_class_from_ctl_domain_type(domain->type))
				    .get_channel(user_visible_channel_name);
	try {
		channel_cfg.get_event_rule_configuration(*event_rule).enable();
	} catch (const lttng::sessiond::exceptions::event_rule_configuration_not_found_error& ex) {
		DBG("%s", ex.what());
		channel_cfg.add_event_rule_configuration(true, std::move(event_rule));
	}

	return LTTNG_OK;
}

/*
 * Command LTTNG_ENABLE_EVENT processed by the client thread.
 * We own filter, exclusion, and filter_expression.
 */
int cmd_enable_event(struct command_ctx *cmd_ctx,
		     ltt_session::locked_ref& locked_session,
		     struct lttng_event *event,
		     char *filter_expression,
		     struct lttng_event_exclusion *exclusion,
		     struct lttng_bytecode *bytecode,
		     int wpipe,
		     lttng::event_rule_uptr event_rule)
{
	int ret;
	/*
	 * Copied to ensure proper alignment since 'lsm' is a packed structure.
	 */
	const lttng_domain command_domain = cmd_ctx->lsm.domain;

	/*
	 * The ownership of the following parameters is transferred to
	 * _cmd_enable_event:
	 *
	 *  - filter_expression,
	 *  - bytecode,
	 *  - exclusion
	 */
	ret = _cmd_enable_event(locked_session,
				&command_domain,
				cmd_ctx->lsm.u.enable.channel_name,
				event,
				filter_expression,
				bytecode,
				exclusion,
				wpipe,
				false,
				std::move(event_rule));
	filter_expression = nullptr;
	bytecode = nullptr;
	exclusion = nullptr;
	return ret;
}

/*
 * Enable an event which is internal to LTTng. An internal should
 * never be made visible to clients and are immune to checks such as
 * reserved names.
 */
static int cmd_enable_event_internal(ltt_session::locked_ref& locked_session,
				     const struct lttng_domain *domain,
				     char *channel_name,
				     struct lttng_event *event,
				     char *filter_expression,
				     struct lttng_bytecode *filter,
				     struct lttng_event_exclusion *exclusion,
				     int wpipe,
				     lttng::event_rule_uptr event_rule)
{
	return _cmd_enable_event(locked_session,
				 domain,
				 channel_name,
				 event,
				 filter_expression,
				 filter,
				 exclusion,
				 wpipe,
				 true,
				 std::move(event_rule));
}

/*
 * Command LTTNG_LIST_TRACEPOINTS processed by the client thread.
 */
enum lttng_error_code cmd_list_tracepoints(enum lttng_domain_type domain,
					   struct lttng_payload *reply_payload)
{
	enum lttng_error_code ret_code;
	int ret;
	ssize_t i, nb_events = 0;
	struct lttng_event *events = nullptr;
	struct lttcomm_list_command_header reply_command_header = {};
	size_t reply_command_header_offset;

	assert(reply_payload);

	/* Reserve space for command reply header. */
	reply_command_header_offset = reply_payload->buffer.size;
	ret = lttng_dynamic_buffer_set_size(&reply_payload->buffer,
					    reply_command_header_offset +
						    sizeof(struct lttcomm_list_command_header));
	if (ret) {
		ret_code = LTTNG_ERR_NOMEM;
		goto error;
	}

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		nb_events = kernel_list_events(&events);
		if (nb_events < 0) {
			ret_code = LTTNG_ERR_KERN_LIST_FAIL;
			goto error;
		}
		break;
	case LTTNG_DOMAIN_UST:
		nb_events = ust_app_list_events(&events);
		if (nb_events < 0) {
			ret_code = LTTNG_ERR_UST_LIST_FAIL;
			goto error;
		}
		break;
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_PYTHON:
		nb_events = agent_list_events(&events, domain);
		if (nb_events < 0) {
			ret_code = LTTNG_ERR_UST_LIST_FAIL;
			goto error;
		}
		break;
	default:
		ret_code = LTTNG_ERR_UND;
		goto error;
	}

	for (i = 0; i < nb_events; i++) {
		ret = lttng_event_serialize(
			&events[i], 0, nullptr, nullptr, 0, nullptr, reply_payload);
		if (ret) {
			ret_code = LTTNG_ERR_NOMEM;
			goto error;
		}
	}

	if (nb_events > UINT32_MAX) {
		ERR("Tracepoint count would overflow the tracepoint listing command's reply");
		ret_code = LTTNG_ERR_OVERFLOW;
		goto error;
	}

	/* Update command reply header. */
	reply_command_header.count = (uint32_t) nb_events;
	memcpy(reply_payload->buffer.data + reply_command_header_offset,
	       &reply_command_header,
	       sizeof(reply_command_header));

	ret_code = LTTNG_OK;
error:
	free(events);
	return ret_code;
}

/*
 * Command LTTNG_LIST_TRACEPOINT_FIELDS processed by the client thread.
 */
enum lttng_error_code cmd_list_tracepoint_fields(enum lttng_domain_type domain,
						 struct lttng_payload *reply)
{
	enum lttng_error_code ret_code;
	int ret;
	unsigned int i, nb_fields;
	struct lttng_event_field *fields = nullptr;
	struct lttcomm_list_command_header reply_command_header = {};
	size_t reply_command_header_offset;

	assert(reply);

	/* Reserve space for command reply header. */
	reply_command_header_offset = reply->buffer.size;
	ret = lttng_dynamic_buffer_set_size(&reply->buffer,
					    reply_command_header_offset +
						    sizeof(struct lttcomm_list_command_header));
	if (ret) {
		ret_code = LTTNG_ERR_NOMEM;
		goto error;
	}

	switch (domain) {
	case LTTNG_DOMAIN_UST:
		ret = ust_app_list_event_fields(&fields);
		if (ret < 0) {
			ret_code = LTTNG_ERR_UST_LIST_FAIL;
			goto error;
		}

		break;
	case LTTNG_DOMAIN_KERNEL:
	default: /* fall-through */
		ret_code = LTTNG_ERR_UND;
		goto error;
	}

	nb_fields = ret;

	for (i = 0; i < nb_fields; i++) {
		ret = lttng_event_field_serialize(&fields[i], reply);
		if (ret) {
			ret_code = LTTNG_ERR_NOMEM;
			goto error;
		}
	}

	if (nb_fields > UINT32_MAX) {
		ERR("Tracepoint field count would overflow the tracepoint field listing command's reply");
		ret_code = LTTNG_ERR_OVERFLOW;
		goto error;
	}

	/* Update command reply header. */
	reply_command_header.count = (uint32_t) nb_fields;

	memcpy(reply->buffer.data + reply_command_header_offset,
	       &reply_command_header,
	       sizeof(reply_command_header));

	ret_code = LTTNG_OK;

error:
	free(fields);
	return ret_code;
}

enum lttng_error_code cmd_list_syscalls(struct lttng_payload *reply_payload)
{
	enum lttng_error_code ret_code;
	ssize_t nb_events, i;
	int ret;
	struct lttng_event *events = nullptr;
	struct lttcomm_list_command_header reply_command_header = {};
	size_t reply_command_header_offset;

	assert(reply_payload);

	/* Reserve space for command reply header. */
	reply_command_header_offset = reply_payload->buffer.size;
	ret = lttng_dynamic_buffer_set_size(&reply_payload->buffer,
					    reply_command_header_offset +
						    sizeof(struct lttcomm_list_command_header));
	if (ret) {
		ret_code = LTTNG_ERR_NOMEM;
		goto end;
	}

	nb_events = syscall_table_list(&events);
	if (nb_events < 0) {
		ret_code = (enum lttng_error_code) - nb_events;
		goto end;
	}

	for (i = 0; i < nb_events; i++) {
		ret = lttng_event_serialize(
			&events[i], 0, nullptr, nullptr, 0, nullptr, reply_payload);
		if (ret) {
			ret_code = LTTNG_ERR_NOMEM;
			goto end;
		}
	}

	if (nb_events > UINT32_MAX) {
		ERR("Syscall count would overflow the syscall listing command's reply");
		ret_code = LTTNG_ERR_OVERFLOW;
		goto end;
	}

	/* Update command reply header. */
	reply_command_header.count = (uint32_t) nb_events;
	memcpy(reply_payload->buffer.data + reply_command_header_offset,
	       &reply_command_header,
	       sizeof(reply_command_header));

	ret_code = LTTNG_OK;
end:
	free(events);
	return ret_code;
}

/*
 * Command LTTNG_START_TRACE processed by the client thread.
 */
int cmd_start_trace(const ltt_session::locked_ref& session)
{
	enum lttng_error_code ret;
	unsigned long nb_chan = 0;
	struct ltt_kernel_session *ksession;
	struct ltt_ust_session *usess;
	const bool session_rotated_after_last_stop = session->rotated_after_last_stop;
	const bool session_cleared_after_last_stop = session->cleared_after_last_stop;

	/* Ease our life a bit ;) */
	ksession = session->kernel_session;
	usess = session->ust_session;

	/* Is the session already started? */
	if (session->active) {
		ret = LTTNG_ERR_TRACE_ALREADY_STARTED;
		/* Perform nothing */
		goto end;
	}

	if (session->rotation_state == LTTNG_ROTATION_STATE_ONGOING &&
	    !session->current_trace_chunk) {
		/*
		 * A rotation was launched while the session was stopped and
		 * it has not been completed yet. It is not possible to start
		 * the session since starting the session here would require a
		 * rotation from "NULL" to a new trace chunk. That rotation
		 * would overlap with the ongoing rotation, which is not
		 * supported.
		 */
		WARN("Refusing to start session \"%s\" as a rotation launched after the last \"stop\" is still ongoing",
		     session->name);
		ret = LTTNG_ERR_ROTATION_PENDING;
		goto error;
	}

	/*
	 * Starting a session without channel is useless since after that it's not
	 * possible to enable channel thus inform the client.
	 */
	if (usess && usess->domain_global.channels) {
		nb_chan += lttng_ht_get_count(usess->domain_global.channels);
	}
	if (ksession) {
		nb_chan += ksession->channel_count;
	}
	if (!nb_chan) {
		ret = LTTNG_ERR_NO_CHANNEL;
		goto error;
	}

	session->active = true;
	session->rotated_after_last_stop = false;
	session->cleared_after_last_stop = false;
	if (session->output_traces && !session->current_trace_chunk) {
		if (!session->has_been_started) {
			struct lttng_trace_chunk *trace_chunk;

			DBG("Creating initial trace chunk of session \"%s\"", session->name);
			trace_chunk =
				session_create_new_trace_chunk(session, nullptr, nullptr, nullptr);
			if (!trace_chunk) {
				ret = LTTNG_ERR_CREATE_DIR_FAIL;
				goto error;
			}
			LTTNG_ASSERT(!session->current_trace_chunk);
			ret = (lttng_error_code) session_set_trace_chunk(
				session, trace_chunk, nullptr);
			lttng_trace_chunk_put(trace_chunk);
			if (ret) {
				ret = LTTNG_ERR_CREATE_TRACE_CHUNK_FAIL_CONSUMER;
				goto error;
			}
		} else {
			DBG("Rotating session \"%s\" from its current \"NULL\" trace chunk to a new chunk",
			    session->name);
			/*
			 * Rotate existing streams into the new chunk.
			 * This is a "quiet" rotation has no client has
			 * explicitly requested this operation.
			 *
			 * There is also no need to wait for the rotation
			 * to complete as it will happen immediately. No data
			 * was produced as the session was stopped, so the
			 * rotation should happen on reception of the command.
			 */
			ret = (lttng_error_code) cmd_rotate_session(
				session, nullptr, true, LTTNG_TRACE_CHUNK_COMMAND_TYPE_NO_OPERATION);
			if (ret != LTTNG_OK) {
				goto error;
			}
		}
	}

	/* Kernel tracing */
	if (ksession != nullptr) {
		DBG("Start kernel tracing session %s", session->name);
		ret = (lttng_error_code) start_kernel_session(ksession);
		if (ret != LTTNG_OK) {
			goto error;
		}
	}

	/* Flag session that trace should start automatically */
	if (usess) {
		const int int_ret = ust_app_start_trace_all(usess);

		if (int_ret < 0) {
			ret = LTTNG_ERR_UST_START_FAIL;
			goto error;
		}
	}

	/*
	 * Open a packet in every stream of the session to ensure that viewers
	 * can correctly identify the boundaries of the periods during which
	 * tracing was active for this session.
	 */
	ret = session_open_packets(session);
	if (ret != LTTNG_OK) {
		goto error;
	}

	/*
	 * Clear the flag that indicates that a rotation was done while the
	 * session was stopped.
	 */
	session->rotated_after_last_stop = false;

	if (session->rotate_timer_period && !session->rotation_schedule_timer_enabled) {
		const int int_ret = timer_session_rotation_schedule_timer_start(
			session, session->rotate_timer_period);

		if (int_ret < 0) {
			ERR("Failed to enable rotate timer");
			ret = LTTNG_ERR_UNK;
			goto error;
		}
	}

	ret = LTTNG_OK;

error:
	if (ret == LTTNG_OK) {
		/* Flag this after a successful start. */
		session->has_been_started = true;
	} else {
		session->active = false;
		/* Restore initial state on error. */
		session->rotated_after_last_stop = session_rotated_after_last_stop;
		session->cleared_after_last_stop = session_cleared_after_last_stop;
	}
end:
	return ret;
}

/*
 * Command LTTNG_STOP_TRACE processed by the client thread.
 */
int cmd_stop_trace(const ltt_session::locked_ref& session)
{
	int ret;
	struct ltt_kernel_session *ksession;
	struct ltt_ust_session *usess;

	DBG("Begin stop session \"%s\" (id %" PRIu64 ")", session->name, session->id);
	/* Short cut */
	ksession = session->kernel_session;
	usess = session->ust_session;

	/* Session is not active. Skip everything and inform the client. */
	if (!session->active) {
		ret = LTTNG_ERR_TRACE_ALREADY_STOPPED;
		goto error;
	}

	ret = stop_kernel_session(ksession);
	if (ret != LTTNG_OK) {
		goto error;
	}

	if (usess && usess->active) {
		ret = ust_app_stop_trace_all(usess);
		if (ret < 0) {
			ret = LTTNG_ERR_UST_STOP_FAIL;
			goto error;
		}
	}

	DBG("Completed stop session \"%s\" (id %" PRIu64 ")", session->name, session->id);
	/* Flag inactive after a successful stop. */
	session->active = false;
	ret = LTTNG_OK;

error:
	return ret;
}

/*
 * Set the base_path of the session only if subdir of a control uris is set.
 * Return LTTNG_OK on success, otherwise LTTNG_ERR_*.
 */
static int set_session_base_path_from_uris(const ltt_session::locked_ref& session,
					   size_t nb_uri,
					   struct lttng_uri *uris)
{
	int ret;
	size_t i;

	for (i = 0; i < nb_uri; i++) {
		if (uris[i].stype != LTTNG_STREAM_CONTROL || uris[i].subdir[0] == '\0') {
			/* Not interested in these URIs */
			continue;
		}

		if (session->base_path != nullptr) {
			free(session->base_path);
			session->base_path = nullptr;
		}

		/* Set session base_path */
		session->base_path = strdup(uris[i].subdir);
		if (!session->base_path) {
			PERROR("Failed to copy base path \"%s\" to session \"%s\"",
			       uris[i].subdir,
			       session->name);
			ret = LTTNG_ERR_NOMEM;
			goto error;
		}
		DBG2("Setting base path \"%s\" for session \"%s\"",
		     session->base_path,
		     session->name);
	}
	ret = LTTNG_OK;
error:
	return ret;
}

/*
 * Command LTTNG_SET_CONSUMER_URI processed by the client thread.
 */
int cmd_set_consumer_uri(const ltt_session::locked_ref& session,
			 size_t nb_uri,
			 struct lttng_uri *uris)
{
	int ret, i;
	struct ltt_kernel_session *ksess = session->kernel_session;
	struct ltt_ust_session *usess = session->ust_session;

	LTTNG_ASSERT(uris);
	LTTNG_ASSERT(nb_uri > 0);

	/* Can't set consumer URI if the session is active. */
	if (session->active) {
		ret = LTTNG_ERR_TRACE_ALREADY_STARTED;
		goto error;
	}

	/*
	 * Set the session base path if any. This is done inside
	 * cmd_set_consumer_uri to preserve backward compatibility of the
	 * previous session creation api vs the session descriptor api.
	 */
	ret = set_session_base_path_from_uris(session, nb_uri, uris);
	if (ret != LTTNG_OK) {
		goto error;
	}

	/* Set the "global" consumer URIs */
	for (i = 0; i < nb_uri; i++) {
		ret = add_uri_to_consumer(session, session->consumer, &uris[i], LTTNG_DOMAIN_NONE);
		if (ret != LTTNG_OK) {
			goto error;
		}
	}

	/* Set UST session URIs */
	if (session->ust_session) {
		for (i = 0; i < nb_uri; i++) {
			ret = add_uri_to_consumer(session,
						  session->ust_session->consumer,
						  &uris[i],
						  LTTNG_DOMAIN_UST);
			if (ret != LTTNG_OK) {
				goto error;
			}
		}
	}

	/* Set kernel session URIs */
	if (session->kernel_session) {
		for (i = 0; i < nb_uri; i++) {
			ret = add_uri_to_consumer(session,
						  session->kernel_session->consumer,
						  &uris[i],
						  LTTNG_DOMAIN_KERNEL);
			if (ret != LTTNG_OK) {
				goto error;
			}
		}
	}

	/*
	 * Make sure to set the session in output mode after we set URI since a
	 * session can be created without URL (thus flagged in no output mode).
	 */
	session->output_traces = true;
	if (ksess) {
		ksess->output_traces = 1;
	}

	if (usess) {
		usess->output_traces = 1;
	}

	/* All good! */
	ret = LTTNG_OK;

error:
	return ret;
}

static enum lttng_error_code
set_session_output_from_descriptor(const ltt_session::locked_ref& session,
				   const struct lttng_session_descriptor *descriptor)
{
	int ret;
	enum lttng_error_code ret_code = LTTNG_OK;
	const lttng_session_descriptor_type session_type =
		lttng_session_descriptor_get_type(descriptor);
	const lttng_session_descriptor_output_type output_type =
		lttng_session_descriptor_get_output_type(descriptor);
	struct lttng_uri uris[2] = {};
	size_t uri_count = 0;

	switch (output_type) {
	case LTTNG_SESSION_DESCRIPTOR_OUTPUT_TYPE_NONE:
		goto end;
	case LTTNG_SESSION_DESCRIPTOR_OUTPUT_TYPE_LOCAL:
		lttng_session_descriptor_get_local_output_uri(descriptor, &uris[0]);
		uri_count = 1;
		break;
	case LTTNG_SESSION_DESCRIPTOR_OUTPUT_TYPE_NETWORK:
		if (utils_force_experimental_ctf_2()) {
			/* CTF2 can't be streamed yet. */
			ERR_FMT("Refusing network output with CTF2 format: session_name=`{}`",
				session->name);
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}

		lttng_session_descriptor_get_network_output_uris(descriptor, &uris[0], &uris[1]);
		uri_count = 2;
		break;
	default:
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}

	switch (session_type) {
	case LTTNG_SESSION_DESCRIPTOR_TYPE_SNAPSHOT:
	{
		struct snapshot_output *new_output = nullptr;

		new_output = snapshot_output_alloc();
		if (!new_output) {
			ret_code = LTTNG_ERR_NOMEM;
			goto end;
		}

		ret = snapshot_output_init_with_uri(session,
						    DEFAULT_SNAPSHOT_MAX_SIZE,
						    nullptr,
						    uris,
						    uri_count,
						    session->consumer,
						    new_output,
						    &session->snapshot);
		if (ret < 0) {
			ret_code = (ret == -ENOMEM) ? LTTNG_ERR_NOMEM : LTTNG_ERR_INVALID;
			snapshot_output_destroy(new_output);
			goto end;
		}
		snapshot_add_output(&session->snapshot, new_output);
		break;
	}
	case LTTNG_SESSION_DESCRIPTOR_TYPE_REGULAR:
	case LTTNG_SESSION_DESCRIPTOR_TYPE_LIVE:
	{
		ret_code = (lttng_error_code) cmd_set_consumer_uri(session, uri_count, uris);
		break;
	}
	default:
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}
end:
	return ret_code;
}

static enum lttng_error_code
cmd_create_session_from_descriptor(struct lttng_session_descriptor *descriptor,
				   const lttng_sock_cred *creds,
				   const char *home_path)
{
	int ret;
	enum lttng_error_code ret_code;
	const char *session_name;
	struct ltt_session *new_session = nullptr;
	enum lttng_session_descriptor_status descriptor_status;

	const auto list_lock = lttng::sessiond::lock_session_list();
	if (home_path) {
		if (*home_path != '/') {
			ERR("Home path provided by client is not absolute");
			ret_code = LTTNG_ERR_INVALID;
			goto end;
		}
	}

	descriptor_status = lttng_session_descriptor_get_session_name(descriptor, &session_name);
	switch (descriptor_status) {
	case LTTNG_SESSION_DESCRIPTOR_STATUS_OK:
		break;
	case LTTNG_SESSION_DESCRIPTOR_STATUS_UNSET:
		session_name = nullptr;
		break;
	default:
		ret_code = LTTNG_ERR_INVALID;
		goto end;
	}

	ret_code = session_create(session_name, creds->uid, creds->gid, &new_session);
	if (ret_code != LTTNG_OK) {
		goto end;
	}

	ret_code = notification_thread_command_add_session(the_notification_thread_handle,
							   new_session->id,
							   new_session->name,
							   new_session->uid,
							   new_session->gid);
	if (ret_code != LTTNG_OK) {
		goto end;
	}

	/* Announce the session's destruction to the notification thread when it is destroyed. */
	ret = session_add_destroy_notifier(
		[new_session]() {
			session_get(new_session);
			new_session->lock();
			return ltt_session::make_locked_ref(*new_session);
		}(),
		[](const ltt_session::locked_ref& session,
		   void *user_data __attribute__((unused))) {
			(void) notification_thread_command_remove_session(
				the_notification_thread_handle, session->id);
		},
		nullptr);
	if (ret) {
		PERROR("Failed to add notification thread command to session's destroy notifiers: session name = %s",
		       new_session->name);
		ret = LTTNG_ERR_NOMEM;
		goto end;
	}

	if (!session_name) {
		ret = lttng_session_descriptor_set_session_name(descriptor, new_session->name);
		if (ret) {
			ret_code = LTTNG_ERR_SESSION_FAIL;
			goto end;
		}
	}

	if (!lttng_session_descriptor_is_output_destination_initialized(descriptor)) {
		/*
		 * Only include the session's creation time in the output
		 * destination if the name of the session itself was
		 * not auto-generated.
		 */
		ret_code = lttng_session_descriptor_set_default_output(
			descriptor,
			session_name ? &new_session->creation_time : nullptr,
			home_path);
		if (ret_code != LTTNG_OK) {
			goto end;
		}
	} else {
		new_session->has_user_specified_directory =
			lttng_session_descriptor_has_output_directory(descriptor);
	}

	switch (lttng_session_descriptor_get_type(descriptor)) {
	case LTTNG_SESSION_DESCRIPTOR_TYPE_SNAPSHOT:
		new_session->snapshot_mode = true;
		break;
	case LTTNG_SESSION_DESCRIPTOR_TYPE_LIVE:
		new_session->live_timer =
			lttng_session_descriptor_live_get_timer_interval(descriptor);
		break;
	default:
		break;
	}

	ret_code = set_session_output_from_descriptor(
		[new_session]() {
			session_get(new_session);
			new_session->lock();
			return ltt_session::make_locked_ref(*new_session);
		}(),
		descriptor);
	if (ret_code != LTTNG_OK) {
		goto end;
	}
	new_session->consumer->enabled = true;
	ret_code = LTTNG_OK;
end:
	/* Release reference provided by the session_create function. */
	session_put(new_session);
	if (ret_code != LTTNG_OK && new_session) {
		/* Release the global reference on error. */
		session_destroy(new_session);
	}

	return ret_code;
}

enum lttng_error_code cmd_create_session(struct command_ctx *cmd_ctx,
					 int sock,
					 struct lttng_session_descriptor **return_descriptor)
{
	int ret;
	size_t payload_size;
	struct lttng_dynamic_buffer payload;
	struct lttng_buffer_view home_dir_view;
	struct lttng_buffer_view session_descriptor_view;
	struct lttng_session_descriptor *session_descriptor = nullptr;
	enum lttng_error_code ret_code;

	lttng_dynamic_buffer_init(&payload);
	if (cmd_ctx->lsm.u.create_session.home_dir_size >= LTTNG_PATH_MAX) {
		ret_code = LTTNG_ERR_INVALID;
		goto error;
	}
	if (cmd_ctx->lsm.u.create_session.session_descriptor_size >
	    LTTNG_SESSION_DESCRIPTOR_MAX_LEN) {
		ret_code = LTTNG_ERR_INVALID;
		goto error;
	}

	payload_size = cmd_ctx->lsm.u.create_session.home_dir_size +
		cmd_ctx->lsm.u.create_session.session_descriptor_size;
	ret = lttng_dynamic_buffer_set_size(&payload, payload_size);
	if (ret) {
		ret_code = LTTNG_ERR_NOMEM;
		goto error;
	}

	ret = lttcomm_recv_unix_sock(sock, payload.data, payload.size);
	if (ret <= 0) {
		ERR("Reception of session descriptor failed, aborting.");
		ret_code = LTTNG_ERR_SESSION_FAIL;
		goto error;
	}

	home_dir_view = lttng_buffer_view_from_dynamic_buffer(
		&payload, 0, cmd_ctx->lsm.u.create_session.home_dir_size);
	if (cmd_ctx->lsm.u.create_session.home_dir_size > 0 &&
	    !lttng_buffer_view_is_valid(&home_dir_view)) {
		ERR("Invalid payload in \"create session\" command: buffer too short to contain home directory");
		ret_code = LTTNG_ERR_INVALID_PROTOCOL;
		goto error;
	}

	session_descriptor_view = lttng_buffer_view_from_dynamic_buffer(
		&payload,
		cmd_ctx->lsm.u.create_session.home_dir_size,
		cmd_ctx->lsm.u.create_session.session_descriptor_size);
	if (!lttng_buffer_view_is_valid(&session_descriptor_view)) {
		ERR("Invalid payload in \"create session\" command: buffer too short to contain session descriptor");
		ret_code = LTTNG_ERR_INVALID_PROTOCOL;
		goto error;
	}

	ret = lttng_session_descriptor_create_from_buffer(&session_descriptor_view,
							  &session_descriptor);
	if (ret < 0) {
		ERR("Failed to create session descriptor from payload of \"create session\" command");
		ret_code = LTTNG_ERR_INVALID;
		goto error;
	}

	/*
	 * Sets the descriptor's auto-generated properties (name, output) if
	 * needed.
	 */
	ret_code = cmd_create_session_from_descriptor(session_descriptor,
						      &cmd_ctx->creds,
						      home_dir_view.size ? home_dir_view.data :
									   nullptr);
	if (ret_code != LTTNG_OK) {
		goto error;
	}

	ret_code = LTTNG_OK;
	*return_descriptor = session_descriptor;
	session_descriptor = nullptr;
error:
	lttng_dynamic_buffer_reset(&payload);
	lttng_session_descriptor_destroy(session_descriptor);
	return ret_code;
}

static void cmd_destroy_session_reply(const ltt_session::locked_ref& session, void *_reply_context)
{
	int ret;
	ssize_t comm_ret;
	const struct cmd_destroy_session_reply_context *reply_context =
		(cmd_destroy_session_reply_context *) _reply_context;
	struct lttng_dynamic_buffer payload;
	struct lttcomm_session_destroy_command_header cmd_header;
	struct lttng_trace_archive_location *location = nullptr;
	struct lttcomm_lttng_msg llm = {
		.cmd_type = LTTCOMM_SESSIOND_COMMAND_DESTROY_SESSION,
		.ret_code = reply_context->destruction_status,
		.pid = UINT32_MAX,
		.cmd_header_size = sizeof(struct lttcomm_session_destroy_command_header),
		.data_size = 0,
		.fd_count = 0,
	};
	size_t payload_size_before_location;

	lttng_dynamic_buffer_init(&payload);

	ret = lttng_dynamic_buffer_append(&payload, &llm, sizeof(llm));
	if (ret) {
		ERR("Failed to append session destruction message");
		goto error;
	}

	cmd_header.rotation_state = (int32_t) (reply_context->implicit_rotation_on_destroy ?
						       session->rotation_state :
						       LTTNG_ROTATION_STATE_NO_ROTATION);
	ret = lttng_dynamic_buffer_append(&payload, &cmd_header, sizeof(cmd_header));
	if (ret) {
		ERR("Failed to append session destruction command header");
		goto error;
	}

	if (!reply_context->implicit_rotation_on_destroy) {
		DBG("No implicit rotation performed during the destruction of session \"%s\", sending reply",
		    session->name);
		goto send_reply;
	}
	if (session->rotation_state != LTTNG_ROTATION_STATE_COMPLETED) {
		DBG("Rotation state of session \"%s\" is not \"completed\", sending session destruction reply",
		    session->name);
		goto send_reply;
	}

	location = session_get_trace_archive_location(session);
	if (!location) {
		ERR("Failed to get the location of the trace archive produced during the destruction of session \"%s\"",
		    session->name);
		goto error;
	}

	payload_size_before_location = payload.size;
	comm_ret = lttng_trace_archive_location_serialize(location, &payload);
	lttng_trace_archive_location_put(location);
	if (comm_ret < 0) {
		ERR("Failed to serialize the location of the trace archive produced during the destruction of session \"%s\"",
		    session->name);
		goto error;
	}
	/* Update the message to indicate the location's length. */
	((struct lttcomm_lttng_msg *) payload.data)->data_size =
		payload.size - payload_size_before_location;
send_reply:
	comm_ret = lttcomm_send_unix_sock(reply_context->reply_sock_fd, payload.data, payload.size);
	if (comm_ret != (ssize_t) payload.size) {
		ERR("Failed to send result of the destruction of session \"%s\" to client",
		    session->name);
	}
error:
	ret = close(reply_context->reply_sock_fd);
	if (ret) {
		PERROR("Failed to close client socket in deferred session destroy reply");
	}
	lttng_dynamic_buffer_reset(&payload);
	free(_reply_context);
}

/*
 * Command LTTNG_DESTROY_SESSION processed by the client thread.
 *
 * Called with session lock held.
 */
int cmd_destroy_session(const ltt_session::locked_ref& session, int *sock_fd)
{
	int ret;
	enum lttng_error_code destruction_last_error = LTTNG_OK;
	struct cmd_destroy_session_reply_context *reply_context = nullptr;

	if (sock_fd) {
		reply_context = zmalloc<cmd_destroy_session_reply_context>();
		if (!reply_context) {
			ret = LTTNG_ERR_NOMEM;
			goto end;
		}

		reply_context->reply_sock_fd = *sock_fd;
	}

	DBG("Begin destroy session %s (id %" PRIu64 ")", session->name, session->id);
	if (session->active) {
		DBG("Session \"%s\" is active, attempting to stop it before destroying it",
		    session->name);
		ret = cmd_stop_trace(session);
		if (ret != LTTNG_OK && ret != LTTNG_ERR_TRACE_ALREADY_STOPPED) {
			/* Carry on with the destruction of the session. */
			ERR("Failed to stop session \"%s\" as part of its destruction: %s",
			    session->name,
			    lttng_strerror(-ret));
			destruction_last_error = (lttng_error_code) ret;
		}
	}

	if (session->rotation_schedule_timer_enabled) {
		if (timer_session_rotation_schedule_timer_stop(session)) {
			ERR("Failed to stop the \"rotation schedule\" timer of session %s",
			    session->name);
			destruction_last_error = LTTNG_ERR_TIMER_STOP_ERROR;
		}
	}

	if (session->rotate_size) {
		try {
			the_rotation_thread_handle->unsubscribe_session_consumed_size_rotation(
				*session);
		} catch (const std::exception& e) {
			/* Continue the destruction of the session anyway. */
			ERR("Failed to unsubscribe rotation thread notification channel from consumed size condition during session destruction: %s",
			    e.what());
		}

		session->rotate_size = 0;
	}

	if (session->rotated && session->current_trace_chunk && session->output_traces) {
		/*
		 * Perform a last rotation on destruction if rotations have
		 * occurred during the session's lifetime.
		 */
		ret = cmd_rotate_session(
			session, nullptr, false, LTTNG_TRACE_CHUNK_COMMAND_TYPE_MOVE_TO_COMPLETED);
		if (ret != LTTNG_OK) {
			ERR("Failed to perform an implicit rotation as part of the destruction of session \"%s\": %s",
			    session->name,
			    lttng_strerror(-ret));
			destruction_last_error = (lttng_error_code) -ret;
		}
		if (reply_context) {
			reply_context->implicit_rotation_on_destroy = true;
		}
	} else if (session->has_been_started && session->current_trace_chunk) {
		/*
		 * The user has not triggered a session rotation. However, to
		 * ensure all data has been consumed, the session is rotated
		 * to a 'null' trace chunk before it is destroyed.
		 *
		 * This is a "quiet" rotation meaning that no notification is
		 * emitted and no renaming of the current trace chunk takes
		 * place.
		 */
		ret = cmd_rotate_session(
			session, nullptr, true, LTTNG_TRACE_CHUNK_COMMAND_TYPE_NO_OPERATION);
		/*
		 * Rotation operations may not be supported by the kernel
		 * tracer. Hence, do not consider this implicit rotation as
		 * a session destruction error. The library has already stopped
		 * the session and waited for pending data; there is nothing
		 * left to do but complete the destruction of the session.
		 */
		if (ret != LTTNG_OK && ret != -LTTNG_ERR_ROTATION_NOT_AVAILABLE_KERNEL) {
			ERR("Failed to perform a quiet rotation as part of the destruction of session \"%s\": %s",
			    session->name,
			    lttng_strerror(ret));
			destruction_last_error = (lttng_error_code) -ret;
		}
	}

	if (session->shm_path[0]) {
		/*
		 * When a session is created with an explicit shm_path,
		 * the consumer daemon will create its shared memory files
		 * at that location and will *not* unlink them. This is normal
		 * as the intention of that feature is to make it possible
		 * to retrieve the content of those files should a crash occur.
		 *
		 * To ensure the content of those files can be used, the
		 * sessiond daemon will replicate the content of the metadata
		 * cache in a metadata file.
		 *
		 * On clean-up, it is expected that the consumer daemon will
		 * unlink the shared memory files and that the session daemon
		 * will unlink the metadata file. Then, the session's directory
		 * in the shm path can be removed.
		 *
		 * Unfortunately, a flaw in the design of the sessiond's and
		 * consumerd's tear down of channels makes it impossible to
		 * determine when the sessiond _and_ the consumerd have both
		 * destroyed their representation of a channel. For one, the
		 * unlinking, close, and rmdir happen in deferred 'call_rcu'
		 * callbacks in both daemons.
		 *
		 * However, it is also impossible for the sessiond to know when
		 * the consumer daemon is done destroying its channel(s) since
		 * it occurs as a reaction to the closing of the channel's file
		 * descriptor. There is no resulting communication initiated
		 * from the consumerd to the sessiond to confirm that the
		 * operation is completed (and was successful).
		 *
		 * Until this is all fixed, the session daemon checks for the
		 * removal of the session's shm path which makes it possible
		 * to safely advertise a session as having been destroyed.
		 *
		 * Prior to this fix, it was not possible to reliably save
		 * a session making use of the --shm-path option, destroy it,
		 * and load it again. This is because the creation of the
		 * session would fail upon seeing the session's shm path
		 * already in existence.
		 *
		 * Note that none of the error paths in the check for the
		 * directory's existence return an error. This is normal
		 * as there isn't much that can be done. The session will
		 * be destroyed properly, except that we can't offer the
		 * guarantee that the same session can be re-created.
		 */
		current_completion_handler = &destroy_completion_handler.handler;
		ret = lttng_strncpy(destroy_completion_handler.shm_path,
				    session->shm_path,
				    sizeof(destroy_completion_handler.shm_path));
		LTTNG_ASSERT(!ret);
	}

	/*
	 * The session is destroyed. However, note that the command context
	 * still holds a reference to the session, thus delaying its destruction
	 * _at least_ up to the point when that reference is released.
	 */
	session_destroy(&session.get());
	if (reply_context) {
		reply_context->destruction_status = destruction_last_error;
		ret = session_add_destroy_notifier(
			session, cmd_destroy_session_reply, (void *) reply_context);
		if (ret) {
			ret = LTTNG_ERR_FATAL;
			goto end;
		} else {
			*sock_fd = -1;
		}
	}
	ret = LTTNG_OK;
end:
	return ret;
}

/*
 * Command LTTNG_REGISTER_CONSUMER processed by the client thread.
 */
int cmd_register_consumer(const ltt_session::locked_ref& session,
			  enum lttng_domain_type domain,
			  const char *sock_path,
			  struct consumer_data *cdata)
{
	int ret, sock;
	struct consumer_socket *socket = nullptr;

	LTTNG_ASSERT(cdata);
	LTTNG_ASSERT(sock_path);

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
	{
		struct ltt_kernel_session *ksess = session->kernel_session;

		LTTNG_ASSERT(ksess);

		/* Can't register a consumer if there is already one */
		if (ksess->consumer_fds_sent != 0) {
			ret = LTTNG_ERR_KERN_CONSUMER_FAIL;
			goto error;
		}

		sock = lttcomm_connect_unix_sock(sock_path);
		if (sock < 0) {
			ret = LTTNG_ERR_CONNECT_FAIL;
			goto error;
		}
		cdata->cmd_sock = sock;

		socket = consumer_allocate_socket(&cdata->cmd_sock);
		if (socket == nullptr) {
			ret = close(sock);
			if (ret < 0) {
				PERROR("close register consumer");
			}
			cdata->cmd_sock = -1;
			ret = LTTNG_ERR_FATAL;
			goto error;
		}

		socket->lock = zmalloc<pthread_mutex_t>();
		if (socket->lock == nullptr) {
			PERROR("zmalloc pthread mutex");
			ret = LTTNG_ERR_FATAL;
			goto error;
		}

		pthread_mutex_init(socket->lock, nullptr);
		socket->registered = 1;

		const lttng::urcu::read_lock_guard read_lock;
		consumer_add_socket(socket, ksess->consumer);

		pthread_mutex_lock(&cdata->pid_mutex);
		cdata->pid = -1;
		pthread_mutex_unlock(&cdata->pid_mutex);

		break;
	}
	default:
		/* TODO: Userspace tracing */
		ret = LTTNG_ERR_UND;
		goto error;
	}

	return LTTNG_OK;

error:
	if (socket) {
		consumer_destroy_socket(socket);
	}
	return ret;
}

/*
 * Command LTTNG_LIST_DOMAINS processed by the client thread.
 */
ssize_t cmd_list_domains(const ltt_session::locked_ref& session, struct lttng_domain **domains)
{
	int ret, index = 0;
	ssize_t nb_dom = 0;

	if (session->kernel_session != nullptr) {
		DBG3("Listing domains found kernel domain");
		nb_dom++;
	}

	if (session->ust_session != nullptr) {
		DBG3("Listing domains found UST global domain");
		nb_dom++;

		for (auto *agt :
		     lttng::urcu::lfht_iteration_adapter<agent, decltype(agent::node), &agent::node>(
			     *session->ust_session->agents->ht)) {
			if (agt->being_used) {
				nb_dom++;
			}
		}
	}

	if (!nb_dom) {
		goto end;
	}

	*domains = calloc<lttng_domain>(nb_dom);
	if (*domains == nullptr) {
		ret = LTTNG_ERR_FATAL;
		goto error;
	}

	if (session->kernel_session != nullptr) {
		(*domains)[index].type = LTTNG_DOMAIN_KERNEL;

		/* Kernel session buffer type is always GLOBAL */
		(*domains)[index].buf_type = LTTNG_BUFFER_GLOBAL;

		index++;
	}

	if (session->ust_session != nullptr) {
		(*domains)[index].type = LTTNG_DOMAIN_UST;
		(*domains)[index].buf_type = session->ust_session->buffer_type;
		index++;

		{
			const lttng::urcu::read_lock_guard read_lock;

			for (auto *agt : lttng::urcu::lfht_iteration_adapter<agent,
									     decltype(agent::node),
									     &agent::node>(
				     *session->ust_session->agents->ht)) {
				if (agt->being_used) {
					(*domains)[index].type = agt->domain;
					(*domains)[index].buf_type =
						session->ust_session->buffer_type;
					index++;
				}
			}
		}
	}
end:
	return nb_dom;

error:
	/* Return negative value to differentiate return code */
	return -ret;
}

/*
 * Command LTTNG_LIST_CHANNELS processed by the client thread.
 */
enum lttng_error_code cmd_list_channels(enum lttng_domain_type domain,
					const ltt_session::locked_ref& session,
					struct lttng_payload *payload)
{
	int ret = 0;
	unsigned int i = 0;
	struct lttcomm_list_command_header cmd_header = {};
	size_t cmd_header_offset;
	enum lttng_error_code ret_code;

	LTTNG_ASSERT(payload);

	DBG("Listing channels for session %s", session->name);

	cmd_header_offset = payload->buffer.size;

	/* Reserve space for command reply header. */
	ret = lttng_dynamic_buffer_set_size(&payload->buffer,
					    cmd_header_offset + sizeof(cmd_header));
	if (ret) {
		ret_code = LTTNG_ERR_NOMEM;
		goto end;
	}

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
	{
		/* Kernel channels */
		if (session->kernel_session != nullptr) {
			for (auto kchan :
			     lttng::urcu::list_iteration_adapter<ltt_kernel_channel,
								 &ltt_kernel_channel::list>(
				     session->kernel_session->channel_list.head)) {
				uint64_t discarded_events, lost_packets;
				struct lttng_channel_extended *extended;

				extended = (struct lttng_channel_extended *)
						   kchan->channel->attr.extended.ptr;

				ret = get_kernel_runtime_stats(
					session, kchan, &discarded_events, &lost_packets);
				if (ret < 0) {
					ret_code = LTTNG_ERR_UNK;
					goto end;
				}

				/*
				 * Update the discarded_events and lost_packets
				 * count for the channel
				 */
				extended->discarded_events = discarded_events;
				extended->lost_packets = lost_packets;

				ret = lttng_channel_serialize(kchan->channel, &payload->buffer);
				if (ret) {
					ERR("Failed to serialize lttng_channel: channel name = '%s'",
					    kchan->channel->name);
					ret_code = LTTNG_ERR_UNK;
					goto end;
				}

				i++;
			}
		}
		break;
	}
	case LTTNG_DOMAIN_UST:
	{
		for (auto *uchan :
		     lttng::urcu::lfht_iteration_adapter<ltt_ust_channel,
							 decltype(ltt_ust_channel::node),
							 &ltt_ust_channel::node>(
			     *session->ust_session->domain_global.channels->ht)) {
			uint64_t discarded_events = 0, lost_packets = 0;
			struct lttng_channel *channel = nullptr;
			struct lttng_channel_extended *extended;

			channel = trace_ust_channel_to_lttng_channel(uchan);
			if (!channel) {
				ret_code = LTTNG_ERR_NOMEM;
				goto end;
			}

			extended = (struct lttng_channel_extended *) channel->attr.extended.ptr;

			ret = get_ust_runtime_stats(
				session, uchan, &discarded_events, &lost_packets);
			if (ret < 0) {
				lttng_channel_destroy(channel);
				ret_code = LTTNG_ERR_UNK;
				goto end;
			}

			extended->discarded_events = discarded_events;
			extended->lost_packets = lost_packets;

			ret = lttng_channel_serialize(channel, &payload->buffer);
			if (ret) {
				ERR("Failed to serialize lttng_channel: channel name = '%s'",
				    channel->name);
				lttng_channel_destroy(channel);
				ret_code = LTTNG_ERR_UNK;
				goto end;
			}

			lttng_channel_destroy(channel);
			i++;
		}

		break;
	}
	default:
		break;
	}

	if (i > UINT32_MAX) {
		ERR("Channel count would overflow the channel listing command's reply");
		ret_code = LTTNG_ERR_OVERFLOW;
		goto end;
	}

	/* Update command reply header. */
	cmd_header.count = (uint32_t) i;
	memcpy(payload->buffer.data + cmd_header_offset, &cmd_header, sizeof(cmd_header));
	ret_code = LTTNG_OK;

end:
	return ret_code;
}

/*
 * Command LTTNG_LIST_EVENTS processed by the client thread.
 */
enum lttng_error_code cmd_list_events(enum lttng_domain_type domain,
				      const ltt_session::locked_ref& session,
				      char *channel_name,
				      struct lttng_payload *reply_payload)
{
	int buffer_resize_ret;
	enum lttng_error_code ret_code = LTTNG_OK;
	struct lttcomm_list_command_header reply_command_header = {};
	size_t reply_command_header_offset;
	unsigned int nb_events = 0;

	assert(reply_payload);

	/* Reserve space for command reply header. */
	reply_command_header_offset = reply_payload->buffer.size;
	buffer_resize_ret = lttng_dynamic_buffer_set_size(
		&reply_payload->buffer,
		reply_command_header_offset + sizeof(struct lttcomm_list_command_header));
	if (buffer_resize_ret) {
		ret_code = LTTNG_ERR_NOMEM;
		goto end;
	}

	switch (domain) {
	case LTTNG_DOMAIN_KERNEL:
		if (session->kernel_session != nullptr) {
			ret_code = list_lttng_kernel_events(
				channel_name, session->kernel_session, reply_payload, &nb_events);
		}

		break;
	case LTTNG_DOMAIN_UST:
	{
		if (session->ust_session != nullptr) {
			ret_code =
				list_lttng_ust_global_events(channel_name,
							     &session->ust_session->domain_global,
							     reply_payload,
							     &nb_events);
		}

		break;
	}
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_PYTHON:
		if (session->ust_session) {
			for (auto *agt : lttng::urcu::lfht_iteration_adapter<agent,
									     decltype(agent::node),
									     &agent::node>(
				     *session->ust_session->agents->ht)) {
				if (agt->domain == domain) {
					ret_code = list_lttng_agent_events(
						agt, reply_payload, &nb_events);
					break;
				}
			}
		}
		break;
	default:
		ret_code = LTTNG_ERR_UND;
		break;
	}

	if (nb_events > UINT32_MAX) {
		ret_code = LTTNG_ERR_OVERFLOW;
		goto end;
	}

	/* Update command reply header. */
	reply_command_header.count = (uint32_t) nb_events;
	memcpy(reply_payload->buffer.data + reply_command_header_offset,
	       &reply_command_header,
	       sizeof(reply_command_header));

end:
	return ret_code;
}

/*
 * Using the session list, filled a lttng_session array to send back to the
 * client for session listing.
 *
 * The session list lock MUST be acquired before calling this function.
 */
void cmd_list_lttng_sessions(struct lttng_session *sessions,
			     size_t session_count,
			     uid_t uid,
			     gid_t gid)
{
	int ret;
	unsigned int i = 0;
	struct ltt_session_list *list = session_get_list();
	struct lttng_session_extended *extended = (typeof(extended)) (&sessions[session_count]);

	DBG("Getting all available session for UID %d GID %d", uid, gid);
	/*
	 * Iterate over session list and append data after the control struct in
	 * the buffer.
	 */
	for (auto raw_session_ptr :
	     lttng::urcu::list_iteration_adapter<ltt_session, &ltt_session::list>(list->head)) {
		auto session = [raw_session_ptr]() {
			session_get(raw_session_ptr);
			raw_session_ptr->lock();
			return ltt_session::make_locked_ref(*raw_session_ptr);
		}();

		/*
		 * Only list the sessions the user can control.
		 */
		if (!session_access_ok(session, uid) || session->destroyed) {
			continue;
		}

		struct ltt_kernel_session *ksess = session->kernel_session;
		struct ltt_ust_session *usess = session->ust_session;

		if (session->consumer->type == CONSUMER_DST_NET ||
		    (ksess && ksess->consumer->type == CONSUMER_DST_NET) ||
		    (usess && usess->consumer->type == CONSUMER_DST_NET)) {
			ret = build_network_session_path(
				sessions[i].path, sizeof(sessions[i].path), session);
		} else {
			ret = snprintf(sessions[i].path,
				       sizeof(sessions[i].path),
				       "%s",
				       session->consumer->dst.session_root_path);
		}
		if (ret < 0) {
			PERROR("snprintf session path");
			continue;
		}

		strncpy(sessions[i].name, session->name, NAME_MAX);
		sessions[i].name[NAME_MAX - 1] = '\0';
		sessions[i].enabled = session->active;
		sessions[i].snapshot_mode = session->snapshot_mode;
		sessions[i].live_timer_interval = session->live_timer;
		extended[i].creation_time.value = (uint64_t) session->creation_time;
		extended[i].creation_time.is_set = 1;
		strncpy(extended[i].shm_path.value, session->shm_path, LTTNG_PATH_MAX);
		i++;
	}
}

/*
 * Command LTTCOMM_SESSIOND_COMMAND_KERNEL_TRACER_STATUS
 */
enum lttng_error_code cmd_kernel_tracer_status(enum lttng_kernel_tracer_status *status)
{
	if (status == nullptr) {
		return LTTNG_ERR_INVALID;
	}

	*status = get_kernel_tracer_status();
	return LTTNG_OK;
}

/*
 * Command LTTNG_DATA_PENDING returning 0 if the data is NOT pending meaning
 * ready for trace analysis (or any kind of reader) or else 1 for pending data.
 */
int cmd_data_pending(const ltt_session::locked_ref& session)
{
	int ret;
	struct ltt_kernel_session *ksess = session->kernel_session;
	struct ltt_ust_session *usess = session->ust_session;

	DBG("Data pending for session %s", session->name);

	/* Session MUST be stopped to ask for data availability. */
	if (session->active) {
		ret = LTTNG_ERR_SESSION_STARTED;
		goto error;
	} else {
		/*
		 * If stopped, just make sure we've started before else the above call
		 * will always send that there is data pending.
		 *
		 * The consumer assumes that when the data pending command is received,
		 * the trace has been started before or else no output data is written
		 * by the streams which is a condition for data pending. So, this is
		 * *VERY* important that we don't ask the consumer before a start
		 * trace.
		 */
		if (!session->has_been_started) {
			ret = 0;
			goto error;
		}
	}

	/* A rotation is still pending, we have to wait. */
	if (session->rotation_state == LTTNG_ROTATION_STATE_ONGOING) {
		DBG("Rotate still pending for session %s", session->name);
		ret = 1;
		goto error;
	}

	if (ksess && ksess->consumer) {
		ret = consumer_is_data_pending(ksess->id, ksess->consumer);
		if (ret == 1) {
			/* Data is still being extracted for the kernel. */
			goto error;
		}
	}

	if (usess && usess->consumer) {
		ret = consumer_is_data_pending(usess->id, usess->consumer);
		if (ret == 1) {
			/* Data is still being extracted for the kernel. */
			goto error;
		}
	}

	/* Data is ready to be read by a viewer */
	ret = 0;

error:
	return ret;
}

/*
 * Command LTTNG_SNAPSHOT_ADD_OUTPUT from the lttng ctl library.
 *
 * Return LTTNG_OK on success or else a LTTNG_ERR code.
 */
int cmd_snapshot_add_output(const ltt_session::locked_ref& session,
			    const struct lttng_snapshot_output *output,
			    uint32_t *id)
{
	int ret;
	struct snapshot_output *new_output;

	LTTNG_ASSERT(output);

	DBG("Cmd snapshot add output for session %s", session->name);

	/*
	 * Can't create an output if the session is not set in no-output mode.
	 */
	if (session->output_traces) {
		ret = LTTNG_ERR_NOT_SNAPSHOT_SESSION;
		goto error;
	}

	if (session->has_non_mmap_channel) {
		ret = LTTNG_ERR_SNAPSHOT_UNSUPPORTED;
		goto error;
	}

	/* Only one output is allowed until we have the "tee" feature. */
	if (session->snapshot.nb_output == 1) {
		ret = LTTNG_ERR_SNAPSHOT_OUTPUT_EXIST;
		goto error;
	}

	new_output = snapshot_output_alloc();
	if (!new_output) {
		ret = LTTNG_ERR_NOMEM;
		goto error;
	}

	ret = snapshot_output_init(session,
				   output->max_size,
				   output->name,
				   output->ctrl_url,
				   output->data_url,
				   session->consumer,
				   new_output,
				   &session->snapshot);
	if (ret < 0) {
		if (ret == -ENOMEM) {
			ret = LTTNG_ERR_NOMEM;
		} else {
			ret = LTTNG_ERR_INVALID;
		}
		goto free_error;
	}

	snapshot_add_output(&session->snapshot, new_output);
	if (id) {
		*id = new_output->id;
	}

	return LTTNG_OK;

free_error:
	snapshot_output_destroy(new_output);
error:
	return ret;
}

/*
 * Command LTTNG_SNAPSHOT_DEL_OUTPUT from lib lttng ctl.
 *
 * Return LTTNG_OK on success or else a LTTNG_ERR code.
 */
int cmd_snapshot_del_output(const ltt_session::locked_ref& session,
			    const struct lttng_snapshot_output *output)
{
	int ret;
	struct snapshot_output *sout = nullptr;

	LTTNG_ASSERT(output);

	const lttng::urcu::read_lock_guard read_lock;

	/*
	 * Permission denied to create an output if the session is not
	 * set in no output mode.
	 */
	if (session->output_traces) {
		ret = LTTNG_ERR_NOT_SNAPSHOT_SESSION;
		goto error;
	}

	if (output->id) {
		DBG("Cmd snapshot del output id %" PRIu32 " for session %s",
		    output->id,
		    session->name);
		sout = snapshot_find_output_by_id(output->id, &session->snapshot);
	} else if (*output->name != '\0') {
		DBG("Cmd snapshot del output name %s for session %s", output->name, session->name);
		sout = snapshot_find_output_by_name(output->name, &session->snapshot);
	}
	if (!sout) {
		ret = LTTNG_ERR_INVALID;
		goto error;
	}

	snapshot_delete_output(&session->snapshot, sout);
	snapshot_output_destroy(sout);
	ret = LTTNG_OK;

error:
	return ret;
}

/*
 * Command LTTNG_SNAPSHOT_LIST_OUTPUT from lib lttng ctl.
 *
 * If no output is available, outputs is untouched and 0 is returned.
 *
 * Return the size of the newly allocated outputs or a negative LTTNG_ERR code.
 */
ssize_t cmd_snapshot_list_outputs(const ltt_session::locked_ref& session,
				  struct lttng_snapshot_output **outputs)
{
	int ret, idx = 0;
	struct lttng_snapshot_output *list = nullptr;

	LTTNG_ASSERT(outputs);

	DBG("Cmd snapshot list outputs for session %s", session->name);

	/*
	 * Permission denied to create an output if the session is not
	 * set in no output mode.
	 */
	if (session->output_traces) {
		ret = -LTTNG_ERR_NOT_SNAPSHOT_SESSION;
		goto end;
	}

	if (session->snapshot.nb_output == 0) {
		ret = 0;
		goto end;
	}

	list = calloc<lttng_snapshot_output>(session->snapshot.nb_output);
	if (!list) {
		ret = -LTTNG_ERR_NOMEM;
		goto end;
	}

	/* Copy list from session to the new list object. */
	for (auto *output : lttng::urcu::lfht_iteration_adapter<snapshot_output,
								decltype(snapshot_output::node),
								&snapshot_output::node>(
		     *session->snapshot.output_ht->ht)) {
		LTTNG_ASSERT(output->consumer);
		list[idx].id = output->id;
		list[idx].max_size = output->max_size;
		if (lttng_strncpy(list[idx].name, output->name, sizeof(list[idx].name))) {
			ret = -LTTNG_ERR_INVALID;
			goto error;
		}

		if (output->consumer->type == CONSUMER_DST_LOCAL) {
			if (lttng_strncpy(list[idx].ctrl_url,
					  output->consumer->dst.session_root_path,
					  sizeof(list[idx].ctrl_url))) {
				ret = -LTTNG_ERR_INVALID;
				goto error;
			}
		} else {
			/* Control URI. */
			ret = uri_to_str_url(&output->consumer->dst.net.control,
					     list[idx].ctrl_url,
					     sizeof(list[idx].ctrl_url));
			if (ret < 0) {
				ret = -LTTNG_ERR_NOMEM;
				goto error;
			}

			/* Data URI. */
			ret = uri_to_str_url(&output->consumer->dst.net.data,
					     list[idx].data_url,
					     sizeof(list[idx].data_url));
			if (ret < 0) {
				ret = -LTTNG_ERR_NOMEM;
				goto error;
			}
		}

		idx++;
	}

	*outputs = list;
	list = nullptr;
	ret = session->snapshot.nb_output;
error:
	free(list);
end:
	return ret;
}

/*
 * Check if we can regenerate the metadata for this session.
 * Only kernel, UST per-uid and non-live sessions are supported.
 *
 * Return 0 if the metadata can be generated, a LTTNG_ERR code otherwise.
 */
static int check_regenerate_metadata_support(const ltt_session::locked_ref& session)
{
	int ret;

	if (session->live_timer != 0) {
		ret = LTTNG_ERR_LIVE_SESSION;
		goto end;
	}
	if (!session->active) {
		ret = LTTNG_ERR_SESSION_NOT_STARTED;
		goto end;
	}
	if (session->ust_session) {
		switch (session->ust_session->buffer_type) {
		case LTTNG_BUFFER_PER_UID:
			break;
		case LTTNG_BUFFER_PER_PID:
			ret = LTTNG_ERR_PER_PID_SESSION;
			goto end;
		default:
			abort();
			ret = LTTNG_ERR_UNK;
			goto end;
		}
	}
	if (session->consumer->type == CONSUMER_DST_NET &&
	    session->consumer->relay_minor_version < 8) {
		ret = LTTNG_ERR_RELAYD_VERSION_FAIL;
		goto end;
	}
	ret = 0;

end:
	return ret;
}

/*
 * Command LTTNG_REGENERATE_METADATA from the lttng-ctl library.
 *
 * Ask the consumer to truncate the existing metadata file(s) and
 * then regenerate the metadata. Live and per-pid sessions are not
 * supported and return an error.
 *
 * Return LTTNG_OK on success or else a LTTNG_ERR code.
 */
int cmd_regenerate_metadata(const ltt_session::locked_ref& session)
{
	int ret;

	ret = check_regenerate_metadata_support(session);
	if (ret) {
		goto end;
	}

	if (session->kernel_session) {
		ret = kernctl_session_regenerate_metadata(session->kernel_session->fd);
		if (ret < 0) {
			ERR("Failed to regenerate the kernel metadata");
			goto end;
		}
	}

	if (session->ust_session) {
		ret = trace_ust_regenerate_metadata(session->ust_session);
		if (ret < 0) {
			ERR("Failed to regenerate the UST metadata");
			goto end;
		}
	}
	DBG("Cmd metadata regenerate for session %s", session->name);
	ret = LTTNG_OK;

end:
	return ret;
}

/*
 * Command LTTNG_REGENERATE_STATEDUMP from the lttng-ctl library.
 *
 * Ask the tracer to regenerate a new statedump.
 *
 * Return LTTNG_OK on success or else a LTTNG_ERR code.
 */
int cmd_regenerate_statedump(const ltt_session::locked_ref& session)
{
	int ret;

	if (!session->active) {
		ret = LTTNG_ERR_SESSION_NOT_STARTED;
		goto end;
	}

	if (session->kernel_session) {
		ret = kernctl_session_regenerate_statedump(session->kernel_session->fd);
		/*
		 * Currently, the statedump in kernel can only fail if out
		 * of memory.
		 */
		if (ret < 0) {
			if (ret == -ENOMEM) {
				ret = LTTNG_ERR_REGEN_STATEDUMP_NOMEM;
			} else {
				ret = LTTNG_ERR_REGEN_STATEDUMP_FAIL;
			}
			ERR("Failed to regenerate the kernel statedump");
			goto end;
		}
	}

	if (session->ust_session) {
		ret = ust_app_regenerate_statedump_all(session->ust_session);
		/*
		 * Currently, the statedump in UST always returns 0.
		 */
		if (ret < 0) {
			ret = LTTNG_ERR_REGEN_STATEDUMP_FAIL;
			ERR("Failed to regenerate the UST statedump");
			goto end;
		}
	}
	DBG("Cmd regenerate statedump for session %s", session->name);
	ret = LTTNG_OK;

end:
	return ret;
}

static enum lttng_error_code
synchronize_tracer_notifier_register(struct notification_thread_handle *notification_thread,
				     struct lttng_trigger *trigger,
				     const struct lttng_credentials *cmd_creds)
{
	enum lttng_error_code ret_code;
	const struct lttng_condition *condition = lttng_trigger_get_const_condition(trigger);
	const char *trigger_name;
	uid_t trigger_owner;
	enum lttng_trigger_status trigger_status;
	const enum lttng_domain_type trigger_domain =
		lttng_trigger_get_underlying_domain_type_restriction(trigger);

	trigger_status = lttng_trigger_get_owner_uid(trigger, &trigger_owner);
	LTTNG_ASSERT(trigger_status == LTTNG_TRIGGER_STATUS_OK);

	LTTNG_ASSERT(condition);
	LTTNG_ASSERT(lttng_condition_get_type(condition) ==
		     LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES);

	trigger_status = lttng_trigger_get_name(trigger, &trigger_name);
	trigger_name = trigger_status == LTTNG_TRIGGER_STATUS_OK ? trigger_name : "(anonymous)";

	const auto list_lock = lttng::sessiond::lock_session_list();
	switch (trigger_domain) {
	case LTTNG_DOMAIN_KERNEL:
	{
		ret_code = kernel_register_event_notifier(trigger, cmd_creds);
		if (ret_code != LTTNG_OK) {
			enum lttng_error_code notif_thread_unregister_ret;

			notif_thread_unregister_ret =
				notification_thread_command_unregister_trigger(notification_thread,
									       trigger);

			if (notif_thread_unregister_ret != LTTNG_OK) {
				/* Return the original error code. */
				ERR("Failed to unregister trigger from notification thread during error recovery: trigger name = '%s', trigger owner uid = %d, error code = %d",
				    trigger_name,
				    (int) trigger_owner,
				    ret_code);
			}

			return ret_code;
		}
		break;
	}
	case LTTNG_DOMAIN_UST:
		ust_app_global_update_all_event_notifier_rules();
		break;
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_PYTHON:
	{
		/* Agent domains. */
		struct agent *agt = agent_find_by_event_notifier_domain(trigger_domain);

		if (!agt) {
			agt = agent_create(trigger_domain);
			if (!agt) {
				ret_code = LTTNG_ERR_NOMEM;
				return ret_code;
			}

			/* Ownership of agt is transferred. */
			agent_add(agt, the_trigger_agents_ht_by_domain);
		}

		ret_code = (lttng_error_code) trigger_agent_enable(trigger, agt);
		if (ret_code != LTTNG_OK) {
			return ret_code;
		}

		break;
	}
	case LTTNG_DOMAIN_NONE:
	default:
		abort();
	}

	return LTTNG_OK;
}

lttng::ctl::trigger cmd_register_trigger(const struct lttng_credentials *cmd_creds,
					 struct lttng_trigger *trigger,
					 bool is_trigger_anonymous,
					 struct notification_thread_handle *notification_thread)
{
	enum lttng_error_code ret_code;
	const char *trigger_name;
	uid_t trigger_owner;
	enum lttng_trigger_status trigger_status;

	trigger_status = lttng_trigger_get_name(trigger, &trigger_name);
	trigger_name = trigger_status == LTTNG_TRIGGER_STATUS_OK ? trigger_name : "(anonymous)";

	trigger_status = lttng_trigger_get_owner_uid(trigger, &trigger_owner);
	LTTNG_ASSERT(trigger_status == LTTNG_TRIGGER_STATUS_OK);

	DBG("Running register trigger command: trigger name = '%s', trigger owner uid = %d, command creds uid = %d",
	    trigger_name,
	    (int) trigger_owner,
	    (int) lttng_credentials_get_uid(cmd_creds));

	/*
	 * Validate the trigger credentials against the command credentials.
	 * Only the root user can register a trigger with non-matching
	 * credentials.
	 */
	if (!lttng_credentials_is_equal_uid(lttng_trigger_get_credentials(trigger), cmd_creds)) {
		if (lttng_credentials_get_uid(cmd_creds) != 0) {
			LTTNG_THROW_CTL(
				fmt::format(
					"Trigger credentials do not match the command credentials: trigger_name = `{}`, trigger_owner_uid={}, command_creds_uid={}",
					trigger_name,
					trigger_owner,
					lttng_credentials_get_uid(cmd_creds)),
				LTTNG_ERR_INVALID_TRIGGER);
		}
	}

	/*
	 * The bytecode generation also serves as a validation step for the
	 * bytecode expressions.
	 */
	ret_code = lttng_trigger_generate_bytecode(trigger, cmd_creds);
	if (ret_code != LTTNG_OK) {
		LTTNG_THROW_CTL(
			fmt::format(
				"Failed to generate bytecode of trigger: trigger_name=`{}`, trigger_owner_uid={}",
				trigger_name,
				trigger_owner),
			ret_code);
	}

	/*
	 * A reference to the trigger is acquired by the notification thread.
	 * It is safe to return the same trigger to the caller since it the
	 * other user holds a reference.
	 *
	 * The trigger is modified during the execution of the
	 * "register trigger" command. However, by the time the command returns,
	 * it is safe to use without any locking as its properties are
	 * immutable.
	 */
	ret_code = notification_thread_command_register_trigger(
		notification_thread, trigger, is_trigger_anonymous);
	if (ret_code != LTTNG_OK) {
		LTTNG_THROW_CTL(
			fmt::format(
				"Failed to register trigger to notification thread: trigger_name=`{}`, trigger_owner_uid={}",
				trigger_name,
				trigger_owner),
			ret_code);
	}

	trigger_status = lttng_trigger_get_name(trigger, &trigger_name);
	trigger_name = trigger_status == LTTNG_TRIGGER_STATUS_OK ? trigger_name : "(anonymous)";

	/*
	 * Synchronize tracers if the trigger adds an event notifier.
	 */
	if (lttng_trigger_needs_tracer_notifier(trigger)) {
		ret_code = synchronize_tracer_notifier_register(
			notification_thread, trigger, cmd_creds);
		if (ret_code != LTTNG_OK) {
			LTTNG_THROW_CTL("Failed to register tracer notifier", ret_code);
		}
	}

	/*
	 * Return an updated trigger to the client.
	 *
	 * Since a modified version of the same trigger is returned, acquire a
	 * reference to the trigger so the caller doesn't have to care if those
	 * are distinct instances or not.
	 */
	LTTNG_ASSERT(ret_code == LTTNG_OK);
	lttng_trigger_get(trigger);
	return lttng::ctl::trigger(trigger);
}

static enum lttng_error_code
synchronize_tracer_notifier_unregister(const struct lttng_trigger *trigger)
{
	enum lttng_error_code ret_code;
	const struct lttng_condition *condition = lttng_trigger_get_const_condition(trigger);
	const enum lttng_domain_type trigger_domain =
		lttng_trigger_get_underlying_domain_type_restriction(trigger);

	LTTNG_ASSERT(condition);
	LTTNG_ASSERT(lttng_condition_get_type(condition) ==
		     LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES);

	const auto list_lock = lttng::sessiond::lock_session_list();
	switch (trigger_domain) {
	case LTTNG_DOMAIN_KERNEL:
		ret_code = kernel_unregister_event_notifier(trigger);
		if (ret_code != LTTNG_OK) {
			return ret_code;
		}

		break;
	case LTTNG_DOMAIN_UST:
		ust_app_global_update_all_event_notifier_rules();
		break;
	case LTTNG_DOMAIN_JUL:
	case LTTNG_DOMAIN_LOG4J:
	case LTTNG_DOMAIN_LOG4J2:
	case LTTNG_DOMAIN_PYTHON:
	{
		/* Agent domains. */
		struct agent *agt = agent_find_by_event_notifier_domain(trigger_domain);

		/*
		 * This trigger was never registered in the first place. Calling
		 * this function under those circumstances is an internal error.
		 */
		LTTNG_ASSERT(agt);
		ret_code = (lttng_error_code) trigger_agent_disable(trigger, agt);
		if (ret_code != LTTNG_OK) {
			return ret_code;
		}

		break;
	}
	case LTTNG_DOMAIN_NONE:
	default:
		abort();
	}

	return LTTNG_OK;
}

enum lttng_error_code cmd_unregister_trigger(const struct lttng_credentials *cmd_creds,
					     const struct lttng_trigger *trigger,
					     struct notification_thread_handle *notification_thread)
{
	enum lttng_error_code ret_code;
	const char *trigger_name;
	uid_t trigger_owner;
	enum lttng_trigger_status trigger_status;
	struct lttng_trigger *sessiond_trigger = nullptr;

	trigger_status = lttng_trigger_get_name(trigger, &trigger_name);
	trigger_name = trigger_status == LTTNG_TRIGGER_STATUS_OK ? trigger_name : "(anonymous)";
	trigger_status = lttng_trigger_get_owner_uid(trigger, &trigger_owner);
	LTTNG_ASSERT(trigger_status == LTTNG_TRIGGER_STATUS_OK);

	DBG("Running unregister trigger command: trigger name = '%s', trigger owner uid = %d, command creds uid = %d",
	    trigger_name,
	    (int) trigger_owner,
	    (int) lttng_credentials_get_uid(cmd_creds));

	/*
	 * Validate the trigger credentials against the command credentials.
	 * Only the root user can unregister a trigger with non-matching
	 * credentials.
	 */
	if (!lttng_credentials_is_equal_uid(lttng_trigger_get_credentials(trigger), cmd_creds)) {
		if (lttng_credentials_get_uid(cmd_creds) != 0) {
			ERR("Trigger credentials do not match the command credentials: trigger name = '%s', trigger owner uid = %d, command creds uid = %d",
			    trigger_name,
			    (int) trigger_owner,
			    (int) lttng_credentials_get_uid(cmd_creds));
			ret_code = LTTNG_ERR_INVALID_TRIGGER;
			goto end;
		}
	}

	/* Fetch the sessiond side trigger object. */
	ret_code = notification_thread_command_get_trigger(
		notification_thread, trigger, &sessiond_trigger);
	if (ret_code != LTTNG_OK) {
		DBG("Failed to get trigger from notification thread during unregister: trigger name = '%s', trigger owner uid = %d, error code = %d",
		    trigger_name,
		    (int) trigger_owner,
		    ret_code);
		goto end;
	}

	LTTNG_ASSERT(sessiond_trigger);

	/*
	 * From this point on, no matter what, consider the trigger
	 * unregistered.
	 *
	 * We set the unregistered state of the sessiond side trigger object in
	 * the client thread since we want to minimize the possibility of the
	 * notification thread being stalled due to a long execution of an
	 * action that required the trigger lock.
	 */
	lttng_trigger_set_as_unregistered(sessiond_trigger);

	ret_code = notification_thread_command_unregister_trigger(notification_thread, trigger);
	if (ret_code != LTTNG_OK) {
		DBG("Failed to unregister trigger from notification thread: trigger name = '%s', trigger owner uid = %d, error code = %d",
		    trigger_name,
		    (int) trigger_owner,
		    ret_code);
		goto end;
	}

	/*
	 * Synchronize tracers if the trigger removes an event notifier.
	 * Do this even if the trigger unregistration failed to at least stop
	 * the tracers from producing notifications associated with this
	 * event notifier.
	 */
	if (lttng_trigger_needs_tracer_notifier(trigger)) {
		ret_code = synchronize_tracer_notifier_unregister(trigger);
		if (ret_code != LTTNG_OK) {
			ERR("Error unregistering trigger to tracer.");
			goto end;
		}
	}

end:
	lttng_trigger_put(sessiond_trigger);
	return ret_code;
}

enum lttng_error_code cmd_list_triggers(struct command_ctx *cmd_ctx,
					struct notification_thread_handle *notification_thread,
					struct lttng_triggers **return_triggers)
{
	int ret;
	enum lttng_error_code ret_code;
	struct lttng_triggers *triggers = nullptr;

	/* Get the set of triggers from the notification thread. */
	ret_code = notification_thread_command_list_triggers(
		notification_thread, cmd_ctx->creds.uid, &triggers);
	if (ret_code != LTTNG_OK) {
		goto end;
	}

	ret = lttng_triggers_remove_hidden_triggers(triggers);
	if (ret) {
		ret_code = LTTNG_ERR_UNK;
		goto end;
	}

	*return_triggers = triggers;
	triggers = nullptr;
	ret_code = LTTNG_OK;
end:
	lttng_triggers_destroy(triggers);
	return ret_code;
}

enum lttng_error_code
cmd_execute_error_query(const struct lttng_credentials *cmd_creds,
			const struct lttng_error_query *query,
			struct lttng_error_query_results **_results,
			struct notification_thread_handle *notification_thread)
{
	enum lttng_error_code ret_code;
	const struct lttng_trigger *query_target_trigger;
	const struct lttng_action *query_target_action = nullptr;
	struct lttng_trigger *matching_trigger = nullptr;
	const char *trigger_name;
	uid_t trigger_owner;
	enum lttng_trigger_status trigger_status;
	struct lttng_error_query_results *results = nullptr;

	switch (lttng_error_query_get_target_type(query)) {
	case LTTNG_ERROR_QUERY_TARGET_TYPE_TRIGGER:
		query_target_trigger = lttng_error_query_trigger_borrow_target(query);
		break;
	case LTTNG_ERROR_QUERY_TARGET_TYPE_CONDITION:
		query_target_trigger = lttng_error_query_condition_borrow_target(query);
		break;
	case LTTNG_ERROR_QUERY_TARGET_TYPE_ACTION:
		query_target_trigger = lttng_error_query_action_borrow_trigger_target(query);
		break;
	default:
		abort();
	}

	LTTNG_ASSERT(query_target_trigger);

	ret_code = notification_thread_command_get_trigger(
		notification_thread, query_target_trigger, &matching_trigger);
	if (ret_code != LTTNG_OK) {
		goto end;
	}

	/* No longer needed. */
	query_target_trigger = nullptr;

	if (lttng_error_query_get_target_type(query) == LTTNG_ERROR_QUERY_TARGET_TYPE_ACTION) {
		/* Get the sessiond-side version of the target action. */
		query_target_action =
			lttng_error_query_action_borrow_action_target(query, matching_trigger);
	}

	trigger_status = lttng_trigger_get_name(matching_trigger, &trigger_name);
	trigger_name = trigger_status == LTTNG_TRIGGER_STATUS_OK ? trigger_name : "(anonymous)";
	trigger_status = lttng_trigger_get_owner_uid(matching_trigger, &trigger_owner);
	LTTNG_ASSERT(trigger_status == LTTNG_TRIGGER_STATUS_OK);

	results = lttng_error_query_results_create();
	if (!results) {
		ret_code = LTTNG_ERR_NOMEM;
		goto end;
	}

	DBG("Running \"execute error query\" command: trigger name = '%s', trigger owner uid = %d, command creds uid = %d",
	    trigger_name,
	    (int) trigger_owner,
	    (int) lttng_credentials_get_uid(cmd_creds));

	/*
	 * Validate the trigger credentials against the command credentials.
	 * Only the root user can target a trigger with non-matching
	 * credentials.
	 */
	if (!lttng_credentials_is_equal_uid(lttng_trigger_get_credentials(matching_trigger),
					    cmd_creds)) {
		if (lttng_credentials_get_uid(cmd_creds) != 0) {
			ERR("Trigger credentials do not match the command credentials: trigger name = '%s', trigger owner uid = %d, command creds uid = %d",
			    trigger_name,
			    (int) trigger_owner,
			    (int) lttng_credentials_get_uid(cmd_creds));
			ret_code = LTTNG_ERR_INVALID_TRIGGER;
			goto end;
		}
	}

	switch (lttng_error_query_get_target_type(query)) {
	case LTTNG_ERROR_QUERY_TARGET_TYPE_TRIGGER:
		trigger_status = lttng_trigger_add_error_results(matching_trigger, results);

		switch (trigger_status) {
		case LTTNG_TRIGGER_STATUS_OK:
			break;
		default:
			ret_code = LTTNG_ERR_UNK;
			goto end;
		}

		break;
	case LTTNG_ERROR_QUERY_TARGET_TYPE_CONDITION:
	{
		trigger_status =
			lttng_trigger_condition_add_error_results(matching_trigger, results);

		switch (trigger_status) {
		case LTTNG_TRIGGER_STATUS_OK:
			break;
		default:
			ret_code = LTTNG_ERR_UNK;
			goto end;
		}

		break;
	}
	case LTTNG_ERROR_QUERY_TARGET_TYPE_ACTION:
	{
		const enum lttng_action_status action_status =
			lttng_action_add_error_query_results(query_target_action, results);

		switch (action_status) {
		case LTTNG_ACTION_STATUS_OK:
			break;
		default:
			ret_code = LTTNG_ERR_UNK;
			goto end;
		}

		break;
	}
	default:
		abort();
		break;
	}

	*_results = results;
	results = nullptr;
	ret_code = LTTNG_OK;
end:
	lttng_trigger_put(matching_trigger);
	lttng_error_query_results_destroy(results);
	return ret_code;
}

/*
 * Send relayd sockets from snapshot output to consumer. Ignore request if the
 * snapshot output is *not* set with a remote destination.
 *
 * Return LTTNG_OK on success or a LTTNG_ERR code.
 */
static enum lttng_error_code set_relayd_for_snapshot(struct consumer_output *output,
						     const ltt_session::locked_ref& session)
{
	enum lttng_error_code status = LTTNG_OK;
	LTTNG_OPTIONAL(uint64_t) current_chunk_id = {};
	const char *base_path;

	LTTNG_ASSERT(output);

	DBG2("Set relayd object from snapshot output");

	if (session->current_trace_chunk) {
		const lttng_trace_chunk_status chunk_status = lttng_trace_chunk_get_id(
			session->current_trace_chunk, &current_chunk_id.value);

		if (chunk_status == LTTNG_TRACE_CHUNK_STATUS_OK) {
			current_chunk_id.is_set = true;
		} else {
			ERR("Failed to get current trace chunk id");
			status = LTTNG_ERR_UNK;
			goto error;
		}
	}

	/* Ignore if snapshot consumer output is not network. */
	if (output->type != CONSUMER_DST_NET) {
		goto error;
	}

	/*
	 * The snapshot record URI base path overrides the session
	 * base path.
	 */
	if (output->dst.net.control.subdir[0] != '\0') {
		base_path = output->dst.net.control.subdir;
	} else {
		base_path = session->base_path;
	}

	/*
	 * For each consumer socket, create and send the relayd object of the
	 * snapshot output.
	 */
	for (auto *socket :
	     lttng::urcu::lfht_iteration_adapter<consumer_socket,
						 decltype(consumer_socket::node),
						 &consumer_socket::node>(*output->socks->ht)) {
		pthread_mutex_lock(socket->lock);
		status = send_consumer_relayd_sockets(
			session->id,
			output,
			socket,
			session->name,
			session->hostname,
			base_path,
			session->live_timer,
			current_chunk_id.is_set ? &current_chunk_id.value : nullptr,
			session->creation_time,
			session->name_contains_creation_time);
		pthread_mutex_unlock(socket->lock);
		if (status != LTTNG_OK) {
			goto error;
		}
	}

error:
	return status;
}

/*
 * Record a kernel snapshot.
 *
 * Return LTTNG_OK on success or a LTTNG_ERR code.
 */
static enum lttng_error_code record_kernel_snapshot(struct ltt_kernel_session *ksess,
						    const struct consumer_output *output,
						    uint64_t nb_packets_per_stream)
{
	enum lttng_error_code status;

	LTTNG_ASSERT(ksess);
	LTTNG_ASSERT(output);

	status = kernel_snapshot_record(ksess, output, nb_packets_per_stream);
	return status;
}

/*
 * Record a UST snapshot.
 *
 * Returns LTTNG_OK on success or a LTTNG_ERR error code.
 */
static enum lttng_error_code record_ust_snapshot(struct ltt_ust_session *usess,
						 const struct consumer_output *output,
						 uint64_t nb_packets_per_stream)
{
	enum lttng_error_code status;

	LTTNG_ASSERT(usess);
	LTTNG_ASSERT(output);

	status = ust_app_snapshot_record(usess, output, nb_packets_per_stream);
	return status;
}

static uint64_t get_session_size_one_more_packet_per_stream(const ltt_session::locked_ref& session,
							    uint64_t cur_nr_packets)
{
	uint64_t tot_size = 0;

	if (session->kernel_session) {
		struct ltt_kernel_session *ksess = session->kernel_session;

		for (auto chan : lttng::urcu::list_iteration_adapter<ltt_kernel_channel,
								     &ltt_kernel_channel::list>(
			     ksess->channel_list.head)) {
			if (cur_nr_packets >= chan->channel->attr.num_subbuf) {
				/*
				 * Don't take channel into account if we
				 * already grab all its packets.
				 */
				continue;
			}
			tot_size += chan->channel->attr.subbuf_size * chan->stream_count;
		}
	}

	if (session->ust_session) {
		const struct ltt_ust_session *usess = session->ust_session;

		tot_size += ust_app_get_size_one_more_packet_per_stream(usess, cur_nr_packets);
	}

	return tot_size;
}

/*
 * Calculate the number of packets we can grab from each stream that
 * fits within the overall snapshot max size.
 *
 * Returns -1 on error, 0 means infinite number of packets, else > 0 is
 * the number of packets per stream.
 *
 * TODO: this approach is not perfect: we consider the worse case
 * (packet filling the sub-buffers) as an upper bound, but we could do
 * better if we do this calculation while we actually grab the packet
 * content: we would know how much padding we don't actually store into
 * the file.
 *
 * This algorithm is currently bounded by the number of packets per
 * stream.
 *
 * Since we call this algorithm before actually grabbing the data, it's
 * an approximation: for instance, applications could appear/disappear
 * in between this call and actually grabbing data.
 */
static int64_t get_session_nb_packets_per_stream(const ltt_session::locked_ref& session,
						 uint64_t max_size)
{
	int64_t size_left;
	uint64_t cur_nb_packets = 0;

	if (!max_size) {
		return 0; /* Infinite */
	}

	size_left = max_size;
	for (;;) {
		uint64_t one_more_packet_tot_size;

		one_more_packet_tot_size =
			get_session_size_one_more_packet_per_stream(session, cur_nb_packets);
		if (!one_more_packet_tot_size) {
			/* We are already grabbing all packets. */
			break;
		}
		size_left -= one_more_packet_tot_size;
		if (size_left < 0) {
			break;
		}
		cur_nb_packets++;
	}
	if (!cur_nb_packets && size_left != max_size) {
		/* Not enough room to grab one packet of each stream, error. */
		return -1;
	}
	return cur_nb_packets;
}

static enum lttng_error_code snapshot_record(const ltt_session::locked_ref& session,
					     const struct snapshot_output *snapshot_output)
{
	int64_t nb_packets_per_stream;
	char snapshot_chunk_name[LTTNG_NAME_MAX];
	int ret;
	enum lttng_error_code ret_code = LTTNG_OK;
	struct lttng_trace_chunk *snapshot_trace_chunk;
	struct consumer_output *original_ust_consumer_output = nullptr;
	struct consumer_output *original_kernel_consumer_output = nullptr;
	struct consumer_output *snapshot_ust_consumer_output = nullptr;
	struct consumer_output *snapshot_kernel_consumer_output = nullptr;

	ret = snprintf(snapshot_chunk_name,
		       sizeof(snapshot_chunk_name),
		       "%s-%s-%" PRIu64,
		       snapshot_output->name,
		       snapshot_output->datetime,
		       snapshot_output->nb_snapshot);
	if (ret < 0 || ret >= sizeof(snapshot_chunk_name)) {
		ERR("Failed to format snapshot name");
		ret_code = LTTNG_ERR_INVALID;
		goto error;
	}
	DBG("Recording snapshot \"%s\" for session \"%s\" with chunk name \"%s\"",
	    snapshot_output->name,
	    session->name,
	    snapshot_chunk_name);
	if (!session->kernel_session && !session->ust_session) {
		ERR("Failed to record snapshot as no channels exist");
		ret_code = LTTNG_ERR_NO_CHANNEL;
		goto error;
	}

	if (session->kernel_session) {
		original_kernel_consumer_output = session->kernel_session->consumer;
		snapshot_kernel_consumer_output = consumer_copy_output(snapshot_output->consumer);
		strcpy(snapshot_kernel_consumer_output->chunk_path, snapshot_chunk_name);

		/* Copy the original domain subdir. */
		strcpy(snapshot_kernel_consumer_output->domain_subdir,
		       original_kernel_consumer_output->domain_subdir);

		ret = consumer_copy_sockets(snapshot_kernel_consumer_output,
					    original_kernel_consumer_output);
		if (ret < 0) {
			ERR("Failed to copy consumer sockets from snapshot output configuration");
			ret_code = LTTNG_ERR_NOMEM;
			goto error;
		}
		ret_code = set_relayd_for_snapshot(snapshot_kernel_consumer_output, session);
		if (ret_code != LTTNG_OK) {
			ERR("Failed to setup relay daemon for kernel tracer snapshot");
			goto error;
		}
		session->kernel_session->consumer = snapshot_kernel_consumer_output;
	}
	if (session->ust_session) {
		original_ust_consumer_output = session->ust_session->consumer;
		snapshot_ust_consumer_output = consumer_copy_output(snapshot_output->consumer);
		strcpy(snapshot_ust_consumer_output->chunk_path, snapshot_chunk_name);

		/* Copy the original domain subdir. */
		strcpy(snapshot_ust_consumer_output->domain_subdir,
		       original_ust_consumer_output->domain_subdir);

		ret = consumer_copy_sockets(snapshot_ust_consumer_output,
					    original_ust_consumer_output);
		if (ret < 0) {
			ERR("Failed to copy consumer sockets from snapshot output configuration");
			ret_code = LTTNG_ERR_NOMEM;
			goto error;
		}
		ret_code = set_relayd_for_snapshot(snapshot_ust_consumer_output, session);
		if (ret_code != LTTNG_OK) {
			ERR("Failed to setup relay daemon for userspace tracer snapshot");
			goto error;
		}
		session->ust_session->consumer = snapshot_ust_consumer_output;
	}

	snapshot_trace_chunk = session_create_new_trace_chunk(
		session,
		snapshot_kernel_consumer_output ?: snapshot_ust_consumer_output,
		consumer_output_get_base_path(snapshot_output->consumer),
		snapshot_chunk_name);
	if (!snapshot_trace_chunk) {
		ERR("Failed to create temporary trace chunk to record a snapshot of session \"%s\"",
		    session->name);
		ret_code = LTTNG_ERR_CREATE_DIR_FAIL;
		goto error;
	}
	LTTNG_ASSERT(!session->current_trace_chunk);
	ret = session_set_trace_chunk(session, snapshot_trace_chunk, nullptr);
	lttng_trace_chunk_put(snapshot_trace_chunk);
	snapshot_trace_chunk = nullptr;
	if (ret) {
		ERR("Failed to set temporary trace chunk to record a snapshot of session \"%s\"",
		    session->name);
		ret_code = LTTNG_ERR_CREATE_TRACE_CHUNK_FAIL_CONSUMER;
		goto error;
	}

	nb_packets_per_stream =
		get_session_nb_packets_per_stream(session, snapshot_output->max_size);
	if (nb_packets_per_stream < 0) {
		ret_code = LTTNG_ERR_MAX_SIZE_INVALID;
		goto error_close_trace_chunk;
	}

	if (session->kernel_session) {
		ret_code = record_kernel_snapshot(session->kernel_session,
						  snapshot_kernel_consumer_output,
						  nb_packets_per_stream);
		if (ret_code != LTTNG_OK) {
			goto error_close_trace_chunk;
		}
	}

	if (session->ust_session) {
		ret_code = record_ust_snapshot(
			session->ust_session, snapshot_ust_consumer_output, nb_packets_per_stream);
		if (ret_code != LTTNG_OK) {
			goto error_close_trace_chunk;
		}
	}

error_close_trace_chunk:
	if (session_set_trace_chunk(session, nullptr, &snapshot_trace_chunk)) {
		ERR("Failed to release the current trace chunk of session \"%s\"", session->name);
		ret_code = LTTNG_ERR_UNK;
	}

	if (session_close_trace_chunk(session,
				      snapshot_trace_chunk,
				      LTTNG_TRACE_CHUNK_COMMAND_TYPE_NO_OPERATION,
				      nullptr)) {
		/*
		 * Don't goto end; make sure the chunk is closed for the session
		 * to allow future snapshots.
		 */
		ERR("Failed to close snapshot trace chunk of session \"%s\"", session->name);
		ret_code = LTTNG_ERR_CLOSE_TRACE_CHUNK_FAIL_CONSUMER;
	}

	lttng_trace_chunk_put(snapshot_trace_chunk);
	snapshot_trace_chunk = nullptr;
error:
	if (original_ust_consumer_output) {
		session->ust_session->consumer = original_ust_consumer_output;
	}
	if (original_kernel_consumer_output) {
		session->kernel_session->consumer = original_kernel_consumer_output;
	}
	consumer_output_put(snapshot_ust_consumer_output);
	consumer_output_put(snapshot_kernel_consumer_output);
	return ret_code;
}

/*
 * Command LTTNG_SNAPSHOT_RECORD from lib lttng ctl.
 *
 * The wait parameter is ignored so this call always wait for the snapshot to
 * complete before returning.
 *
 * Return LTTNG_OK on success or else a LTTNG_ERR code.
 */
int cmd_snapshot_record(const ltt_session::locked_ref& session,
			const struct lttng_snapshot_output *output)
{
	enum lttng_error_code cmd_ret = LTTNG_OK;
	int ret;
	unsigned int snapshot_success = 0;
	char datetime[16];
	struct snapshot_output *tmp_output = nullptr;

	LTTNG_ASSERT(output);

	DBG("Cmd snapshot record for session %s", session->name);

	/* Get the datetime for the snapshot output directory. */
	ret = utils_get_current_time_str("%Y%m%d-%H%M%S", datetime, sizeof(datetime));
	if (!ret) {
		cmd_ret = LTTNG_ERR_INVALID;
		goto error;
	}

	/*
	 * Permission denied to create an output if the session is not
	 * set in no output mode.
	 */
	if (session->output_traces) {
		cmd_ret = LTTNG_ERR_NOT_SNAPSHOT_SESSION;
		goto error;
	}

	/* The session needs to be started at least once. */
	if (!session->has_been_started) {
		cmd_ret = LTTNG_ERR_START_SESSION_ONCE;
		goto error;
	}

	/* Use temporary output for the session. */
	if (*output->ctrl_url != '\0') {
		tmp_output = snapshot_output_alloc();
		if (!tmp_output) {
			cmd_ret = LTTNG_ERR_NOMEM;
			goto error;
		}

		ret = snapshot_output_init(session,
					   output->max_size,
					   output->name,
					   output->ctrl_url,
					   output->data_url,
					   session->consumer,
					   tmp_output,
					   nullptr);
		if (ret < 0) {
			if (ret == -ENOMEM) {
				cmd_ret = LTTNG_ERR_NOMEM;
			} else {
				cmd_ret = LTTNG_ERR_INVALID;
			}
			goto error;
		}
		/* Use the global session count for the temporary snapshot. */
		tmp_output->nb_snapshot = session->snapshot.nb_snapshot;

		/* Use the global datetime */
		memcpy(tmp_output->datetime, datetime, sizeof(datetime));
		cmd_ret = snapshot_record(session, tmp_output);
		if (cmd_ret != LTTNG_OK) {
			goto error;
		}
		snapshot_success = 1;
	} else {
		for (auto *sout :
		     lttng::urcu::lfht_iteration_adapter<snapshot_output,
							 decltype(snapshot_output::node),
							 &snapshot_output::node>(
			     *session->snapshot.output_ht->ht)) {
			struct snapshot_output output_copy;

			/*
			 * Make a local copy of the output and override output
			 * parameters with those provided as part of the
			 * command.
			 */
			memcpy(&output_copy, sout, sizeof(output_copy));

			if (output->max_size != (uint64_t) -1ULL) {
				output_copy.max_size = output->max_size;
			}

			output_copy.nb_snapshot = session->snapshot.nb_snapshot;
			memcpy(output_copy.datetime, datetime, sizeof(datetime));

			/* Use temporary name. */
			if (*output->name != '\0') {
				if (lttng_strncpy(output_copy.name,
						  output->name,
						  sizeof(output_copy.name))) {
					cmd_ret = LTTNG_ERR_INVALID;
					goto error;
				}
			}

			cmd_ret = snapshot_record(session, &output_copy);
			if (cmd_ret != LTTNG_OK) {
				goto error;
			}

			snapshot_success = 1;
		}
	}

	if (snapshot_success) {
		session->snapshot.nb_snapshot++;
	} else {
		cmd_ret = LTTNG_ERR_SNAPSHOT_FAIL;
	}

error:
	if (tmp_output) {
		snapshot_output_destroy(tmp_output);
	}

	return cmd_ret;
}

/*
 * Command LTTNG_SET_SESSION_SHM_PATH processed by the client thread.
 */
int cmd_set_session_shm_path(const ltt_session::locked_ref& session, const char *shm_path)
{
	/*
	 * Can only set shm path before session is started.
	 */
	if (session->has_been_started) {
		return LTTNG_ERR_SESSION_STARTED;
	}

	/* Report an error if shm_path is too long or not null-terminated. */
	const auto copy_ret = lttng_strncpy(session->shm_path, shm_path, sizeof(session->shm_path));
	return copy_ret == 0 ? LTTNG_OK : LTTNG_ERR_INVALID;
}

/*
 * Command LTTNG_ROTATE_SESSION from the lttng-ctl library.
 *
 * Ask the consumer to rotate the session output directory.
 * The session lock must be held.
 *
 * Returns LTTNG_OK on success or else a negative LTTng error code.
 */
int cmd_rotate_session(const ltt_session::locked_ref& session,
		       struct lttng_rotate_session_return *rotate_return,
		       bool quiet_rotation,
		       enum lttng_trace_chunk_command_type command)
{
	int ret;
	uint64_t ongoing_rotation_chunk_id;
	enum lttng_error_code cmd_ret = LTTNG_OK;
	struct lttng_trace_chunk *chunk_being_archived = nullptr;
	struct lttng_trace_chunk *new_trace_chunk = nullptr;
	enum lttng_trace_chunk_status chunk_status;
	bool failed_to_rotate = false;
	enum lttng_error_code rotation_fail_code = LTTNG_OK;

	if (!session->has_been_started) {
		cmd_ret = LTTNG_ERR_START_SESSION_ONCE;
		goto end;
	}

	/*
	 * Explicit rotation is not supported for live sessions.
	 * However, live sessions can perform a quiet rotation on
	 * destroy.
	 * Rotation is not supported for snapshot traces (no output).
	 */
	if ((!quiet_rotation && session->live_timer) || !session->output_traces) {
		cmd_ret = LTTNG_ERR_ROTATION_NOT_AVAILABLE;
		goto end;
	}

	/* Unsupported feature in lttng-relayd before 2.11. */
	if (!quiet_rotation && session->consumer->type == CONSUMER_DST_NET &&
	    (session->consumer->relay_major_version == 2 &&
	     session->consumer->relay_minor_version < 11)) {
		cmd_ret = LTTNG_ERR_ROTATION_NOT_AVAILABLE_RELAY;
		goto end;
	}

	/* Unsupported feature in lttng-modules before 2.8 (lack of sequence number). */
	if (session->kernel_session && !kernel_supports_ring_buffer_packet_sequence_number()) {
		cmd_ret = LTTNG_ERR_ROTATION_NOT_AVAILABLE_KERNEL;
		goto end;
	}

	if (session->rotation_state == LTTNG_ROTATION_STATE_ONGOING) {
		DBG("Refusing to launch a rotation; a rotation is already in progress for session %s",
		    session->name);
		cmd_ret = LTTNG_ERR_ROTATION_PENDING;
		goto end;
	}

	/*
	 * After a stop, we only allow one rotation to occur, the other ones are
	 * useless until a new start.
	 */
	if (session->rotated_after_last_stop) {
		DBG("Session \"%s\" was already rotated after stop, refusing rotation",
		    session->name);
		cmd_ret = LTTNG_ERR_ROTATION_MULTIPLE_AFTER_STOP;
		goto end;
	}

	/*
	 * After a stop followed by a clear, disallow following rotations a they would
	 * generate empty chunks.
	 */
	if (session->cleared_after_last_stop) {
		DBG("Session \"%s\" was already cleared after stop, refusing rotation",
		    session->name);
		cmd_ret = LTTNG_ERR_ROTATION_AFTER_STOP_CLEAR;
		goto end;
	}

	if (session->active) {
		new_trace_chunk =
			session_create_new_trace_chunk(session, nullptr, nullptr, nullptr);
		if (!new_trace_chunk) {
			cmd_ret = LTTNG_ERR_CREATE_DIR_FAIL;
			goto error;
		}
	}

	/*
	 * The current trace chunk becomes the chunk being archived.
	 *
	 * After this point, "chunk_being_archived" must absolutely
	 * be closed on the consumer(s), otherwise it will never be
	 * cleaned-up, which will result in a leak.
	 */
	ret = session_set_trace_chunk(session, new_trace_chunk, &chunk_being_archived);
	if (ret) {
		cmd_ret = LTTNG_ERR_CREATE_TRACE_CHUNK_FAIL_CONSUMER;
		goto error;
	}

	if (session->kernel_session) {
		cmd_ret = kernel_rotate_session(session);
		if (cmd_ret != LTTNG_OK) {
			failed_to_rotate = true;
			rotation_fail_code = cmd_ret;
		}
	}
	if (session->ust_session) {
		cmd_ret = ust_app_rotate_session(session);
		if (cmd_ret != LTTNG_OK) {
			failed_to_rotate = true;
			rotation_fail_code = cmd_ret;
		}
	}

	if (!session->active) {
		session->rotated_after_last_stop = true;
	}

	if (!chunk_being_archived) {
		DBG("Rotating session \"%s\" from a \"NULL\" trace chunk to a new trace chunk, skipping completion check",
		    session->name);
		if (failed_to_rotate) {
			cmd_ret = rotation_fail_code;
			goto error;
		}
		cmd_ret = LTTNG_OK;
		goto end;
	}

	session->rotation_state = LTTNG_ROTATION_STATE_ONGOING;
	chunk_status = lttng_trace_chunk_get_id(chunk_being_archived, &ongoing_rotation_chunk_id);
	LTTNG_ASSERT(chunk_status == LTTNG_TRACE_CHUNK_STATUS_OK);

	ret = session_close_trace_chunk(
		session, chunk_being_archived, command, session->last_chunk_path);
	if (ret) {
		cmd_ret = LTTNG_ERR_CLOSE_TRACE_CHUNK_FAIL_CONSUMER;
		goto error;
	}

	if (failed_to_rotate) {
		cmd_ret = rotation_fail_code;
		goto error;
	}

	session->quiet_rotation = quiet_rotation;
	ret = timer_session_rotation_pending_check_start(session, DEFAULT_ROTATE_PENDING_TIMER);
	if (ret) {
		cmd_ret = LTTNG_ERR_UNK;
		goto error;
	}

	if (rotate_return) {
		rotate_return->rotation_id = ongoing_rotation_chunk_id;
	}

	session->chunk_being_archived = chunk_being_archived;
	chunk_being_archived = nullptr;
	if (!quiet_rotation) {
		ret = notification_thread_command_session_rotation_ongoing(
			the_notification_thread_handle, session->id, ongoing_rotation_chunk_id);
		if (ret != LTTNG_OK) {
			ERR("Failed to notify notification thread that a session rotation is ongoing for session %s",
			    session->name);
			cmd_ret = (lttng_error_code) ret;
		}
	}

	DBG("Cmd rotate session %s, archive_id %" PRIu64 " sent",
	    session->name,
	    ongoing_rotation_chunk_id);
end:
	lttng_trace_chunk_put(new_trace_chunk);
	lttng_trace_chunk_put(chunk_being_archived);
	ret = (cmd_ret == LTTNG_OK) ? cmd_ret : -((int) cmd_ret);
	return ret;
error:
	if (session_reset_rotation_state(session, LTTNG_ROTATION_STATE_ERROR)) {
		ERR("Failed to reset rotation state of session \"%s\"", session->name);
	}
	goto end;
}

/*
 * Command LTTNG_ROTATION_GET_INFO from the lttng-ctl library.
 *
 * Check if the session has finished its rotation.
 *
 * Return LTTNG_OK on success or else an LTTNG_ERR code.
 */
int cmd_rotate_get_info(const ltt_session::locked_ref& session,
			struct lttng_rotation_get_info_return *info_return,
			uint64_t rotation_id)
{
	enum lttng_error_code cmd_ret = LTTNG_OK;
	enum lttng_rotation_state rotation_state;

	DBG("Cmd rotate_get_info session %s, rotation id %" PRIu64,
	    session->name,
	    session->most_recent_chunk_id.value);

	if (session->chunk_being_archived) {
		enum lttng_trace_chunk_status chunk_status;
		uint64_t chunk_id;

		chunk_status = lttng_trace_chunk_get_id(session->chunk_being_archived, &chunk_id);
		LTTNG_ASSERT(chunk_status == LTTNG_TRACE_CHUNK_STATUS_OK);

		rotation_state = rotation_id == chunk_id ? LTTNG_ROTATION_STATE_ONGOING :
							   LTTNG_ROTATION_STATE_EXPIRED;
	} else {
		if (session->last_archived_chunk_id.is_set &&
		    rotation_id != session->last_archived_chunk_id.value) {
			rotation_state = LTTNG_ROTATION_STATE_EXPIRED;
		} else {
			rotation_state = session->rotation_state;
		}
	}

	switch (rotation_state) {
	case LTTNG_ROTATION_STATE_NO_ROTATION:
		DBG("Reporting that no rotation has occurred within the lifetime of session \"%s\"",
		    session->name);
		goto end;
	case LTTNG_ROTATION_STATE_EXPIRED:
		DBG("Reporting that the rotation state of rotation id %" PRIu64
		    " of session \"%s\" has expired",
		    rotation_id,
		    session->name);
		break;
	case LTTNG_ROTATION_STATE_ONGOING:
		DBG("Reporting that rotation id %" PRIu64 " of session \"%s\" is still pending",
		    rotation_id,
		    session->name);
		break;
	case LTTNG_ROTATION_STATE_COMPLETED:
	{
		int fmt_ret;
		char *chunk_path;
		char *current_tracing_path_reply;
		size_t current_tracing_path_reply_len;

		DBG("Reporting that rotation id %" PRIu64 " of session \"%s\" is completed",
		    rotation_id,
		    session->name);

		switch (session_get_consumer_destination_type(session)) {
		case CONSUMER_DST_LOCAL:
			current_tracing_path_reply = info_return->location.local.absolute_path;
			current_tracing_path_reply_len =
				sizeof(info_return->location.local.absolute_path);
			info_return->location_type =
				(int8_t) LTTNG_TRACE_ARCHIVE_LOCATION_TYPE_LOCAL;
			fmt_ret = asprintf(&chunk_path,
					   "%s/" DEFAULT_ARCHIVED_TRACE_CHUNKS_DIRECTORY "/%s",
					   session_get_base_path(session),
					   session->last_archived_chunk_name);
			if (fmt_ret == -1) {
				PERROR("Failed to format the path of the last archived trace chunk");
				info_return->status = LTTNG_ROTATION_STATUS_ERROR;
				cmd_ret = LTTNG_ERR_UNK;
				goto end;
			}
			break;
		case CONSUMER_DST_NET:
		{
			uint16_t ctrl_port, data_port;

			current_tracing_path_reply = info_return->location.relay.relative_path;
			current_tracing_path_reply_len =
				sizeof(info_return->location.relay.relative_path);
			/* Currently the only supported relay protocol. */
			info_return->location.relay.protocol =
				(int8_t) LTTNG_TRACE_ARCHIVE_LOCATION_RELAY_PROTOCOL_TYPE_TCP;

			fmt_ret = lttng_strncpy(info_return->location.relay.host,
						session_get_net_consumer_hostname(session),
						sizeof(info_return->location.relay.host));
			if (fmt_ret) {
				ERR("Failed to copy host name to rotate_get_info reply");
				info_return->status = LTTNG_ROTATION_STATUS_ERROR;
				cmd_ret = LTTNG_ERR_SET_URL;
				goto end;
			}

			session_get_net_consumer_ports(session, &ctrl_port, &data_port);
			info_return->location.relay.ports.control = ctrl_port;
			info_return->location.relay.ports.data = data_port;
			info_return->location_type =
				(int8_t) LTTNG_TRACE_ARCHIVE_LOCATION_TYPE_RELAY;
			chunk_path = strdup(session->last_chunk_path);
			if (!chunk_path) {
				ERR("Failed to allocate the path of the last archived trace chunk");
				info_return->status = LTTNG_ROTATION_STATUS_ERROR;
				cmd_ret = LTTNG_ERR_UNK;
				goto end;
			}
			break;
		}
		default:
			abort();
		}

		fmt_ret = lttng_strncpy(
			current_tracing_path_reply, chunk_path, current_tracing_path_reply_len);
		free(chunk_path);
		if (fmt_ret) {
			ERR("Failed to copy path of the last archived trace chunk to rotate_get_info reply");
			info_return->status = LTTNG_ROTATION_STATUS_ERROR;
			cmd_ret = LTTNG_ERR_UNK;
			goto end;
		}

		break;
	}
	case LTTNG_ROTATION_STATE_ERROR:
		DBG("Reporting that an error occurred during rotation %" PRIu64
		    " of session \"%s\"",
		    rotation_id,
		    session->name);
		break;
	default:
		abort();
	}

	cmd_ret = LTTNG_OK;
end:
	info_return->status = (int32_t) rotation_state;
	return cmd_ret;
}

/*
 * Command LTTNG_ROTATION_SET_SCHEDULE from the lttng-ctl library.
 *
 * Configure the automatic rotation parameters.
 * 'activate' to true means activate the rotation schedule type with 'new_value'.
 * 'activate' to false means deactivate the rotation schedule and validate that
 * 'new_value' has the same value as the currently active value.
 *
 * Return LTTNG_OK on success or else a positive LTTNG_ERR code.
 */
int cmd_rotation_set_schedule(const ltt_session::locked_ref& session,
			      bool activate,
			      enum lttng_rotation_schedule_type schedule_type,
			      uint64_t new_value)
{
	int ret;
	uint64_t *parameter_value;

	DBG("Cmd rotate set schedule session %s", session->name);

	if (session->live_timer || !session->output_traces) {
		DBG("Failing ROTATION_SET_SCHEDULE command as the rotation feature is not available for this session");
		ret = LTTNG_ERR_ROTATION_NOT_AVAILABLE;
		goto end;
	}

	switch (schedule_type) {
	case LTTNG_ROTATION_SCHEDULE_TYPE_SIZE_THRESHOLD:
		parameter_value = &session->rotate_size;
		break;
	case LTTNG_ROTATION_SCHEDULE_TYPE_PERIODIC:
		parameter_value = &session->rotate_timer_period;
		if (new_value >= UINT_MAX) {
			DBG("Failing ROTATION_SET_SCHEDULE command as the value requested for a periodic rotation schedule is invalid: %" PRIu64
			    " > %u (UINT_MAX)",
			    new_value,
			    UINT_MAX);
			ret = LTTNG_ERR_INVALID;
			goto end;
		}
		break;
	default:
		WARN("Failing ROTATION_SET_SCHEDULE command on unknown schedule type");
		ret = LTTNG_ERR_INVALID;
		goto end;
	}

	/* Improper use of the API. */
	if (new_value == -1ULL) {
		WARN("Failing ROTATION_SET_SCHEDULE command as the value requested is -1");
		ret = LTTNG_ERR_INVALID;
		goto end;
	}

	/*
	 * As indicated in struct ltt_session's comments, a value of == 0 means
	 * this schedule rotation type is not in use.
	 *
	 * Reject the command if we were asked to activate a schedule that was
	 * already active.
	 */
	if (activate && *parameter_value != 0) {
		DBG("Failing ROTATION_SET_SCHEDULE (activate) command as the schedule is already active");
		ret = LTTNG_ERR_ROTATION_SCHEDULE_SET;
		goto end;
	}

	/*
	 * Reject the command if we were asked to deactivate a schedule that was
	 * not active.
	 */
	if (!activate && *parameter_value == 0) {
		DBG("Failing ROTATION_SET_SCHEDULE (deactivate) command as the schedule is already inactive");
		ret = LTTNG_ERR_ROTATION_SCHEDULE_NOT_SET;
		goto end;
	}

	/*
	 * Reject the command if we were asked to deactivate a schedule that
	 * doesn't exist.
	 */
	if (!activate && *parameter_value != new_value) {
		DBG("Failing ROTATION_SET_SCHEDULE (deactivate) command as an inexistant schedule was provided");
		ret = LTTNG_ERR_ROTATION_SCHEDULE_NOT_SET;
		goto end;
	}

	*parameter_value = activate ? new_value : 0;

	switch (schedule_type) {
	case LTTNG_ROTATION_SCHEDULE_TYPE_PERIODIC:
		if (activate && session->active) {
			/*
			 * Only start the timer if the session is active,
			 * otherwise it will be started when the session starts.
			 */
			ret = timer_session_rotation_schedule_timer_start(session, new_value);
			if (ret) {
				ERR("Failed to enable session rotation timer in ROTATION_SET_SCHEDULE command");
				ret = LTTNG_ERR_UNK;
				goto end;
			}
		} else {
			ret = timer_session_rotation_schedule_timer_stop(session);
			if (ret) {
				ERR("Failed to disable session rotation timer in ROTATION_SET_SCHEDULE command");
				ret = LTTNG_ERR_UNK;
				goto end;
			}
		}
		break;
	case LTTNG_ROTATION_SCHEDULE_TYPE_SIZE_THRESHOLD:
		if (activate) {
			try {
				the_rotation_thread_handle->subscribe_session_consumed_size_rotation(
					*session, new_value);
			} catch (const std::exception& e) {
				ERR("Failed to enable consumed-size notification in ROTATION_SET_SCHEDULE command: %s",
				    e.what());
				ret = LTTNG_ERR_UNK;
				goto end;
			}
		} else {
			try {
				the_rotation_thread_handle
					->unsubscribe_session_consumed_size_rotation(*session);
			} catch (const std::exception& e) {
				ERR("Failed to disable consumed-size notification in ROTATION_SET_SCHEDULE command: %s",
				    e.what());
				ret = LTTNG_ERR_UNK;
				goto end;
			}
		}
		break;
	default:
		/* Would have been caught before. */
		abort();
	}

	ret = LTTNG_OK;

	goto end;

end:
	return ret;
}

/* Wait for a given path to be removed before continuing. */
static enum lttng_error_code wait_on_path(void *path_data)
{
	const char *shm_path = (const char *) path_data;

	DBG("Waiting for the shm path at %s to be removed before completing session destruction",
	    shm_path);
	while (true) {
		int ret;
		struct stat st;

		ret = stat(shm_path, &st);
		if (ret) {
			if (errno != ENOENT) {
				PERROR("stat() returned an error while checking for the existence of the shm path");
			} else {
				DBG("shm path no longer exists, completing the destruction of session");
			}
			break;
		} else {
			if (!S_ISDIR(st.st_mode)) {
				ERR("The type of shm path %s returned by stat() is not a directory; aborting the wait for shm path removal",
				    shm_path);
				break;
			}
		}
		usleep(SESSION_DESTROY_SHM_PATH_CHECK_DELAY_US);
	}
	return LTTNG_OK;
}

/*
 * Returns a pointer to a handler to run on completion of a command.
 * Returns NULL if no handler has to be run for the last command executed.
 */
const struct cmd_completion_handler *cmd_pop_completion_handler()
{
	struct cmd_completion_handler *handler = current_completion_handler;

	current_completion_handler = nullptr;
	return handler;
}

/*
 * Init command subsystem.
 */
void cmd_init()
{
	/*
	 * Set network sequence index to 1 for streams to match a relayd
	 * socket on the consumer side.
	 */
	pthread_mutex_lock(&relayd_net_seq_idx_lock);
	relayd_net_seq_idx = 1;
	pthread_mutex_unlock(&relayd_net_seq_idx_lock);

	DBG("Command subsystem initialized");
}
