/*
 * SPDX-FileCopyrightText: 2011 EfficiOS Inc.
 * SPDX-FileCopyrightText: 2011 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
 * SPDX-FileCopyrightText: 2017 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "common/unix.hpp"

#include <cstdint>
#include <exception>
#define _LGPL_SOURCE
#include "ust-consumer.hpp"

#include <common/common.hpp>
#include <common/compat/endian.hpp>
#include <common/consumer/consumer-channel.hpp>
#include <common/consumer/consumer-metadata-cache.hpp>
#include <common/consumer/consumer-stream.hpp>
#include <common/consumer/consumer-timer.hpp>
#include <common/consumer/consumer.hpp>
#include <common/consumer/watchdog-timer-task.hpp>
#include <common/exception.hpp>
#include <common/index/index.hpp>
#include <common/make-unique-wrapper.hpp>
#include <common/optional.hpp>
#include <common/pthread-lock.hpp>
#include <common/relayd/relayd.hpp>
#include <common/scope-exit.hpp>
#include <common/sessiond-comm/sessiond-comm.hpp>
#include <common/shm.hpp>
#include <common/urcu.hpp>
#include <common/ust-consumer/ust-consumer.hpp>
#include <common/utils.hpp>

#include <lttng/ust-ctl.h>
#include <lttng/ust-sigbus.h>

#include <algorithm>
#include <bin/lttng-consumerd/health-consumerd.hpp>
#include <chrono>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <pthread.h>
#include <set>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <urcu/list.h>

#define INT_MAX_STR_LEN 12 /* includes \0 */

extern struct lttng_consumer_global_data the_consumer_data;
extern int consumer_poll_timeout;

LTTNG_EXPORT DEFINE_LTTNG_UST_SIGBUS_STATE();

/*
 * Add channel to internal consumer state.
 *
 * Returns 0 on success or else a negative value.
 */
static int add_channel(struct lttng_consumer_channel *channel,
		       struct lttng_consumer_local_data *ctx)
{
	int ret = 0;

	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(ctx);

	if (ctx->on_recv_channel != nullptr) {
		ret = ctx->on_recv_channel(channel);
		if (ret == 0) {
			ret = consumer_add_channel(channel, ctx);
		} else if (ret < 0) {
			/* Most likely an ENOMEM. */
			lttng_consumer_send_error(ctx->consumer_error_socket,
						  LTTCOMM_CONSUMERD_OUTFD_ERROR);
			goto error;
		}
	} else {
		ret = consumer_add_channel(channel, ctx);
	}

	DBG("UST consumer channel added (key: %" PRIu64 ")", channel->key);

error:
	return ret;
}

/*
 * Allocate and return a consumer stream object. If _alloc_ret is not NULL, the
 * error value if applicable is set in it else it is kept untouched.
 *
 * Return NULL on error else the newly allocated stream object.
 */
static struct lttng_consumer_stream *allocate_stream(int cpu,
						     int key,
						     struct lttng_consumer_channel *channel,
						     struct lttng_consumer_local_data *ctx,
						     int *_alloc_ret)
{
	int alloc_ret;
	struct lttng_consumer_stream *stream = nullptr;

	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(ctx);

	try {
		stream = consumer_stream_create(channel,
						channel->key,
						key,
						channel->name,
						channel->relayd_id,
						channel->session_id,
						channel->trace_chunk,
						cpu,
						&alloc_ret,
						channel->type,
						channel->monitor);
	} catch (const std::bad_alloc&) {
		LTTNG_ASSERT(!stream);
	}

	if (stream == nullptr) {
		switch (alloc_ret) {
		case -ENOENT:
			/*
			 * We could not find the channel. Can happen if cpu hotplug
			 * happens while tearing down.
			 */
			DBG3("Could not find channel");
			break;
		case -EINVAL:
		default:
			lttng_consumer_send_error(ctx->consumer_error_socket,
						  LTTCOMM_CONSUMERD_OUTFD_ERROR);
			break;
		}
		goto error;
	}

	consumer_stream_update_channel_attributes(stream, channel);

error:
	if (_alloc_ret) {
		*_alloc_ret = alloc_ret;
	}
	return stream;
}

/*
 * Send the given stream pointer to the corresponding thread.
 *
 * Returns 0 on success else a negative value.
 */
static int send_stream_to_thread(struct lttng_consumer_stream *stream,
				 struct lttng_consumer_local_data *ctx)
{
	int ret;
	struct lttng_pipe *stream_pipe;

	/* Get the right pipe where the stream will be sent. */
	if (stream->metadata_flag) {
		consumer_add_metadata_stream(stream);
		stream_pipe = ctx->consumer_metadata_pipe;
	} else {
		consumer_add_data_stream(stream);
		stream_pipe = ctx->consumer_data_pipe;
	}

	/*
	 * From this point on, the stream's ownership has been moved away from
	 * the channel and it becomes globally visible. Hence, remove it from
	 * the local stream list to prevent the stream from being both local and
	 * global.
	 */
	stream->globally_visible = 1;
	ASSERT_LOCKED(stream->chan->lock);
	cds_list_del_init(&stream->send_node);

	ret = lttng_pipe_write(stream_pipe, &stream, sizeof(stream)); /* NOLINT sizeof used on a
									 pointer. */
	if (ret < 0) {
		ERR("Consumer write %s stream to pipe %d",
		    stream->metadata_flag ? "metadata" : "data",
		    lttng_pipe_get_writefd(stream_pipe));
		if (stream->metadata_flag) {
			consumer_del_stream_for_metadata(stream);
		} else {
			consumer_del_stream_for_data(stream);
		}
		goto error;
	}

error:
	return ret;
}

static int get_stream_shm_path(char *stream_shm_path, const char *shm_path, int cpu)
{
	char cpu_nr[INT_MAX_STR_LEN]; /* int max len */
	int ret;

	strncpy(stream_shm_path, shm_path, PATH_MAX);
	stream_shm_path[PATH_MAX - 1] = '\0';
	ret = snprintf(cpu_nr, INT_MAX_STR_LEN, "%i", cpu);
	if (ret < 0) {
		PERROR("snprintf");
		goto end;
	}
	strncat(stream_shm_path, cpu_nr, PATH_MAX - strlen(stream_shm_path) - 1);
	ret = 0;
end:
	return ret;
}

/*
 * Create streams for the given channel using liblttng-ust-ctl.
 * The channel lock must be acquired by the caller.
 *
 * Return 0 on success else a negative value.
 */
static int create_ust_streams(struct lttng_consumer_channel *channel,
			      struct lttng_consumer_local_data *ctx)
{
	int ret, cpu = 0;
	struct lttng_ust_ctl_consumer_stream *ustream;
	struct lttng_consumer_stream *stream;
	pthread_mutex_t *current_stream_lock = nullptr;

	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(ctx);

	/*
	 * While a stream is available from ustctl. When NULL is returned, we've
	 * reached the end of the possible stream for the channel.
	 */
	while ((ustream = lttng_ust_ctl_create_stream(channel->uchan, cpu))) {
		int wait_fd;
		int ust_metadata_pipe[2];

		health_code_update();

		if (channel->type == CONSUMER_CHANNEL_TYPE_METADATA && channel->monitor) {
			ret = utils_create_pipe_cloexec_nonblock(ust_metadata_pipe);
			if (ret < 0) {
				ERR("Create ust metadata poll pipe");
				goto error;
			}
			wait_fd = ust_metadata_pipe[0];
		} else {
			wait_fd = lttng_ust_ctl_stream_get_wait_fd(ustream);
		}

		/* Allocate consumer stream object. */
		stream = allocate_stream(cpu, wait_fd, channel, ctx, &ret);
		if (!stream) {
			goto error_alloc;
		}
		stream->ustream = ustream;
		/*
		 * Store it so we can save multiple function calls afterwards since
		 * this value is used heavily in the stream threads. This is UST
		 * specific so this is why it's done after allocation.
		 */
		stream->wait_fd = wait_fd;

		/*
		 * Increment channel refcount since the channel reference has now been
		 * assigned in the allocation process above.
		 */
		if (stream->chan->monitor) {
			uatomic_inc(&stream->chan->refcount);
		}

		pthread_mutex_lock(&stream->lock);
		current_stream_lock = &stream->lock;
		/*
		 * Order is important this is why a list is used. On error, the caller
		 * should clean this list.
		 */
		cds_list_add_tail(&stream->send_node, &channel->streams.head);

		ret = lttng_ust_ctl_get_max_subbuf_size(stream->ustream, &stream->max_sb_size);
		if (ret < 0) {
			ERR("lttng_ust_ctl_get_max_subbuf_size failed for stream %s", stream->name);
			goto error;
		}

		/* Do actions once stream has been received. */
		if (ctx->on_recv_stream) {
			ret = ctx->on_recv_stream(stream);
			if (ret < 0) {
				goto error;
			}
		}

		DBG("UST consumer add stream %s (key: %" PRIu64 ") with relayd id %" PRIu64,
		    stream->name,
		    stream->key,
		    stream->relayd_stream_id);

		/* Set next CPU stream. */
		channel->streams.count = ++cpu;

		/* Keep stream reference when creating metadata. */
		if (channel->type == CONSUMER_CHANNEL_TYPE_METADATA) {
			channel->metadata_stream = stream;
			if (channel->monitor) {
				/* Set metadata poll pipe if we created one */
				memcpy(stream->ust_metadata_poll_pipe,
				       ust_metadata_pipe,
				       sizeof(ust_metadata_pipe));
			}
		}
		pthread_mutex_unlock(&stream->lock);
		current_stream_lock = nullptr;
	}

	return 0;

error:
error_alloc:
	if (current_stream_lock) {
		pthread_mutex_unlock(current_stream_lock);
	}
	return ret;
}

static int open_ust_stream_fd(struct lttng_consumer_channel *channel,
			      int cpu,
			      const struct lttng_credentials *session_credentials)
{
	char shm_path[PATH_MAX];
	int ret;

	if (!channel->shm_path[0]) {
		return shm_create_anonymous("ust-consumer");
	}
	ret = get_stream_shm_path(shm_path, channel->shm_path, cpu);
	if (ret) {
		goto error_shm_path;
	}
	return run_as_open(shm_path,
			   O_RDWR | O_CREAT | O_EXCL,
			   S_IRUSR | S_IWUSR,
			   lttng_credentials_get_uid(session_credentials),
			   lttng_credentials_get_gid(session_credentials));

error_shm_path:
	return -1;
}

/*
 * Create an UST channel with the given attributes and send it to the session
 * daemon using the ust ctl API.
 *
 * Return 0 on success or else a negative value.
 */
static int create_ust_channel(struct lttng_consumer_channel *channel,
			      struct lttng_ust_ctl_consumer_channel_attr *attr,
			      struct lttng_ust_ctl_consumer_channel **ust_chanp)
{
	int ret, nr_stream_fds, i, j;
	int *stream_fds;
	struct lttng_ust_ctl_consumer_channel *ust_channel;

	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(attr);
	LTTNG_ASSERT(ust_chanp);
	LTTNG_ASSERT(channel->buffer_credentials.is_set);

	DBG3("Creating channel to ustctl with attr: [overwrite: %d, "
	     "subbuf_size: %" PRIu64 ", num_subbuf: %" PRIu64 ", "
	     "switch_timer_interval: %u, read_timer_interval: %u, "
	     "output: %d, type: %d",
	     attr->overwrite,
	     attr->subbuf_size,
	     attr->num_subbuf,
	     attr->switch_timer_interval,
	     attr->read_timer_interval,
	     attr->output,
	     attr->type);

	switch (channel->type) {
	case CONSUMER_CHANNEL_TYPE_METADATA:
		/* Fallthrough */
	case CONSUMER_CHANNEL_TYPE_DATA_PER_CHANNEL:
		nr_stream_fds = 1;
		break;
	case CONSUMER_CHANNEL_TYPE_DATA_PER_CPU:
		nr_stream_fds = lttng_ust_ctl_get_nr_stream_per_channel();
		break;
	default:
		ERR("Invalid channel type");
		ret = -1;
		goto error_channel_type;
	}

	stream_fds = calloc<int>(nr_stream_fds);
	if (!stream_fds) {
		ret = -1;
		goto error_alloc;
	}
	for (i = 0; i < nr_stream_fds; i++) {
		stream_fds[i] = open_ust_stream_fd(channel, i, &channel->buffer_credentials.value);
		if (stream_fds[i] < 0) {
			ret = -1;
			goto error_open;
		}
	}
	ust_channel = lttng_ust_ctl_create_channel(attr, stream_fds, nr_stream_fds);
	if (!ust_channel) {
		ret = -1;
		goto error_create;
	}
	channel->nr_stream_fds = nr_stream_fds;
	channel->stream_fds = stream_fds;
	*ust_chanp = ust_channel;

	return 0;

error_create:
error_open:
	for (j = i - 1; j >= 0; j--) {
		int closeret;

		closeret = close(stream_fds[j]);
		if (closeret) {
			PERROR("close");
		}
		if (channel->shm_path[0]) {
			char shm_path[PATH_MAX];

			closeret = get_stream_shm_path(shm_path, channel->shm_path, j);
			if (closeret) {
				ERR("Cannot get stream shm path");
			}
			closeret = run_as_unlink(shm_path,
						 lttng_credentials_get_uid(LTTNG_OPTIONAL_GET_PTR(
							 channel->buffer_credentials)),
						 lttng_credentials_get_gid(LTTNG_OPTIONAL_GET_PTR(
							 channel->buffer_credentials)));
			if (closeret) {
				PERROR("unlink %s", shm_path);
			}
		}
	}
	/* Try to rmdir all directories under shm_path root. */
	if (channel->root_shm_path[0]) {
		(void) run_as_rmdir_recursive(channel->root_shm_path,
					      lttng_credentials_get_uid(LTTNG_OPTIONAL_GET_PTR(
						      channel->buffer_credentials)),
					      lttng_credentials_get_gid(LTTNG_OPTIONAL_GET_PTR(
						      channel->buffer_credentials)),
					      LTTNG_DIRECTORY_HANDLE_SKIP_NON_EMPTY_FLAG);
	}
	free(stream_fds);
error_alloc:
error_channel_type:
	return ret;
}

/*
 * Send a single given stream to the session daemon using the sock.
 *
 * Return 0 on success else a negative value.
 */
static int send_sessiond_stream(int sock, struct lttng_consumer_stream *stream)
{
	int ret;

	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(sock >= 0);

	DBG("UST consumer sending stream %" PRIu64 " to sessiond", stream->key);

	/* Send stream to session daemon. */
	ret = lttng_ust_ctl_send_stream_to_sessiond(sock, stream->ustream);
	if (ret < 0) {
		goto error;
	}

error:
	return ret;
}

/*
 * Send channel to sessiond and relayd if applicable.
 *
 * Return 0 on success or else a negative value.
 */
static int send_channel_to_sessiond_and_relayd(int sock,
					       struct lttng_consumer_channel *channel,
					       struct lttng_consumer_local_data *ctx,
					       int *relayd_error)
{
	int ret, ret_code = LTTCOMM_CONSUMERD_SUCCESS;
	uint64_t net_seq_idx = -1ULL;

	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(ctx);
	LTTNG_ASSERT(sock >= 0);

	DBG("UST consumer sending channel %s to sessiond", channel->name);

	if (channel->relayd_id != (uint64_t) -1ULL) {
		for (auto& stream : channel->get_streams()) {
			health_code_update();

			/* Try to send the stream to the relayd if one is available. */
			DBG("Sending stream %" PRIu64 " of channel \"%s\" to relayd",
			    stream.key,
			    channel->name);
			ret = consumer_send_relayd_stream(&stream, stream.chan->pathname);
			if (ret < 0) {
				/*
				 * Flag that the relayd was the problem here probably due to a
				 * communicaton error on the socket.
				 */
				if (relayd_error) {
					*relayd_error = 1;
				}
				ret_code = LTTCOMM_CONSUMERD_RELAYD_FAIL;
			}
			if (net_seq_idx == -1ULL) {
				net_seq_idx = stream.net_seq_idx;
			}
		}
	}

	/* Inform sessiond that we are about to send channel and streams. */
	ret = consumer_send_status_msg(sock, ret_code);
	if (ret < 0 || ret_code != LTTCOMM_CONSUMERD_SUCCESS) {
		/*
		 * Either the session daemon is not responding or the relayd died so we
		 * stop now.
		 */
		goto error;
	}

	/* Send channel to sessiond. */
	ret = lttng_ust_ctl_send_channel_to_sessiond(sock, channel->uchan);
	if (ret < 0) {
		goto error;
	}

	ret = lttng_ust_ctl_channel_close_wakeup_fd(channel->uchan);
	if (ret < 0) {
		goto error;
	}

	/* The channel was sent successfully to the sessiond at this point. */
	for (auto& stream : channel->get_streams()) {
		health_code_update();

		/* Send stream to session daemon. */
		ret = send_sessiond_stream(sock, &stream);
		if (ret < 0) {
			goto error;
		}
	}

	/* Tell sessiond there is no more stream. */
	ret = lttng_ust_ctl_send_stream_to_sessiond(sock, nullptr);
	if (ret < 0) {
		goto error;
	}

	DBG("UST consumer NULL stream sent to sessiond");

	return 0;

error:
	if (ret_code != LTTCOMM_CONSUMERD_SUCCESS) {
		ret = -1;
	}
	return ret;
}

/*
 * Creates a channel and streams and add the channel it to the channel internal
 * state. The created stream must ONLY be sent once the GET_CHANNEL command is
 * received.
 *
 * Return 0 on success or else, a negative value is returned and the channel
 * MUST be destroyed by consumer_del_channel().
 */
static int ask_channel(struct lttng_consumer_local_data *ctx,
		       struct lttng_consumer_channel *channel,
		       struct lttng_ust_ctl_consumer_channel_attr *attr)
{
	int ret;

	LTTNG_ASSERT(ctx);
	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(attr);

	/*
	 * This value is still used by the kernel consumer since for the kernel,
	 * the stream ownership is not IN the consumer so we need to have the
	 * number of left stream that needs to be initialized so we can know when
	 * to delete the channel (see consumer.c).
	 *
	 * As for the user space tracer now, the consumer creates and sends the
	 * stream to the session daemon which only sends them to the application
	 * once every stream of a channel is received making this value useless
	 * because we they will be added to the poll thread before the application
	 * receives them. This ensures that a stream can not hang up during
	 * initilization of a channel.
	 */
	channel->nb_init_stream_left = 0;

	/* The reply msg status is handled in the following call. */
	ret = create_ust_channel(channel, attr, &channel->uchan);
	if (ret < 0) {
		goto end;
	}

	channel->wait_fd = lttng_ust_ctl_channel_get_wait_fd(channel->uchan);

	/*
	 * For the snapshots (no monitor), we create the metadata streams
	 * on demand, not during the channel creation.
	 */
	if (channel->type == CONSUMER_CHANNEL_TYPE_METADATA && !channel->monitor) {
		ret = 0;
		goto end;
	}

	/* Open all streams for this channel. */
	pthread_mutex_lock(&channel->lock);
	ret = create_ust_streams(channel, ctx);
	pthread_mutex_unlock(&channel->lock);
	if (ret < 0) {
		goto end;
	}

end:
	return ret;
}

/*
 * Send all stream of a channel to the right thread handling it.
 *
 * On error, return a negative value else 0 on success.
 */
static int send_streams_to_thread(struct lttng_consumer_channel *channel,
				  struct lttng_consumer_local_data *ctx)
{
	int ret = 0;

	LTTNG_ASSERT(channel);
	LTTNG_ASSERT(!channel->is_deleted);
	LTTNG_ASSERT(ctx);

	/* Send streams to the corresponding thread. */
	for (auto& stream :
	     channel->get_streams(lttng::consumer::stream_set::filter::UNPUBLISHED)) {
		health_code_update();

		const lttng::pthread::lock_guard stream_lock(stream.lock);

		/* Sending the stream to the thread. */
		ret = send_stream_to_thread(&stream, ctx);
		if (ret < 0) {
			/*
			 * If we are unable to send the stream to the thread, there is
			 * a big problem so just stop everything.
			 */
			goto error;
		}
	}

error:
	return ret;
}

/*
 * Flush channel's streams using the given key to retrieve the channel.
 *
 * Return 0 on success else an LTTng error code.
 */
static int flush_channel(uint64_t chan_key)
{
	int ret = 0;

	DBG("UST consumer flush channel key %" PRIu64, chan_key);

	const lttng::urcu::read_lock_guard read_lock;
	struct lttng_consumer_channel *channel = nullptr;

	{
		const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);
		channel = consumer_find_channel(chan_key);
		if (!channel) {
			ERR("UST consumer flush channel %" PRIu64 " not found", chan_key);
			return LTTNG_ERR_UST_CHAN_NOT_FOUND;
		}

		const lttng::pthread::lock_guard channel_lock(channel->lock);

		/* For each stream of the channel id, flush it. */
		for (auto& stream : channel->get_streams()) {
			health_code_update();

			/*
			 * Protect against concurrent teardown of a stream.
			 */
			if (cds_lfht_is_node_deleted(&stream.node.node)) {
				continue;
			}

			const lttng::pthread::lock_guard stream_lock(stream.lock);

			if (!stream.quiescent) {
				ret = lttng_ust_ctl_flush_buffer(stream.ustream, 0);
				if (ret) {
					ERR("Failed to flush buffer while flushing channel: channel key = %" PRIu64
					    ", channel name = '%s'",
					    chan_key,
					    channel->name);
					return LTTNG_ERR_BUFFER_FLUSH_FAILED;
				}

				stream.quiescent = true;
			}
		}
	}

	lttng_ustconsumer_quiescent_stalled_channel(*channel);

	/*
	 * Send one last buffer statistics update to the session daemon. This
	 * ensures that the session daemon gets at least one statistics update
	 * per channel even in the case of short-lived channels, such as when a
	 * short-lived app is traced in per-pid mode.
	 */
	if (channel->monitor_timer_task) {
		channel->monitor_timer_task->run(std::chrono::steady_clock::now());
	}

	return ret;
}

/*
 * Clear quiescent state from channel's streams using the given key to
 * retrieve the channel.
 *
 * Return 0 on success else an LTTng error code.
 */
static int clear_quiescent_channel(uint64_t chan_key)
{
	DBG("UST consumer clear quiescent channel key %" PRIu64, chan_key);

	const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);
	const lttng::urcu::read_lock_guard read_lock;
	auto channel = consumer_find_channel(chan_key);
	if (!channel) {
		ERR("UST consumer clear quiescent channel %" PRIu64 " not found", chan_key);
		return LTTNG_ERR_UST_CHAN_NOT_FOUND;
	}

	const lttng::pthread::lock_guard channel_lock(channel->lock);

	/* For each stream of the channel id, clear quiescent state. */
	for (auto& stream : channel->get_streams()) {
		health_code_update();

		const lttng::pthread::lock_guard stream_lock(stream.lock);
		stream.quiescent = false;
	}

	return 0;
}

/*
 * Close metadata stream wakeup_fd using the given key to retrieve the channel.
 *
 * Return 0 on success else an LTTng error code.
 */
static int close_metadata(uint64_t chan_key)
{
	int ret = 0;
	struct lttng_consumer_channel *channel;
	unsigned int channel_monitor;

	DBG("UST consumer close metadata key %" PRIu64, chan_key);

	channel = consumer_find_channel(chan_key);
	if (!channel) {
		/*
		 * This is possible if the metadata thread has issue a delete because
		 * the endpoint point of the stream hung up. There is no way the
		 * session daemon can know about it thus use a DBG instead of an actual
		 * error.
		 */
		DBG("UST consumer close metadata %" PRIu64 " not found", chan_key);
		ret = LTTNG_ERR_UST_CHAN_NOT_FOUND;
		goto error;
	}

	pthread_mutex_lock(&the_consumer_data.lock);
	pthread_mutex_lock(&channel->lock);
	channel_monitor = channel->monitor;
	if (cds_lfht_is_node_deleted(&channel->node.node)) {
		goto error_unlock;
	}

	lttng_ustconsumer_close_metadata(channel);
	pthread_mutex_unlock(&channel->lock);
	pthread_mutex_unlock(&the_consumer_data.lock);

	/*
	 * The ownership of a metadata channel depends on the type of
	 * session to which it belongs. In effect, the monitor flag is checked
	 * to determine if this metadata channel is in "snapshot" mode or not.
	 *
	 * In the non-snapshot case, the metadata channel is created along with
	 * a single stream which will remain present until the metadata channel
	 * is destroyed (on the destruction of its session). In this case, the
	 * metadata stream in "monitored" by the metadata poll thread and holds
	 * the ownership of its channel.
	 *
	 * Closing the metadata will cause the metadata stream's "metadata poll
	 * pipe" to be closed. Closing this pipe will wake-up the metadata poll
	 * thread which will teardown the metadata stream which, in return,
	 * deletes the metadata channel.
	 *
	 * In the snapshot case, the metadata stream is created and destroyed
	 * on every snapshot record. Since the channel doesn't have an owner
	 * other than the session daemon, it is safe to destroy it immediately
	 * on reception of the CLOSE_METADATA command.
	 */
	if (!channel_monitor) {
		/*
		 * The channel and consumer_data locks must be
		 * released before this call since consumer_del_channel
		 * re-acquires the channel and consumer_data locks to teardown
		 * the channel and queue its reclamation by the "call_rcu"
		 * worker thread.
		 */
		consumer_del_channel(channel);
	}

	return ret;
error_unlock:
	pthread_mutex_unlock(&channel->lock);
	pthread_mutex_unlock(&the_consumer_data.lock);
error:
	return ret;
}

/*
 * RCU read side lock MUST be acquired before calling this function.
 *
 * Return 0 on success else an LTTng error code.
 */
static int setup_metadata(struct lttng_consumer_local_data *ctx, uint64_t key)
{
	int ret;
	struct lttng_consumer_channel *metadata;

	ASSERT_RCU_READ_LOCKED();

	DBG("UST consumer setup metadata key %" PRIu64, key);
	{
		const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);

		metadata = consumer_find_channel(key);
		if (!metadata) {
			ERR("UST consumer push metadata %" PRIu64 " not found", key);
			return LTTNG_ERR_UST_CHAN_NOT_FOUND;
		}

		const lttng::pthread::lock_guard channel_lock(metadata->lock);
		const lttng::pthread::lock_guard channel_timer_lock(metadata->timer_lock);

		/*
		 * In no monitor mode, the metadata channel has no stream(s) so skip the
		 * ownership transfer to the metadata thread.
		 */
		if (!metadata->monitor) {
			DBG("Metadata channel in no monitor");
			ret = 0;
			goto end;
		}

		/*
		 * Send metadata stream to relayd if one available. Availability is
		 * known if the stream is still in the list of the channel.
		 */
		if (cds_list_empty(&metadata->streams.head)) {
			ERR("Metadata channel key %" PRIu64 ", no stream available.", key);
			ret = LTTCOMM_CONSUMERD_ERROR_METADATA;
			goto error_no_stream;
		}

		/* Send metadata stream to relayd if needed. */
		if (metadata->metadata_stream->net_seq_idx != (uint64_t) -1ULL) {
			ret = consumer_send_relayd_stream(metadata->metadata_stream,
							  metadata->pathname);
			if (ret < 0) {
				ret = LTTCOMM_CONSUMERD_ERROR_METADATA;
				goto error;
			}
			ret = consumer_send_relayd_streams_sent(
				metadata->metadata_stream->net_seq_idx);
			if (ret < 0) {
				ret = LTTCOMM_CONSUMERD_RELAYD_FAIL;
				goto error;
			}
		}

		/*
		 * Ownership of metadata stream is passed along. Freeing is handled by
		 * the callee.
		 */
		ret = send_streams_to_thread(metadata, ctx);
		if (ret < 0) {
			/*
			 * If we are unable to send the stream to the thread, there is
			 * a big problem so just stop everything.
			 */
			ret = LTTCOMM_CONSUMERD_FATAL;
			goto send_streams_error;
		}
		/* List MUST be empty after or else it could be reused. */
		LTTNG_ASSERT(cds_list_empty(&metadata->streams.head));

		ret = 0;
		goto end;
	}

error:
	/*
	 * Delete metadata channel on error. At this point, the metadata stream can
	 * NOT be monitored by the metadata thread thus having the guarantee that
	 * the stream is still in the local stream list of the channel. This call
	 * will make sure to clean that list.
	 */
	consumer_stream_destroy(metadata->metadata_stream, nullptr);
	metadata->metadata_stream = nullptr;
	metadata->metadata_pushed_wait_queue.wake_all();

send_streams_error:
error_no_stream:
end:
	return ret;
}

/*
 * Snapshot the whole metadata.
 * RCU read-side lock must be held by the caller.
 *
 * Returns 0 on success, < 0 on error
 */
static int snapshot_metadata(struct lttng_consumer_channel *metadata_channel,
			     uint64_t key,
			     char *path,
			     uint64_t relayd_id,
			     struct lttng_consumer_local_data *ctx)
{
	int ret = 0;
	struct lttng_consumer_stream *metadata_stream;

	LTTNG_ASSERT(path);
	LTTNG_ASSERT(ctx);
	ASSERT_RCU_READ_LOCKED();

	DBG("UST consumer snapshot metadata with key %" PRIu64 " at path %s", key, path);

	const lttng::urcu::read_lock_guard read_lock;

	LTTNG_ASSERT(!metadata_channel->monitor);

	health_code_update();

	/*
	 * Ask the sessiond if we have new metadata waiting and update the
	 * consumer metadata cache.
	 */
	ret = lttng_ustconsumer_request_metadata(
		*metadata_channel, ctx->metadata_socket, ctx->consumer_error_socket, false, 1);
	if (ret < 0) {
		goto error;
	}

	health_code_update();

	/*
	 * The metadata stream is NOT created in no monitor mode when the channel
	 * is created on a sessiond ask channel command.
	 */
	ret = create_ust_streams(metadata_channel, ctx);
	if (ret < 0) {
		goto error;
	}

	metadata_stream = metadata_channel->metadata_stream;
	LTTNG_ASSERT(metadata_stream);

	metadata_stream->read_subbuffer_ops.lock(metadata_stream);
	if (relayd_id != (uint64_t) -1ULL) {
		metadata_stream->net_seq_idx = relayd_id;
		ret = consumer_send_relayd_stream(metadata_stream, path);
	} else {
		ret = consumer_stream_create_output_files(metadata_stream, false);
	}
	if (ret < 0) {
		goto error_stream;
	}

	do {
		health_code_update();
		ret = lttng_consumer_read_subbuffer(metadata_stream, ctx, true);
		if (ret < 0) {
			goto error_stream;
		}
	} while (ret > 0);

error_stream:
	metadata_stream->read_subbuffer_ops.unlock(metadata_stream);
	/*
	 * Clean up the stream completely because the next snapshot will use a
	 * new metadata stream.
	 */
	consumer_stream_destroy(metadata_stream, nullptr);
	metadata_channel->metadata_stream = nullptr;
	metadata_channel->metadata_pushed_wait_queue.wake_all();

error:
	return ret;
}

static int get_current_subbuf_addr(struct lttng_consumer_stream *stream, const char **addr)
{
	int ret;
	unsigned long mmap_offset;
	const char *mmap_base;

	mmap_base = (const char *) lttng_ust_ctl_get_mmap_base(stream->ustream);
	if (!mmap_base) {
		ERR("Failed to get mmap base for stream `%s`", stream->name);
		ret = -EPERM;
		goto error;
	}

	ret = lttng_ust_ctl_get_mmap_read_offset(stream->ustream, &mmap_offset);
	if (ret != 0) {
		ERR("Failed to get mmap offset for stream `%s`", stream->name);
		ret = -EINVAL;
		goto error;
	}

	*addr = mmap_base + mmap_offset;
error:
	return ret;
}

/*
 * Take a snapshot of all the streams of a channel.
 * RCU read-side lock and the channel lock must be held by the caller.
 *
 * Returns 0 on success, < 0 on error
 */
static int snapshot_channel(struct lttng_consumer_channel *channel,
			    uint64_t key,
			    char *path,
			    uint64_t relayd_id,
			    uint64_t nb_packets_per_stream,
			    struct lttng_consumer_local_data *ctx)
{
	/* Allocate a terminal packet for the snapshot process. */
	auto terminal_packet = []() {
		lttng_ust_ctl_consumer_packet *raw_packet = nullptr;
		lttng_ust_ctl_packet_create(&raw_packet);
		return lttng::make_unique_wrapper<lttng_ust_ctl_consumer_packet,
						  lttng_ust_ctl_packet_destroy>(raw_packet);
	}();
	const bool use_relayd = relayd_id != (uint64_t) -1ULL;
	int ret;

	LTTNG_ASSERT(path);
	LTTNG_ASSERT(ctx);
	ASSERT_RCU_READ_LOCKED();

	/* Prevent channel modifications while we perform the snapshot. */
	const lttng::pthread::lock_guard channe_lock(channel->lock);

	if (!terminal_packet) {
		ERR("Failed to allocate lttng-ust consumer packet");
		return -1;
	}

	LTTNG_ASSERT(!channel->monitor);
	DBG("UST consumer snapshot channel %" PRIu64, key);

	for (auto& stream : channel->get_streams()) {
		unsigned long consumed_pos, produced_pos;
		bool terminal_packet_populated = false;

		health_code_update();

		/* Lock stream because we are about to change its state. */
		const lttng::pthread::lock_guard stream_lock(stream.lock);
		LTTNG_ASSERT(channel->trace_chunk);
		if (!lttng_trace_chunk_get(channel->trace_chunk)) {
			/*
			 * Can't happen barring an internal error as the channel
			 * holds a reference to the trace chunk.
			 */
			ERR("Failed to acquire reference to channel's trace chunk");
			return -1;
		}

		LTTNG_ASSERT(!stream.trace_chunk);
		stream.trace_chunk = channel->trace_chunk;
		stream.net_seq_idx = relayd_id;

		/* Close stream output when we are done. */
		const auto close_stream_output = lttng::make_scope_exit(
			[&stream]() noexcept { consumer_stream_close_output(&stream); });

		/* Handle relayd or local file output. */
		if (use_relayd) {
			ret = consumer_send_relayd_stream(&stream, path);
			if (ret < 0) {
				return ret;
			}
		} else {
			ret = consumer_stream_create_output_files(&stream, false);
			if (ret < 0) {
				return ret;
			}

			DBG("UST consumer snapshot stream (%" PRIu64 ")", stream.key);
		}

		/*
		 * Handle empty or terminal packets:
		 * - If no events were produced, generate an empty packet to indicate
		 *   the recording interval.
		 * - If consecutive snapshots are taken without new events, generate
		 *   a terminal packet to indicate the recording was still active.
		 */
		if (!stream.quiescent) {
			ret = lttng_ustconsumer_flush_buffer_or_populate_packet(
				&stream, terminal_packet.get(), &terminal_packet_populated, nullptr);
			if (ret < 0) {
				ERR("Failed to flush buffer during snapshot of channel: channel key = %" PRIu64
				    ", channel name='%s', ret=%d",
				    channel->key,
				    channel->name,
				    ret);
				return ret;
			}
		}

		bool forced_empty_packet = false;

		/* Take a snapshot of the stream's positions. */
		ret = lttng_ustconsumer_take_snapshot(&stream);
		if (ret == -EAGAIN) {
			DBG_FMT("Stream has no active packet (no activity yet), forcing a flush before snapshot: session_id={}, channel_name=`{}`, channel_key={}, stream_key={}",
				channel->session_id,
				channel->name,
				channel->key,
				stream.key);

			/*
			 * There was no content in the buffers, produce an empty packet
			 * so that readers can infer that tracing was underway for that
			 * stream.
			 */
			ret = consumer_stream_flush_buffer(&stream, false);
			if (ret < 0) {
				ERR_FMT("Failed to force a flush before snapshot on an empty stream: session_id={}, channel_name=`{}`, channel_key={}, stream_key={}",
					channel->session_id,
					channel->name,
					channel->key,
					stream.key);
				return ret;
			}

			forced_empty_packet = true;
			ret = lttng_ustconsumer_take_snapshot(&stream);
			if (ret < 0) {
				ERR_FMT("Failed to sample positions while taking a snapshot: session_id={}, channel_name=`{}`, channel_key={}, stream_key={}",
					channel->session_id,
					channel->name,
					channel->key,
					stream.key);
				return ret;
			}
		} else if (ret < 0) {
			ERR_FMT("Failed to sample positions while taking a snapshot: session_id={}, channel_name=`{}`, channel_key={}, stream_key={}",
				channel->session_id,
				channel->name,
				channel->key,
				stream.key);
			return ret;
		}

		ret = lttng_ustconsumer_get_produced_snapshot(&stream, &produced_pos);
		if (ret < 0) {
			ERR_FMT("Failed to get produced position while taking a snapshot: session_id={}, channel_name=`{}`, channel_key={}, stream_key={}",
				channel->session_id,
				channel->name,
				channel->key,
				stream.key);
			return ret;
		}

		ret = lttng_ustconsumer_get_consumed_snapshot(&stream, &consumed_pos);
		if (ret < 0) {
			ERR_FMT("Failed to get consumed position while taking a snapshot: session_id={}, channel_name=`{}`, channel_key={}, stream_key={}",
				channel->session_id,
				channel->name,
				channel->key,
				stream.key);
			return ret;
		}

		/* Adjust the consumed position based on the number of packets to snapshot. */
		consumed_pos = consumer_get_consume_start_pos(
			consumed_pos, produced_pos, nb_packets_per_stream, stream.max_sb_size);

		/* Process each available sub-buffer in the stream. */
		while ((long) (consumed_pos - produced_pos) < 0) {
			ssize_t read_len;
			unsigned long len, padded_len;
			const char *subbuf_addr;
			struct lttng_buffer_view subbuf_view;

			health_code_update();

			DBG("UST consumer taking snapshot at pos %lu", consumed_pos);

			ret = lttng_ust_ctl_get_subbuf(stream.ustream, &consumed_pos);
			if (ret < 0) {
				if (ret != -EAGAIN) {
					PERROR("lttng_ust_ctl_get_subbuf snapshot");
					return ret;
				}

				DBG("UST consumer get subbuf failed. Skipping it.");
				consumed_pos += stream.max_sb_size;
				stream.chan->lost_packets++;
				continue;
			}

			/* Put the subbuffer once we are done. */
			const auto put_subbuf = lttng::make_scope_exit([&stream]() noexcept {
				if (lttng_ust_ctl_put_subbuf(stream.ustream) < 0) {
					ERR("Snapshot lttng_ust_ctl_put_subbuf");
				}
			});

			ret = lttng_ust_ctl_get_subbuf_size(stream.ustream, &len);
			if (ret < 0) {
				ERR("Snapshot lttng_ust_ctl_get_subbuf_size");
				return ret;
			}

			ret = lttng_ust_ctl_get_padded_subbuf_size(stream.ustream, &padded_len);
			if (ret < 0) {
				ERR("Snapshot lttng_ust_ctl_get_padded_subbuf_size");
				return ret;
			}

			ret = get_current_subbuf_addr(&stream, &subbuf_addr);
			if (ret) {
				return ret;
			}

			subbuf_view = lttng_buffer_view_init(subbuf_addr, 0, padded_len);
			read_len = lttng_consumer_on_read_subbuffer_mmap(
				&stream, &subbuf_view, padded_len - len);
			if (use_relayd) {
				if (read_len != len) {
					return -EPERM;
				}
			} else {
				if (read_len != padded_len) {
					return -EPERM;
				}
			}

			consumed_pos += stream.max_sb_size;
		}

		/* Append terminal packet if necessary. */
		if (terminal_packet_populated && !forced_empty_packet) {
			uint64_t length, packet_length = 0, packet_length_padded = 0;
			struct lttng_buffer_view subbuf_view;
			ssize_t read_len;
			const char *src;

			ret = lttng_ust_ctl_packet_get_buffer(terminal_packet.get(),
							      (void **) &src,
							      &packet_length,
							      &packet_length_padded);
			if (ret < 0) {
				WARN("Failed to get terminal packet, ret=%d", ret);
				return ret;
			}

			if (use_relayd) {
				length = packet_length;
			} else {
				length = packet_length_padded;
			}

			subbuf_view =
				lttng_buffer_view_init(src, 0, (ptrdiff_t) packet_length_padded);
			read_len = lttng_consumer_on_read_subbuffer_mmap(
				&stream, &subbuf_view, packet_length_padded - packet_length);
			if (read_len < length) {
				WARN("Failed to write terminal packet to stream, read %ld of %ld",
				     read_len,
				     length);
				return -EPERM;
			}
		}

		/* Simply close the stream so we can use it on the next snapshot. */
		consumer_stream_close_output(&stream);
	}

	return 0;
}

static void metadata_stream_reset_cache_consumed_position(struct lttng_consumer_stream *stream)
{
	ASSERT_LOCKED(stream->lock);

	DBG("Reset metadata cache of session %" PRIu64, stream->chan->session_id);
	stream->ust_metadata_pushed = 0;
}

static int stream_send_live_beacon(lttng_consumer_stream& stream)
{
	uint64_t ts, stream_id;
	int ret;

	ret = cds_lfht_is_node_deleted(&stream.node.node);
	if (ret) {
		goto end;
	}

	ret = lttng_ustconsumer_get_current_timestamp(&stream, &ts);
	if (ret < 0) {
		ERR("Failed to get the current timestamp");
		goto end;
	}
	ret = lttng_ustconsumer_flush_buffer(&stream, 1);
	if (ret < 0) {
		ERR("Failed to flush buffer while flushing index");
		goto end;
	}
	ret = lttng_ustconsumer_take_snapshot(&stream);
	if (ret < 0) {
		if (ret != -EAGAIN) {
			ERR("Taking UST snapshot");
			ret = -1;
			goto end;
		}
		ret = lttng_ustconsumer_get_stream_id(&stream, &stream_id);
		if (ret < 0) {
			PERROR("lttng_ust_ctl_get_stream_id");
			goto end;
		}
		DBG("Stream %" PRIu64 " empty, sending beacon", stream.key);
		ret = consumer_stream_send_live_beacon(stream, ts, stream_id);
		if (ret < 0) {
			goto end;
		}
	}
	ret = 0;
end:
	return ret;
}

static void lttng_ustconsumer_get_channels_memory_usage(int socket, std::uint64_t channel_count)
{
	std::vector<std::uint64_t> channel_keys;

	channel_keys.resize(channel_count);
	const auto channel_key_payload_size =
		channel_count * sizeof(decltype(channel_keys)::value_type);
	const auto recv_ret =
		lttcomm_recv_unix_sock(socket, channel_keys.data(), channel_key_payload_size);

	if (recv_ret != channel_key_payload_size) {
		LTTNG_THROW_POSIX("Failed to receive channel keys from session daemon", errno);
	}

	/*
	 * The reply has the following structure:
	 * - generic reply header (announcing the command status and payload size)
	 * - command-specific reply header (announcing the number of stream memory usage entries)
	 * - stream memory usage entries (contains channel key, logical size, and physical size)
	 */
	std::vector<std::uint8_t> reply_payload;
	lttcomm_consumer_status_msg generic_reply_header = {};
	lttcomm_consumer_channel_memory_usage_reply_header command_specific_reply_header = {};

	/*
	 * The two headers are inserted in the payload to "reserve" space for them before the
	 * actual stream memory usage entries are inserted.
	 *
	 * Their content will be overwritten later with the actual command status and
	 * the number of stream memory usage entries.
	 */
	reply_payload.resize(sizeof(generic_reply_header) + sizeof(command_specific_reply_header));

	std::size_t total_stream_count = 0;
	for (const auto channel_key : channel_keys) {
		auto *channel = consumer_find_channel(channel_key);
		if (!channel) {
			LTTNG_THROW_CHANNEL_NOT_FOUND_BY_KEY_ERROR(channel_key);
		}

		const lttng::pthread::lock_guard channel_lock(channel->lock);
		DBG_FMT("Measuring memory usage of channel: key={}, channel_name=`{}`",
			channel_key,
			channel->name);

		for (std::size_t stream_idx = 0; stream_idx < channel->nr_stream_fds;
		     stream_idx++) {
			const auto stream_fd = channel->stream_fds[stream_idx];

			struct stat file_status;
			if (fstat(stream_fd, &file_status) == -1) {
				LTTNG_THROW_POSIX(
					fmt::format(
						"Failed to fstat stream file descriptor of channel: channel_key={}, channel_name=`{}`, stream_fd={}",
						channel_key,
						channel->name,
						stream_fd),
					errno);
			}

			const auto logical_size = static_cast<std::uint64_t>(file_status.st_size);
			const auto physical_size =
				static_cast<std::uint64_t>(file_status.st_blocks) * 512;

			const lttcomm_stream_memory_usage stream_memory_usage = {
				.channel_key = channel_key,
				.logical_size_bytes = logical_size,
				.physical_size_bytes = physical_size,
			};

			reply_payload.insert(
				reply_payload.end(),
				reinterpret_cast<const std::uint8_t *>(&stream_memory_usage),
				reinterpret_cast<const std::uint8_t *>(&stream_memory_usage) +
					sizeof(stream_memory_usage));
		}

		total_stream_count += channel->nr_stream_fds;
	}

	/* Update the payload headers. */
	generic_reply_header.ret_code = LTTCOMM_CONSUMERD_SUCCESS;
	generic_reply_header.payload_size = reply_payload.size() - sizeof(generic_reply_header);
	std::memcpy(reply_payload.data(), &generic_reply_header, sizeof(generic_reply_header));

	command_specific_reply_header.count = total_stream_count;
	std::memcpy(reply_payload.data() + sizeof(generic_reply_header),
		    &command_specific_reply_header,
		    sizeof(command_specific_reply_header));

	const auto send_ret =
		lttcomm_send_unix_sock(socket, reply_payload.data(), reply_payload.size());
	if (send_ret != reply_payload.size()) {
		LTTNG_THROW_POSIX(
			fmt::format(
				"Failed to send channel memory usage reply to session daemon: payload_size={} bytes",
				reply_payload.size()),
			errno);
	}
}

static void destroy_subbuf_iter(lttng_ust_ctl_subbuf_iter *it)
{
	if (!it) {
		return;
	}

	const auto ret = lttng_ust_ctl_subbuf_iter_destroy(it);
	LTTNG_ASSERT(ret == 0);
}

static std::size_t reclaim_stream_memory(lttng_consumer_stream& stream,
					 nonstd::optional<std::chrono::microseconds> age_limit,
					 bool require_consumed)
{
	DBG_FMT("Reclaiming stream memory: channel_name=`{}`, stream_key={}",
		stream.chan->name,
		stream.key);

	std::uint64_t current_tracer_time;
	const auto current_time_ret =
		lttng_ust_ctl_get_current_timestamp(stream.ustream, &current_tracer_time);
	if (current_time_ret < 0) {
		LTTNG_THROW_ERROR(fmt::format(
			"Failed to get current tracer time for stream: channel_name=`{}`, stream_key={}, error={}",
			stream.chan->name,
			stream.key,
			current_time_ret));
	}

	bool should_flush = false;
	if (age_limit) {
		std::uint64_t expiry_limit = current_tracer_time;
		lttng_ust_ctl_timestamp_add(stream.ustream,
					    &expiry_limit,
					    -std::chrono::nanoseconds(*age_limit).count());

		const auto compare_ret =
			lttng_ust_ctl_last_activity_timestamp_compare(stream.ustream, expiry_limit);
		if (compare_ret <= 0) {
			should_flush = true;
		}
	} else {
		should_flush = true;
	}

	if (should_flush) {
		const auto flush_ret = lttng_ust_ctl_flush_buffer(stream.ustream, 1);
		if (flush_ret) {
			WARN_FMT(
				"Failed to flush stream when reclaiming buffer memory: flush_ret={}",
				flush_ret);
		}
	}

	/* The iterator is initially invalid; call [...]_next() before accessing its value. */
	auto subbuf_iter = lttng::make_unique_wrapper<lttng_ust_ctl_subbuf_iter,
						      destroy_subbuf_iter>([&stream,
									    require_consumed]() {
		lttng_ust_ctl_subbuf_iter *it = nullptr;

		const auto err = lttng_ust_ctl_subbuf_iter_create(
			stream.ustream,
			require_consumed ? LTTNG_UST_CTL_SUBBUF_ITER_DELIVERED_CONSUMED :
					   LTTNG_UST_CTL_SUBBUF_ITER_DELIVERED,
			&it);
		if (err) {
			ERR_FMT("Failed to create sub-buffer iterator for stream: stream_name=`{}`, stream_key={}, error={}",
				stream.name,
				stream.key,
				-err);
		}

		return it;
	}());

	if (!subbuf_iter) {
		LTTNG_THROW_ALLOCATION_FAILURE_ERROR(fmt::format(
			"Failed to create sub-buffer iterator for stream: channel_name=`{}`, stream_key={}",
			stream.chan->name,
			stream.key));
	}

	unsigned int reclaimed_subbuf_count = 0;
	while (true) {
		const auto next_ret = lttng_ust_ctl_subbuf_iter_next(subbuf_iter.get());
		if (next_ret == 0) {
			DBG_FMT("Sub-buffer iterator reached end of stream: channel_name=`{}`, stream_key={}",
				stream.chan->name,
				stream.key);
			break;
		} else if (next_ret < 0) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to move sub-buffer iterator for stream: channel_name=`{}`, stream_key={}, error={}",
				stream.chan->name,
				stream.key,
				next_ret));
		}

		unsigned long subbuf_position;
		const auto subbuf_pos_ret =
			lttng_ust_ctl_subbuf_iter_pos(subbuf_iter.get(), &subbuf_position);
		if (subbuf_pos_ret < 0) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to get sub-buffer iterator position for stream: channel_name=`{}`, stream_key={}, error={}",
				stream.chan->name,
				stream.key,
				subbuf_pos_ret));
		}

		std::uint64_t subbuf_timestamp;
		const auto timestamp_ret = lttng_ust_ctl_subbuf_iter_timestamp_end(
			subbuf_iter.get(), &subbuf_timestamp);
		if (timestamp_ret < 0) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to get sub-buffer iterator timestamp for stream: channel_name=`{}`, stream_key={}, error={}",
				stream.chan->name,
				stream.key,
				timestamp_ret));
		}

		bool is_allocated;
		const auto allocated_ret =
			lttng_ust_ctl_subbuf_iter_allocated(subbuf_iter.get(), &is_allocated);
		if (allocated_ret < 0) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to check if sub-buffer iterator is allocated for stream: channel_name=`{}`, stream_key={}, error={}",
				stream.chan->name,
				stream.key,
				allocated_ret));
		}

		DBG_FMT("Sub-buffer iterator properties: channel_name=`{}`, stream_key={}, position={}, allocated={}, current_timestamp={}, subbuf_timestamp={}",
			stream.chan->name,
			stream.key,
			subbuf_position,
			is_allocated,
			current_tracer_time,
			subbuf_timestamp);
		if (!is_allocated) {
			/* Sub-buffer already reclaimed, skip it. */
			continue;
		}

		if (age_limit) {
			std::uint64_t expiry_limit = subbuf_timestamp;
			lttng_ust_ctl_timestamp_add(stream.ustream,
						    &expiry_limit,
						    std::chrono::nanoseconds(*age_limit).count());

			/*
			 * Timeline:
			 * <----[subbuffer lifetime]---------------|---------------------|------>
			 * 	                   ^               ^                     ^
			 *            subbuffer end timestamp   current_tracer_time  expiry_limit
			 *
			 * expiry limit is the subbuffer's end timestamp shifted in the future
			 * by the age limit. When that limit ends up _before_ the current time, the
			 * subbuffer should be reclaimed.
			 */
			if (expiry_limit > current_tracer_time) {
				DBG_FMT("Sub-buffer is too recent, skipping: channel_name=`{}`, stream_key={}, subbuf_timestamp={}, oldest_data_limit={}",
					stream.chan->name,
					stream.key,
					subbuf_timestamp,
					expiry_limit);
				continue;
			}
		}

		/*
		 * Attempt to reclaim the sub-buffer.
		 *
		 * Start by reclaiming the reader sub-buffer, then attempt to exchange it with
		 * the iterator's current sub-buffer.
		 *
		 * Exchanging can fail legitimately (i.e., a writer has entered the sub-buffer), in
		 * which case we simply skip the sub-buffer and continue to the next one.
		 */
		const auto reclaim_ret = lttng_ust_ctl_reclaim_reader_subbuf(stream.ustream);
		/*
		 * Ignore -ENOMEM; it simply means the reader sub-buffer was already reclaimed
		 * previously. For instance, an exchange could have been attempted
		 * but failed due to the sub-buffer being in use by a writer during a previous
		 * reclamation attempt.
		 */
		if (reclaim_ret < 0 && reclaim_ret != -ENOMEM) {
			LTTNG_THROW_POSIX(
				fmt::format(
					"Failed to reclaim reader sub-buffer for stream: channel_name=`{}`, stream_key={}",
					stream.chan->name,
					stream.key),
				errno);
		}

		const auto exchg_ret =
			lttng_ust_ctl_try_exchange_subbuf(stream.ustream, subbuf_position);
		if (exchg_ret == -ENOENT) {
			/* The sub-buffer is now in use, skip it. */
			DBG_FMT("Sub-buffer is in use, skipping: channel_name=`{}`, stream_key={}, subbuf_position={}",
				stream.chan->name,
				stream.key,
				subbuf_position);
			continue;
		} else if (exchg_ret < 0) {
			LTTNG_THROW_ERROR(fmt::format(
				"Failed to exchange sub-buffer for stream: channel_name=`{}`, stream_key={}, subbuf_position={}, error={}",
				stream.chan->name,
				stream.key,
				subbuf_position,
				exchg_ret));
		}

		DBG_FMT("Reclaimed sub-buffer: channel_name=`{}`, stream_key={}, subbuf_position={}",
			stream.chan->name,
			stream.key,
			subbuf_position);
		reclaimed_subbuf_count++;
	}

	if (reclaimed_subbuf_count != 0) {
		/*
		 * The reader sub-buffer was exchanged with the iterator's current sub-buffer,
		 * so it is "allocated". Reclaim it to reduce the memory footprint.
		 */
		const auto reclaim_ret = lttng_ust_ctl_reclaim_reader_subbuf(stream.ustream);
		if (reclaim_ret < 0 && reclaim_ret != -ENOMEM) {
			LTTNG_THROW_POSIX(
				fmt::format(
					"Failed to reclaim reader sub-buffer for stream: channel_name=`{}`, stream_key={}",
					stream.chan->name,
					stream.key),
				errno);
		}
	}

	return reclaimed_subbuf_count * stream.max_sb_size;
}

static void
lttng_ustconsumer_reclaim_channels_memory(int socket,
					  std::uint64_t channel_count,
					  nonstd::optional<std::chrono::microseconds> age_limit,
					  bool require_consumed)
{
	std::vector<std::uint64_t> channel_keys;

	channel_keys.resize(channel_count);
	const auto channel_key_payload_size =
		channel_count * sizeof(decltype(channel_keys)::value_type);
	const auto recv_ret =
		lttcomm_recv_unix_sock(socket, channel_keys.data(), channel_key_payload_size);

	if (recv_ret != channel_key_payload_size) {
		LTTNG_THROW_POSIX("Failed to receive channel keys from session daemon", errno);
	}

	/*
	 * The reply has the following structure:
	 * - generic reply header (announcing the command status and payload size)
	 * - command-specific reply header (announcing the number of stream memory reclamation
	 * entries)
	 * - stream memory reclamation entries (contains channel key, amount reclaimed)
	 */
	std::vector<std::uint8_t> reply_payload;
	lttcomm_consumer_status_msg generic_reply_header = {};
	lttcomm_consumer_channel_memory_reclamation_reply_header command_specific_reply_header = {};

	/*
	 * The two headers are inserted in the payload to "reserve" space for them before the
	 * actual stream memory usage entries are inserted.
	 *
	 * Their content will be overwritten later with the actual command status and
	 * the number of stream memory usage entries.
	 */
	reply_payload.resize(sizeof(generic_reply_header) + sizeof(command_specific_reply_header));

	std::size_t total_stream_count = 0;
	for (const auto channel_key : channel_keys) {
		auto *channel = consumer_find_channel(channel_key);
		if (!channel) {
			LTTNG_THROW_CHANNEL_NOT_FOUND_BY_KEY_ERROR(channel_key);
		}

		const lttng::pthread::lock_guard channel_lock(channel->lock);
		DBG_FMT("Reclaiming memory channel: key={}, channel_name=`{}`",
			channel_key,
			channel->name);

		if (channel->monitor) {
			const lttng::urcu::read_lock_guard read_lock;
			for (auto *stream : lttng::urcu::lfht_filtered_iteration_adapter<
				     lttng_consumer_stream,
				     decltype(lttng_consumer_stream::node_channel_id),
				     &lttng_consumer_stream::node_channel_id,
				     std::uint64_t>(
				     *the_consumer_data.stream_per_chan_id_ht->ht,
				     &channel->key,
				     the_consumer_data.stream_per_chan_id_ht->hash_fct(
					     &channel->key, lttng_ht_seed),
				     the_consumer_data.stream_per_chan_id_ht->match_fct)) {
				const lttng::pthread::lock_guard stream_lock(stream->lock);

				if (cds_lfht_is_node_deleted(&stream->node.node)) {
					continue;
				}

				const auto bytes_reclaimed =
					reclaim_stream_memory(*stream, age_limit, require_consumed);

				const lttcomm_stream_memory_reclamation_result
					stream_reclamation_result = {
						.channel_key = channel_key,
						.bytes_reclaimed = bytes_reclaimed,
					};

				reply_payload.insert(reply_payload.end(),
						     reinterpret_cast<const std::uint8_t *>(
							     &stream_reclamation_result),
						     reinterpret_cast<const std::uint8_t *>(
							     &stream_reclamation_result) +
							     sizeof(stream_reclamation_result));

				total_stream_count++;
			}
		} else {
			for (auto *stream :
			     lttng::urcu::list_iteration_adapter<lttng_consumer_stream,
								 &lttng_consumer_stream::send_node>(
				     channel->streams.head)) {
				const lttng::pthread::lock_guard stream_lock(stream->lock);

				const auto bytes_reclaimed =
					reclaim_stream_memory(*stream, age_limit, require_consumed);

				const lttcomm_stream_memory_reclamation_result
					stream_reclamation_result = {
						.channel_key = channel_key,
						.bytes_reclaimed = bytes_reclaimed,
					};

				reply_payload.insert(reply_payload.end(),
						     reinterpret_cast<const std::uint8_t *>(
							     &stream_reclamation_result),
						     reinterpret_cast<const std::uint8_t *>(
							     &stream_reclamation_result) +
							     sizeof(stream_reclamation_result));

				total_stream_count++;
			}
		}
	}

	/* Update the payload headers. */
	generic_reply_header.ret_code = LTTCOMM_CONSUMERD_SUCCESS;
	generic_reply_header.payload_size = reply_payload.size() - sizeof(generic_reply_header);
	std::memcpy(reply_payload.data(), &generic_reply_header, sizeof(generic_reply_header));

	command_specific_reply_header.count = total_stream_count;
	std::memcpy(reply_payload.data() + sizeof(generic_reply_header),
		    &command_specific_reply_header,
		    sizeof(command_specific_reply_header));

	const auto send_ret =
		lttcomm_send_unix_sock(socket, reply_payload.data(), reply_payload.size());
	if (send_ret != reply_payload.size()) {
		LTTNG_THROW_POSIX(
			fmt::format(
				"Failed to send channel memory reclamation reply to session daemon: payload_size={} bytes",
				reply_payload.size()),
			errno);
	}
}

/*
 * Receive the metadata updates from the sessiond. Supports receiving
 * overlapping metadata, but is needs to always belong to a contiguous
 * range starting from 0.
 * Be careful about the locks held when calling this function: it needs
 * the metadata cache flush to concurrently progress in order to
 * complete.
 */
int lttng_ustconsumer_recv_metadata(int sock,
				    uint64_t key,
				    uint64_t offset,
				    uint64_t len,
				    uint64_t version,
				    struct lttng_consumer_channel *channel,
				    bool invoked_by_timer,
				    int wait)
{
	int ret, ret_code = LTTCOMM_CONSUMERD_SUCCESS;
	char *metadata_str;
	enum consumer_metadata_cache_write_status cache_write_status;

	DBG("UST consumer push metadata key %" PRIu64 " of len %" PRIu64, key, len);

	metadata_str = calloc<char>(len);
	if (!metadata_str) {
		PERROR("zmalloc metadata string");
		ret_code = LTTCOMM_CONSUMERD_ENOMEM;
		goto end;
	}

	health_code_update();

	/* Receive metadata string. */
	ret = lttcomm_recv_unix_sock(sock, metadata_str, len);
	if (ret < 0) {
		/* Session daemon is dead so return gracefully. */
		ret_code = ret;
		goto end_free;
	}

	health_code_update();

	pthread_mutex_lock(&channel->metadata_cache->lock);
	cache_write_status = consumer_metadata_cache_write(
		channel->metadata_cache, offset, len, version, metadata_str);
	pthread_mutex_unlock(&channel->metadata_cache->lock);
	switch (cache_write_status) {
	case CONSUMER_METADATA_CACHE_WRITE_STATUS_NO_CHANGE:
		/*
		 * The write entirely overlapped with existing contents of the
		 * same metadata version (same content); there is nothing to do.
		 */
		break;
	case CONSUMER_METADATA_CACHE_WRITE_STATUS_INVALIDATED:
		/*
		 * The metadata cache was invalidated (previously pushed
		 * content has been overwritten). Reset the stream's consumed
		 * metadata position to ensure the metadata poll thread consumes
		 * the whole cache.
		 */

		/*
		 * channel::metadata_stream can be null when the metadata
		 * channel is under a snapshot session type. No need to update
		 * the stream position in that scenario.
		 */
		if (channel->metadata_stream != nullptr) {
			pthread_mutex_lock(&channel->metadata_stream->lock);
			metadata_stream_reset_cache_consumed_position(channel->metadata_stream);
			pthread_mutex_unlock(&channel->metadata_stream->lock);
		} else {
			/* Validate we are in snapshot mode. */
			LTTNG_ASSERT(!channel->monitor);
		}
		/* Fall-through. */
	case CONSUMER_METADATA_CACHE_WRITE_STATUS_APPENDED_CONTENT:
		/*
		 * In both cases, the metadata poll thread has new data to
		 * consume.
		 */
		ret = consumer_metadata_wakeup_pipe(channel);
		if (ret) {
			ret_code = LTTCOMM_CONSUMERD_ERROR_METADATA;
			goto end_free;
		}
		break;
	case CONSUMER_METADATA_CACHE_WRITE_STATUS_ERROR:
		/* Unable to handle metadata. Notify session daemon. */
		ret_code = LTTCOMM_CONSUMERD_ERROR_METADATA;
		/*
		 * Skip metadata flush on write error since the offset and len might
		 * not have been updated which could create an infinite loop below when
		 * waiting for the metadata cache to be flushed.
		 */
		goto end_free;
	default:
		abort();
	}

	if (!wait) {
		goto end_free;
	}

	consumer_wait_metadata_cache_flushed(channel, offset + len, invoked_by_timer);

end_free:
	free(metadata_str);
end:
	return ret_code;
}

/*
 * Receive command from session daemon and process it.
 *
 * Return 1 on success else a negative value or 0.
 */
int lttng_ustconsumer_recv_cmd(struct lttng_consumer_local_data *ctx,
			       int sock,
			       struct pollfd *consumer_sockpoll)
{
	int ret_func;
	enum lttcomm_return_code ret_code = LTTCOMM_CONSUMERD_SUCCESS;
	struct lttcomm_consumer_msg msg;
	struct lttng_consumer_channel *channel = nullptr;

	health_code_update();

	{
		ssize_t ret_recv;

		ret_recv = lttcomm_recv_unix_sock(sock, &msg, sizeof(msg));
		if (ret_recv != sizeof(msg)) {
			DBG("Consumer received unexpected message size %zd (expects %zu)",
			    ret_recv,
			    sizeof(msg));
			/*
			 * The ret value might 0 meaning an orderly shutdown but this is ok
			 * since the caller handles this.
			 */
			if (ret_recv > 0) {
				lttng_consumer_send_error(ctx->consumer_error_socket,
							  LTTCOMM_CONSUMERD_ERROR_RECV_CMD);
				ret_recv = -1;
			}
			return ret_recv;
		}
	}

	health_code_update();

	/* deprecated */
	LTTNG_ASSERT(msg.cmd_type != LTTNG_CONSUMER_STOP);

	health_code_update();

	/* relayd needs RCU read-side lock */
	const lttng::urcu::read_lock_guard read_lock;

	switch (msg.cmd_type) {
	case LTTNG_CONSUMER_ADD_RELAYD_SOCKET:
	{
		const uint32_t major = msg.u.relayd_sock.major;
		const uint32_t minor = msg.u.relayd_sock.minor;
		const lttcomm_sock_proto protocol =
			(enum lttcomm_sock_proto) msg.u.relayd_sock.relayd_socket_protocol;

		/* Session daemon status message are handled in the following call. */
		consumer_add_relayd_socket(msg.u.relayd_sock.net_index,
					   msg.u.relayd_sock.type,
					   ctx,
					   sock,
					   consumer_sockpoll,
					   msg.u.relayd_sock.session_id,
					   msg.u.relayd_sock.relayd_session_id,
					   major,
					   minor,
					   protocol);
		goto end_nosignal;
	}
	case LTTNG_CONSUMER_DESTROY_RELAYD:
	{
		const uint64_t index = msg.u.destroy_relayd.net_seq_idx;
		struct consumer_relayd_sock_pair *relayd;

		DBG("UST consumer destroying relayd %" PRIu64, index);

		/* Get relayd reference if exists. */
		relayd = consumer_find_relayd(index);
		if (relayd == nullptr) {
			DBG("Unable to find relayd %" PRIu64, index);
			ret_code = LTTCOMM_CONSUMERD_RELAYD_FAIL;
		}

		/*
		 * Each relayd socket pair has a refcount of stream attached to it
		 * which tells if the relayd is still active or not depending on the
		 * refcount value.
		 *
		 * This will set the destroy flag of the relayd object and destroy it
		 * if the refcount reaches zero when called.
		 *
		 * The destroy can happen either here or when a stream fd hangs up.
		 */
		if (relayd) {
			consumer_flag_relayd_for_destroy(relayd);
		}

		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_UPDATE_STREAM:
	{
		return -ENOSYS;
	}
	case LTTNG_CONSUMER_DATA_PENDING:
	{
		int is_data_pending;
		ssize_t ret_send;
		const uint64_t id = msg.u.data_pending.session_id;

		DBG("UST consumer data pending command for id %" PRIu64, id);

		is_data_pending = consumer_data_pending(id);

		/* Send back returned value to session daemon */
		ret_send = lttcomm_send_unix_sock(sock, &is_data_pending, sizeof(is_data_pending));
		if (ret_send < 0) {
			DBG("Error when sending the data pending ret code: %zd", ret_send);
			goto error_fatal;
		}

		/*
		 * No need to send back a status message since the data pending
		 * returned value is the response.
		 */
		break;
	}
	case LTTNG_CONSUMER_ASK_CHANNEL_CREATION:
	{
		int ret_ask_channel, ret_add_channel, ret_send;
		struct lttng_ust_ctl_consumer_channel_attr attr = {};
		const uint64_t chunk_id = msg.u.ask_channel.chunk_id.value;
		const struct lttng_credentials buffer_credentials = {
			.uid = LTTNG_OPTIONAL_INIT_VALUE(msg.u.ask_channel.buffer_credentials.uid),
			.gid = LTTNG_OPTIONAL_INIT_VALUE(msg.u.ask_channel.buffer_credentials.gid),
		};

		/* Create a plain object and reserve a channel key. */
		channel = consumer_allocate_channel(
			msg.u.ask_channel.key,
			msg.u.ask_channel.session_id,
			msg.u.ask_channel.chunk_id.is_set ? &chunk_id : nullptr,
			msg.u.ask_channel.pathname,
			msg.u.ask_channel.name,
			msg.u.ask_channel.relayd_id,
			(enum lttng_event_output) msg.u.ask_channel.output,
			msg.u.ask_channel.tracefile_size,
			msg.u.ask_channel.tracefile_count,
			msg.u.ask_channel.num_subbuf,
			msg.u.ask_channel.session_id_per_pid,
			msg.u.ask_channel.monitor,
			msg.u.ask_channel.live_timer_interval,
			msg.u.ask_channel.is_live,
			msg.u.ask_channel.root_shm_path,
			msg.u.ask_channel.shm_path);
		if (!channel) {
			goto end_channel_error;
		}

		LTTNG_OPTIONAL_SET(&channel->buffer_credentials, buffer_credentials);

		/*
		 * Assign UST application UID to the channel. This value is ignored for
		 * per PID buffers. This is specific to UST thus setting this after the
		 * allocation.
		 */
		channel->ust_app_uid = msg.u.ask_channel.ust_app_uid;

		/* Build channel attributes from received message. */
		attr.subbuf_size = msg.u.ask_channel.subbuf_size;
		attr.num_subbuf = msg.u.ask_channel.num_subbuf;
		attr.overwrite = msg.u.ask_channel.overwrite;
		attr.switch_timer_interval = msg.u.ask_channel.switch_timer_interval;
		attr.read_timer_interval = msg.u.ask_channel.read_timer_interval;
		attr.chan_id = msg.u.ask_channel.chan_id;
		memcpy(attr.uuid, msg.u.ask_channel.uuid, sizeof(attr.uuid));
		attr.blocking_timeout = msg.u.ask_channel.blocking_timeout;
		attr.owner_id = LTTNG_UST_ABI_OWNER_ID_CONSUMER;
		attr.preallocate_backing = true;

		/* Match channel buffer type to the UST abi. */
		switch (msg.u.ask_channel.output) {
		case LTTNG_EVENT_MMAP:
		default:
			attr.output = LTTNG_UST_ABI_MMAP;
			break;
		}

		/* Translate and save channel type. */
		switch (msg.u.ask_channel.type) {
		case LTTNG_UST_ABI_CHAN_PER_CPU:
			/* fall-through */
		case LTTNG_UST_ABI_CHAN_PER_CHANNEL:

			if (msg.u.ask_channel.type == LTTNG_UST_ABI_CHAN_PER_CPU) {
				channel->type = CONSUMER_CHANNEL_TYPE_DATA_PER_CPU;
				attr.type = LTTNG_UST_ABI_CHAN_PER_CPU;
			} else {
				channel->type = CONSUMER_CHANNEL_TYPE_DATA_PER_CHANNEL;
				attr.type = LTTNG_UST_ABI_CHAN_PER_CHANNEL;
			}

			/*
			 * Set refcount to 1 for owner. Below, we will
			 * pass ownership to the
			 * consumer_thread_channel_poll() thread.
			 */
			channel->refcount = 1;
			break;
		case LTTNG_UST_ABI_CHAN_METADATA:
			channel->type = CONSUMER_CHANNEL_TYPE_METADATA;
			attr.type = LTTNG_UST_ABI_CHAN_METADATA;
			break;
		default:
			abort();
			goto error_fatal;
		};

		health_code_update();

		ret_ask_channel = ask_channel(ctx, channel, &attr);
		if (ret_ask_channel < 0) {
			goto end_channel_error;
		}

		if (msg.u.ask_channel.type == LTTNG_UST_ABI_CHAN_METADATA) {
			int ret_allocate;

			ret_allocate = consumer_metadata_cache_allocate(channel);
			if (ret_allocate < 0) {
				ERR("Allocating metadata cache");
				goto end_channel_error;
			}

			consumer_timer_switch_start(channel,
						    attr.switch_timer_interval,
						    ctx->metadata_socket,
						    ctx->consumer_error_socket,
						    ctx->timer_task_scheduler);
		} else {
			int monitor_start_ret;

			consumer_timer_live_start(channel,
						  msg.u.ask_channel.live_timer_interval,
						  ctx->timer_task_scheduler);
			monitor_start_ret = consumer_timer_monitor_start(
				channel,
				msg.u.ask_channel.monitor_timer_interval,
				ctx->timer_task_scheduler);
			if (monitor_start_ret < 0) {
				ERR("Starting channel monitoring timer failed");
				goto end_channel_error;
			}

			if (msg.u.ask_channel.watchdog_timer_interval.is_set) {
				int stall_watchdog_start_ret = consumer_timer_stall_watchdog_start(
					channel,
					ctx->consumer_error_socket,
					LTTNG_OPTIONAL_GET(
						msg.u.ask_channel.watchdog_timer_interval),
					ctx->timer_task_scheduler);

				if (stall_watchdog_start_ret < 0) {
					ERR("Failed to start buffer-stall watchdog timer of channel: "
					    "session_id=%" PRIu64 ", channel_name=`%s`",
					    msg.u.ask_channel.session_id,
					    msg.u.ask_channel.name);
					goto end_channel_error;
				}
			}
		}

		health_code_update();

		/*
		 * Add the channel to the internal state AFTER all streams were created
		 * and successfully sent to session daemon. This way, all streams must
		 * be ready before this channel is visible to the threads.
		 * If add_channel succeeds, ownership of the channel is
		 * passed to consumer_thread_channel_poll().
		 */
		ret_add_channel = add_channel(channel, ctx);
		if (ret_add_channel < 0) {
			if (msg.u.ask_channel.type == LTTNG_UST_ABI_CHAN_METADATA) {
				if (channel->metadata_switch_timer_task) {
					consumer_timer_switch_stop(channel);
				}
				consumer_metadata_cache_destroy(channel);
			}
			if (channel->live_timer_task) {
				consumer_timer_live_stop(channel);
			}
			if (channel->monitor_timer_task) {
				consumer_timer_monitor_stop(channel);
			}
			if (channel->stall_watchdog_timer_task) {
				consumer_timer_stall_watchdog_stop(channel);
			}

			goto end_channel_error;
		}

		health_code_update();

		/*
		 * Channel and streams are now created. Inform the session daemon that
		 * everything went well and should wait to receive the channel and
		 * streams with ustctl API.
		 */
		ret_send = consumer_send_status_channel(sock, channel);
		if (ret_send < 0) {
			/*
			 * There is probably a problem on the socket.
			 */
			goto error_fatal;
		}

		break;
	}
	case LTTNG_CONSUMER_GET_CHANNEL:
	{
		int ret, relayd_err = 0;
		const uint64_t key = msg.u.get_channel.key;
		struct lttng_consumer_channel *found_channel;

		const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);

		found_channel = consumer_find_channel(key);
		if (!found_channel) {
			ERR("UST consumer get channel key %" PRIu64 " not found", key);
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
		} else {
			health_code_update();
			const lttng::pthread::lock_guard channel_lock(found_channel->lock);
			const lttng::pthread::lock_guard channel_timer_lock(
				found_channel->timer_lock);

			/* Send the channel to sessiond (and relayd, if applicable). */
			ret = send_channel_to_sessiond_and_relayd(
				sock, found_channel, ctx, &relayd_err);
			if (ret < 0) {
				if (relayd_err) {
					/*
					 * We were unable to send to the relayd the stream so avoid
					 * sending back a fatal error to the thread since this is OK
					 * and the consumer can continue its work. The above call
					 * has sent the error status message to the sessiond.
					 */
					goto end_get_channel_nosignal;
				}
				/*
				 * The communicaton was broken hence there is a bad state between
				 * the consumer and sessiond so stop everything.
				 */
				goto error_get_channel_fatal;
			}

			health_code_update();

			/*
			 * In no monitor mode, the streams ownership is kept inside the channel
			 * so don't send them to the data thread.
			 */
			if (!found_channel->monitor) {
				goto end_get_channel;
			}

			ret = send_streams_to_thread(found_channel, ctx);
			if (ret < 0) {
				/*
				 * If we are unable to send the stream to the thread, there is
				 * a big problem so just stop everything.
				 */
				goto error_get_channel_fatal;
			}
			/* List MUST be empty after or else it could be reused. */
			LTTNG_ASSERT(cds_list_empty(&found_channel->streams.head));
		}

	end_get_channel:
		goto end_msg_sessiond;
	error_get_channel_fatal:
		goto error_fatal;
	end_get_channel_nosignal:
		goto end_nosignal;
	}
	case LTTNG_CONSUMER_DESTROY_CHANNEL:
	{
		const uint64_t key = msg.u.destroy_channel.key;

		/*
		 * Only called if streams have not been sent to stream
		 * manager thread. However, channel has been sent to
		 * channel manager thread.
		 */
		notify_thread_del_channel(ctx, key);
		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_CLOSE_METADATA:
	{
		int ret;

		ret = close_metadata(msg.u.close_metadata.key);
		if (ret != 0) {
			ret_code = (lttcomm_return_code) ret;
		}

		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_FLUSH_CHANNEL:
	{
		int ret;

		ret = flush_channel(msg.u.flush_channel.key);
		if (ret != 0) {
			ret_code = (lttcomm_return_code) ret;
		}

		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_CLEAR_QUIESCENT_CHANNEL:
	{
		int ret;

		ret = clear_quiescent_channel(msg.u.clear_quiescent_channel.key);
		if (ret != 0) {
			ret_code = (lttcomm_return_code) ret;
		}

		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_PUSH_METADATA:
	{
		int ret;
		const uint64_t len = msg.u.push_metadata.len;
		const uint64_t key = msg.u.push_metadata.key;
		const uint64_t offset = msg.u.push_metadata.target_offset;
		const uint64_t version = msg.u.push_metadata.version;
		struct lttng_consumer_channel *found_channel;

		DBG("UST consumer push metadata key %" PRIu64 " of len %" PRIu64, key, len);

		found_channel = consumer_find_channel(key);
		if (!found_channel) {
			/*
			 * This is possible if the metadata creation on the consumer side
			 * is in flight vis-a-vis a concurrent push metadata from the
			 * session daemon.  Simply return that the channel failed and the
			 * session daemon will handle that message correctly considering
			 * that this race is acceptable thus the DBG() statement here.
			 */
			DBG("UST consumer push metadata %" PRIu64 " not found", key);
			ret_code = LTTCOMM_CONSUMERD_CHANNEL_FAIL;
			goto end_push_metadata_msg_sessiond;
		}

		health_code_update();

		if (!len) {
			/*
			 * There is nothing to receive. We have simply
			 * checked whether the channel can be found.
			 */
			ret_code = LTTCOMM_CONSUMERD_SUCCESS;
			goto end_push_metadata_msg_sessiond;
		}

		/* Tell session daemon we are ready to receive the metadata. */
		ret = consumer_send_status_msg(sock, LTTCOMM_CONSUMERD_SUCCESS);
		if (ret < 0) {
			/* Somehow, the session daemon is not responding anymore. */
			goto error_push_metadata_fatal;
		}

		health_code_update();

		/* Wait for more data. */
		health_poll_entry();
		ret = lttng_consumer_poll_socket(consumer_sockpoll);
		health_poll_exit();
		if (ret) {
			goto error_push_metadata_fatal;
		}

		health_code_update();

		ret = lttng_ustconsumer_recv_metadata(
			sock, key, offset, len, version, found_channel, false, 1);
		if (ret < 0) {
			/* error receiving from sessiond */
			goto error_push_metadata_fatal;
		} else {
			ret_code = (lttcomm_return_code) ret;
			goto end_push_metadata_msg_sessiond;
		}
	end_push_metadata_msg_sessiond:
		goto end_msg_sessiond;
	error_push_metadata_fatal:
		goto error_fatal;
	}
	case LTTNG_CONSUMER_SETUP_METADATA:
	{
		int ret;

		ret = setup_metadata(ctx, msg.u.setup_metadata.key);
		if (ret) {
			ret_code = (lttcomm_return_code) ret;
		}
		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_SNAPSHOT_CHANNEL:
	{
		const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);
		struct lttng_consumer_channel *found_channel;
		const uint64_t key = msg.u.snapshot_channel.key;
		int ret_send;

		found_channel = consumer_find_channel(key);
		if (!found_channel) {
			DBG("UST snapshot channel not found for key %" PRIu64, key);
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
		} else {
			if (msg.u.snapshot_channel.metadata) {
				int ret_snapshot;

				ret_snapshot = snapshot_metadata(found_channel,
								 key,
								 msg.u.snapshot_channel.pathname,
								 msg.u.snapshot_channel.relayd_id,
								 ctx);
				if (ret_snapshot < 0) {
					ERR("Snapshot metadata failed");
					ret_code = LTTCOMM_CONSUMERD_SNAPSHOT_FAILED;
				}
			} else {
				int ret_snapshot;

				ret_snapshot = snapshot_channel(
					found_channel,
					key,
					msg.u.snapshot_channel.pathname,
					msg.u.snapshot_channel.relayd_id,
					msg.u.snapshot_channel.nb_packets_per_stream,
					ctx);
				if (ret_snapshot < 0) {
					ERR("Snapshot channel failed");
					ret_code = LTTCOMM_CONSUMERD_SNAPSHOT_FAILED;
				}
			}
		}
		health_code_update();
		ret_send = consumer_send_status_msg(sock, ret_code);
		if (ret_send < 0) {
			/* Somehow, the session daemon is not responding anymore. */
			goto end_nosignal;
		}
		health_code_update();
		break;
	}
	case LTTNG_CONSUMER_DISCARDED_EVENTS:
	{
		int ret = 0;
		uint64_t discarded_events;
		const auto id = msg.u.discarded_events.session_id;
		const auto key = msg.u.discarded_events.channel_key;

		DBG("UST consumer discarded events command for session id %" PRIu64, id);
		{
			const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);
			const auto ht = the_consumer_data.stream_list_ht;

			/*
			 * We only need a reference to the channel, but they are not
			 * directly indexed, so we just use the first matching stream
			 * to extract the information we need, we default to 0 if not
			 * found (no events are dropped if the channel is not yet in
			 * use).
			 */
			discarded_events = 0;
			for (auto *stream : lttng::urcu::lfht_filtered_iteration_adapter<
				     lttng_consumer_stream,
				     decltype(lttng_consumer_stream::node_channel_id),
				     &lttng_consumer_stream::node_session_id,
				     std::uint64_t>(*ht->ht,
						    &id,
						    ht->hash_fct(&id, lttng_ht_seed),
						    ht->match_fct)) {
				if (stream->chan->key == key) {
					discarded_events = stream->chan->discarded_events;
					break;
				}
			}
		}

		DBG("UST consumer discarded events command for session id %" PRIu64
		    ", channel key %" PRIu64,
		    id,
		    key);

		health_code_update();

		/* Send back returned value to session daemon */
		ret = lttcomm_send_unix_sock(sock, &discarded_events, sizeof(discarded_events));
		if (ret < 0) {
			PERROR("send discarded events");
			goto error_fatal;
		}

		break;
	}
	case LTTNG_CONSUMER_LOST_PACKETS:
	{
		int ret;
		uint64_t lost_packets;
		const auto id = msg.u.lost_packets.session_id;
		const auto key = msg.u.lost_packets.channel_key;

		DBG("UST consumer lost packets command for session id %" PRIu64, id);
		{
			const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);
			const auto ht = the_consumer_data.stream_list_ht;

			/*
			 * We only need a reference to the channel, but they are not
			 * directly indexed, so we just use the first matching stream
			 * to extract the information we need, we default to 0 if not
			 * found (no packets lost if the channel is not yet in use).
			 */
			lost_packets = 0;
			for (auto *stream : lttng::urcu::lfht_filtered_iteration_adapter<
				     lttng_consumer_stream,
				     decltype(lttng_consumer_stream::node_session_id),
				     &lttng_consumer_stream::node_session_id,
				     std::uint64_t>(*ht->ht,
						    &id,
						    ht->hash_fct(&id, lttng_ht_seed),
						    ht->match_fct)) {
				if (stream->chan->key == key) {
					lost_packets = stream->chan->lost_packets;
					break;
				}
			}
		}

		DBG("UST consumer lost packets command for session id %" PRIu64
		    ", channel key %" PRIu64,
		    id,
		    key);

		health_code_update();

		/* Send back returned value to session daemon */
		ret = lttcomm_send_unix_sock(sock, &lost_packets, sizeof(lost_packets));
		if (ret < 0) {
			PERROR("send lost packets");
			goto error_fatal;
		}

		break;
	}
	case LTTNG_CONSUMER_SET_CHANNEL_MONITOR_PIPE:
	{
		int channel_monitor_pipe, ret_send, ret_set_channel_monitor_pipe;
		ssize_t ret_recv;

		ret_code = LTTCOMM_CONSUMERD_SUCCESS;
		/* Successfully received the command's type. */
		ret_send = consumer_send_status_msg(sock, ret_code);
		if (ret_send < 0) {
			goto error_fatal;
		}

		ret_recv = lttcomm_recv_fds_unix_sock(sock, &channel_monitor_pipe, 1);
		if (ret_recv != sizeof(channel_monitor_pipe)) {
			ERR("Failed to receive channel monitor pipe");
			goto error_fatal;
		}

		DBG("Received channel monitor pipe (%d)", channel_monitor_pipe);
		ret_set_channel_monitor_pipe =
			consumer_timer_thread_set_channel_monitor_pipe(channel_monitor_pipe);
		if (!ret_set_channel_monitor_pipe) {
			int flags;
			int ret_fcntl;

			ret_code = LTTCOMM_CONSUMERD_SUCCESS;
			/* Set the pipe as non-blocking. */
			ret_fcntl = fcntl(channel_monitor_pipe, F_GETFL, 0);
			if (ret_fcntl == -1) {
				PERROR("fcntl get flags of the channel monitoring pipe");
				goto error_fatal;
			}
			flags = ret_fcntl;

			ret_fcntl = fcntl(channel_monitor_pipe, F_SETFL, flags | O_NONBLOCK);
			if (ret_fcntl == -1) {
				PERROR("fcntl set O_NONBLOCK flag of the channel monitoring pipe");
				goto error_fatal;
			}
			DBG("Channel monitor pipe set as non-blocking");
		} else {
			ret_code = LTTCOMM_CONSUMERD_ALREADY_SET;
		}
		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_ROTATE_CHANNEL:
	{
		struct lttng_consumer_channel *found_channel;
		const uint64_t key = msg.u.rotate_channel.key;
		int ret_send_status;

		const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);

		found_channel = consumer_find_channel(key);
		if (!found_channel) {
			DBG("Channel %" PRIu64 " not found", key);
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
		} else {
			int rotate_channel;

			/*
			 * Sample the rotate position of all the streams in
			 * this channel.
			 */
			rotate_channel = lttng_consumer_rotate_channel(
				found_channel, key, msg.u.rotate_channel.relayd_id);
			if (rotate_channel < 0) {
				ERR("Rotate channel failed");
				ret_code = LTTCOMM_CONSUMERD_ROTATION_FAIL;
			}

			health_code_update();
		}

		ret_send_status = consumer_send_status_msg(sock, ret_code);
		if (ret_send_status < 0) {
			/* Somehow, the session daemon is not responding anymore. */
			goto end_rotate_channel_nosignal;
		}

		/*
		 * Rotate the streams that are ready right now.
		 * FIXME: this is a second consecutive iteration over the
		 * streams in a channel, there is probably a better way to
		 * handle this, but it needs to be after the
		 * consumer_send_status_msg() call.
		 */
		if (found_channel) {
			int ret_rotate_read_streams;

			ret_rotate_read_streams =
				lttng_consumer_rotate_ready_streams(found_channel, key);
			if (ret_rotate_read_streams < 0) {
				ERR("Rotate channel failed");
			}
		}
		break;
	end_rotate_channel_nosignal:
		goto end_nosignal;
	}
	case LTTNG_CONSUMER_CLEAR_CHANNEL:
	{
		struct lttng_consumer_channel *found_channel;
		const uint64_t key = msg.u.clear_channel.key;
		int ret_send_status;

		const lttng::pthread::lock_guard global_lock(the_consumer_data.lock);

		found_channel = consumer_find_channel(key);
		if (!found_channel) {
			DBG("Channel %" PRIu64 " not found", key);
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
		} else {
			const lttng::pthread::lock_guard channel_lock(found_channel->lock);

			const auto ret_clear_channel = lttng_consumer_clear_channel(found_channel);
			if (ret_clear_channel) {
				ERR("Clear channel failed key %" PRIu64, key);
				ret_code = (lttcomm_return_code) ret_clear_channel;
			}

			health_code_update();
		}
		ret_send_status = consumer_send_status_msg(sock, ret_code);
		if (ret_send_status < 0) {
			/* Somehow, the session daemon is not responding anymore. */
			goto end_nosignal;
		}
		break;
	}
	case LTTNG_CONSUMER_INIT:
	{
		int ret_send_status;
		lttng_uuid sessiond_uuid;

		std::copy(std::begin(msg.u.init.sessiond_uuid),
			  std::end(msg.u.init.sessiond_uuid),
			  sessiond_uuid.begin());
		ret_code = lttng_consumer_init_command(ctx, sessiond_uuid);
		health_code_update();
		ret_send_status = consumer_send_status_msg(sock, ret_code);
		if (ret_send_status < 0) {
			/* Somehow, the session daemon is not responding anymore. */
			goto end_nosignal;
		}
		break;
	}
	case LTTNG_CONSUMER_CREATE_TRACE_CHUNK:
	{
		const struct lttng_credentials credentials = {
			.uid = LTTNG_OPTIONAL_INIT_VALUE(
				msg.u.create_trace_chunk.credentials.value.uid),
			.gid = LTTNG_OPTIONAL_INIT_VALUE(
				msg.u.create_trace_chunk.credentials.value.gid),
		};
		const bool is_local_trace = !msg.u.create_trace_chunk.relayd_id.is_set;
		const uint64_t relayd_id = msg.u.create_trace_chunk.relayd_id.value;
		const char *chunk_override_name = *msg.u.create_trace_chunk.override_name ?
			msg.u.create_trace_chunk.override_name :
			nullptr;
		struct lttng_directory_handle *chunk_directory_handle = nullptr;

		/*
		 * The session daemon will only provide a chunk directory file
		 * descriptor for local traces.
		 */
		if (is_local_trace) {
			int chunk_dirfd;
			int ret_send_status;
			ssize_t ret_recv;

			/* Acnowledge the reception of the command. */
			ret_send_status = consumer_send_status_msg(sock, LTTCOMM_CONSUMERD_SUCCESS);
			if (ret_send_status < 0) {
				/* Somehow, the session daemon is not responding anymore. */
				goto end_nosignal;
			}

			/*
			 * Receive trace chunk domain dirfd.
			 */
			ret_recv = lttcomm_recv_fds_unix_sock(sock, &chunk_dirfd, 1);
			if (ret_recv != sizeof(chunk_dirfd)) {
				ERR("Failed to receive trace chunk domain directory file descriptor");
				goto error_fatal;
			}

			DBG("Received trace chunk domain directory fd (%d)", chunk_dirfd);
			chunk_directory_handle =
				lttng_directory_handle_create_from_dirfd(chunk_dirfd);
			if (!chunk_directory_handle) {
				ERR("Failed to initialize chunk domain directory handle from directory file descriptor");
				if (close(chunk_dirfd)) {
					PERROR("Failed to close chunk directory file descriptor");
				}
				goto error_fatal;
			}
		}

		ret_code = lttng_consumer_create_trace_chunk(
			!is_local_trace ? &relayd_id : nullptr,
			msg.u.create_trace_chunk.session_id,
			msg.u.create_trace_chunk.chunk_id,
			(time_t) msg.u.create_trace_chunk.creation_timestamp,
			chunk_override_name,
			msg.u.create_trace_chunk.credentials.is_set ? &credentials : nullptr,
			chunk_directory_handle);
		lttng_directory_handle_put(chunk_directory_handle);
		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_CLOSE_TRACE_CHUNK:
	{
		enum lttng_trace_chunk_command_type close_command =
			(lttng_trace_chunk_command_type) msg.u.close_trace_chunk.close_command.value;
		const uint64_t relayd_id = msg.u.close_trace_chunk.relayd_id.value;
		struct lttcomm_consumer_close_trace_chunk_reply reply;
		char closed_trace_chunk_path[LTTNG_PATH_MAX] = {};
		int ret;

		ret_code = lttng_consumer_close_trace_chunk(
			msg.u.close_trace_chunk.relayd_id.is_set ? &relayd_id : nullptr,
			msg.u.close_trace_chunk.session_id,
			msg.u.close_trace_chunk.chunk_id,
			(time_t) msg.u.close_trace_chunk.close_timestamp,
			msg.u.close_trace_chunk.close_command.is_set ? &close_command : nullptr,
			closed_trace_chunk_path);
		reply.ret_code = ret_code;
		reply.path_length = strlen(closed_trace_chunk_path) + 1;
		ret = lttcomm_send_unix_sock(sock, &reply, sizeof(reply));
		if (ret != sizeof(reply)) {
			goto error_fatal;
		}
		ret = lttcomm_send_unix_sock(sock, closed_trace_chunk_path, reply.path_length);
		if (ret != reply.path_length) {
			goto error_fatal;
		}
		goto end_nosignal;
	}
	case LTTNG_CONSUMER_TRACE_CHUNK_EXISTS:
	{
		const uint64_t relayd_id = msg.u.trace_chunk_exists.relayd_id.value;

		ret_code = lttng_consumer_trace_chunk_exists(
			msg.u.trace_chunk_exists.relayd_id.is_set ? &relayd_id : nullptr,
			msg.u.trace_chunk_exists.session_id,
			msg.u.trace_chunk_exists.chunk_id);
		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_OPEN_CHANNEL_PACKETS:
	{
		const lttng::pthread::lock_guard consumer_data_lock(the_consumer_data.lock);
		const uint64_t key = msg.u.open_channel_packets.key;
		struct lttng_consumer_channel *found_channel = consumer_find_channel(key);

		if (found_channel) {
			pthread_mutex_lock(&found_channel->lock);
			ret_code = lttng_consumer_open_channel_packets(found_channel);
			pthread_mutex_unlock(&found_channel->lock);
		} else {
			/*
			 * The channel could have disappeared in per-pid
			 * buffering mode.
			 */
			DBG("Channel %" PRIu64 " not found", key);
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
		}

		health_code_update();
		goto end_msg_sessiond;
	}
	case LTTNG_CONSUMER_RECLAIM_SESSION_OWNER_ID:
	{
		const auto session_id = msg.u.reclaim_session_owner_id.session_id;
		const auto owner_id = msg.u.reclaim_session_owner_id.owner_id;

		DBG_FMT("Receiving payload of "
			"LTTNG_CONSUMER_RECLAIM_SESSION_OWNER_ID command: "
			"session_id={}, owner_id={}",
			session_id,
			owner_id);

		const auto pending_reclamations =
			lttng_ustconsumer_reclaim_session_owner_id(session_id, owner_id);

		health_code_update();

		const ssize_t ret_send = lttcomm_send_unix_sock(
			sock, &pending_reclamations, sizeof(pending_reclamations));

		health_code_update();

		if (ret_send != sizeof(pending_reclamations)) {
			ERR_FMT("Error while sending not pending reclamation channel count: "
				"session_id={}, owner_id={}, pending_reclamations={}",
				session_id,
				owner_id,
				pending_reclamations);
			goto error_fatal;
		}

		break;
	}
	case LTTNG_CONSUMER_GET_CHANNELS_MEMORY_USAGE:
	{
		try {
			health_code_update();
			lttng_ustconsumer_get_channels_memory_usage(
				sock, msg.u.get_channels_memory_usage.key_count);
		} catch (lttng::consumerd::exceptions::channel_not_found_error& ex) {
			ERR_FMT("Failed to get memory usage of channels: {}", ex.what());
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
			goto end_msg_sessiond;
		} catch (const std::exception& ex) {
			/* Generic error. */
			ERR_FMT("Failed to get memory usage of channels: {}", ex.what());
			ret_code = LTTCOMM_CONSUMERD_FATAL;
			goto end_msg_sessiond;
		}

		/*
		 * This command's return payload is sent directly to the session daemon
		 * and not through the consumer_send_status_msg() function. This is because the
		 * session daemon expects a payload with the memory usage memory usage of the
		 * channels and not a return code except in the case of errors.
		 */
		goto end_nosignal;
	}
	case LTTNG_CONSUMER_RECLAIM_CHANNELS_MEMORY:
	{
		try {
			health_code_update();
			nonstd::optional<std::chrono::microseconds> age_limit;

			if (msg.u.reclaim_channels_memory.age_limit_us.is_set) {
				age_limit = std::chrono::microseconds(
					msg.u.reclaim_channels_memory.age_limit_us.value);
			}

			lttng_ustconsumer_reclaim_channels_memory(
				sock,
				msg.u.reclaim_channels_memory.key_count,
				age_limit,
				msg.u.reclaim_channels_memory.require_consumed);
		} catch (lttng::consumerd::exceptions::channel_not_found_error& ex) {
			ERR_FMT("Failed to reclaim memory of channels: {}", ex.what());
			ret_code = LTTCOMM_CONSUMERD_CHAN_NOT_FOUND;
			goto end_msg_sessiond;
		} catch (const std::exception& ex) {
			/* Generic error. */
			ERR_FMT("Failed to reclaim memory of channels: {}", ex.what());
			ret_code = LTTCOMM_CONSUMERD_FATAL;
			goto end_msg_sessiond;
		}

		/*
		 * This command's return payload is sent directly to the session daemon
		 * and not through the consumer_send_status_msg() function. This is because the
		 * session daemon expects an amount of reclaimed memory on a per-channel basis and
		 * not just a return code.
		 */
		goto end_nosignal;
	}
	default:
		break;
	}

end_nosignal:
	/*
	 * Return 1 to indicate success since the 0 value can be a socket
	 * shutdown during the recv() or send() call.
	 */
	ret_func = 1;
	goto end;

end_msg_sessiond:
	/*
	 * The returned value here is not useful since either way we'll return 1 to
	 * the caller because the session daemon socket management is done
	 * elsewhere. Returning a negative code or 0 will shutdown the consumer.
	 */
	{
		int ret_send_status;

		ret_send_status = consumer_send_status_msg(sock, ret_code);
		if (ret_send_status < 0) {
			goto error_fatal;
		}
	}

	ret_func = 1;
	goto end;

end_channel_error:
	if (channel) {
		consumer_del_channel(channel);
	}
	/* We have to send a status channel message indicating an error. */
	{
		int ret_send_status;

		ret_send_status = consumer_send_status_channel(sock, nullptr);
		if (ret_send_status < 0) {
			/* Stop everything if session daemon can not be notified. */
			goto error_fatal;
		}
	}

	ret_func = 1;
	goto end;

error_fatal:
	/* This will issue a consumer stop. */
	ret_func = -1;
	goto end;

end:
	health_code_update();
	return ret_func;
}

/*
 * Take a snapshot for a specific stream.
 *
 * Returns 0 on success, < 0 on error
 */
int lttng_ustconsumer_take_snapshot(struct lttng_consumer_stream *stream)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	return lttng_ust_ctl_snapshot(stream->ustream);
}

/*
 * Sample consumed and produced positions for a specific stream.
 *
 * Returns 0 on success, < 0 on error.
 */
int lttng_ustconsumer_sample_snapshot_positions(struct lttng_consumer_stream *stream) noexcept
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	return lttng_ust_ctl_snapshot_sample_positions(stream->ustream);
}

/*
 * Get the produced position
 *
 * Returns 0 on success, < 0 on error
 */
int lttng_ustconsumer_get_produced_snapshot(struct lttng_consumer_stream *stream,
					    unsigned long *pos) noexcept
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);
	LTTNG_ASSERT(pos);

	return lttng_ust_ctl_snapshot_get_produced(stream->ustream, pos);
}

/*
 * Get the consumed position
 *
 * Returns 0 on success, < 0 on error
 */
int lttng_ustconsumer_get_consumed_snapshot(struct lttng_consumer_stream *stream,
					    unsigned long *pos) noexcept
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);
	LTTNG_ASSERT(pos);

	return lttng_ust_ctl_snapshot_get_consumed(stream->ustream, pos);
}

int lttng_ustconsumer_flush_buffer(struct lttng_consumer_stream *stream, int producer)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	return lttng_ust_ctl_flush_buffer(stream->ustream, producer);
}

int lttng_ustconsumer_flush_buffer_or_populate_packet(
	struct lttng_consumer_stream *stream,
	struct lttng_ust_ctl_consumer_packet *terminal_packet,
	bool *packet_populated,
	bool *flush_done)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);
	LTTNG_ASSERT(terminal_packet);
	LTTNG_ASSERT(packet_populated);

	return lttng_ust_ctl_flush_events_or_populate_packet(
		stream->ustream, terminal_packet, packet_populated, flush_done);
}

int lttng_ustconsumer_clear_buffer(struct lttng_consumer_stream *stream)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	return lttng_ust_ctl_clear_buffer(stream->ustream);
}

int lttng_ustconsumer_get_current_timestamp(struct lttng_consumer_stream *stream, uint64_t *ts)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);
	LTTNG_ASSERT(ts);

	return lttng_ust_ctl_get_current_timestamp(stream->ustream, ts);
}

int lttng_ustconsumer_get_sequence_number(struct lttng_consumer_stream *stream, uint64_t *seq)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);
	LTTNG_ASSERT(seq);

	return lttng_ust_ctl_get_sequence_number(stream->ustream, seq);
}

/*
 * Called when the stream signals the consumer that it has hung up.
 */
void lttng_ustconsumer_on_stream_hangup(struct lttng_consumer_stream *stream)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	pthread_mutex_lock(&stream->lock);
	if (!stream->quiescent) {
		if (lttng_ust_ctl_flush_buffer(stream->ustream, 0) < 0) {
			ERR("Failed to flush buffer on stream hang-up");
		} else {
			stream->quiescent = true;
		}
	}

	stream->hangup_flush_done = 1;
	pthread_mutex_unlock(&stream->lock);
}

void lttng_ustconsumer_del_channel(struct lttng_consumer_channel *chan)
{
	int i;

	LTTNG_ASSERT(chan);
	LTTNG_ASSERT(chan->uchan);
	LTTNG_ASSERT(chan->buffer_credentials.is_set);

	if (chan->metadata_switch_timer_task) {
		consumer_timer_switch_stop(chan);
	}
	for (i = 0; i < chan->nr_stream_fds; i++) {
		int ret;

		ret = close(chan->stream_fds[i]);
		if (ret) {
			PERROR("close");
		}
		if (chan->shm_path[0]) {
			char shm_path[PATH_MAX];

			ret = get_stream_shm_path(shm_path, chan->shm_path, i);
			if (ret) {
				ERR("Cannot get stream shm path");
			}
			ret = run_as_unlink(shm_path,
					    lttng_credentials_get_uid(LTTNG_OPTIONAL_GET_PTR(
						    chan->buffer_credentials)),
					    lttng_credentials_get_gid(LTTNG_OPTIONAL_GET_PTR(
						    chan->buffer_credentials)));
			if (ret) {
				PERROR("unlink %s", shm_path);
			}
		}
	}
}

void lttng_ustconsumer_free_channel(struct lttng_consumer_channel *chan)
{
	LTTNG_ASSERT(chan);
	LTTNG_ASSERT(chan->uchan);
	LTTNG_ASSERT(chan->buffer_credentials.is_set);

	consumer_metadata_cache_destroy(chan);
	lttng_ust_ctl_destroy_channel(chan->uchan);
	/* Try to rmdir all directories under shm_path root. */
	if (chan->root_shm_path[0]) {
		(void) run_as_rmdir_recursive(
			chan->root_shm_path,
			lttng_credentials_get_uid(LTTNG_OPTIONAL_GET_PTR(chan->buffer_credentials)),
			lttng_credentials_get_gid(LTTNG_OPTIONAL_GET_PTR(chan->buffer_credentials)),
			LTTNG_DIRECTORY_HANDLE_SKIP_NON_EMPTY_FLAG);
	}
	free(chan->stream_fds);
}

void lttng_ustconsumer_del_stream(struct lttng_consumer_stream *stream)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	if (stream->chan->metadata_switch_timer_task) {
		consumer_timer_switch_stop(stream->chan);
	}
	lttng_ust_ctl_destroy_stream(stream->ustream);
}

int lttng_ustconsumer_get_wakeup_fd(struct lttng_consumer_stream *stream)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	return lttng_ust_ctl_stream_get_wakeup_fd(stream->ustream);
}

int lttng_ustconsumer_close_wakeup_fd(struct lttng_consumer_stream *stream)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);

	return lttng_ust_ctl_stream_close_wakeup_fd(stream->ustream);
}

/*
 * Write up to one packet from the metadata cache to the channel.
 *
 * Returns the number of bytes pushed from the cache into the ring buffer, or a
 * negative value on error.
 */
static int commit_one_metadata_packet(struct lttng_consumer_stream *stream)
{
	ssize_t write_len;
	int ret;

	pthread_mutex_lock(&stream->chan->metadata_cache->lock);
	if (stream->chan->metadata_cache->contents.size == stream->ust_metadata_pushed) {
		/*
		 * In the context of a user space metadata channel, a
		 * change in version can be detected in two ways:
		 *   1) During the pre-consume of the `read_subbuffer` loop,
		 *   2) When populating the metadata ring buffer (i.e. here).
		 *
		 * This function is invoked when there is no metadata
		 * available in the ring-buffer. If all data was consumed
		 * up to the size of the metadata cache, there is no metadata
		 * to insert in the ring-buffer.
		 *
		 * However, the metadata version could still have changed (a
		 * regeneration without any new data will yield the same cache
		 * size).
		 *
		 * The cache's version is checked for a version change and the
		 * consumed position is reset if one occurred.
		 *
		 * This check is only necessary for the user space domain as
		 * it has to manage the cache explicitly. If this reset was not
		 * performed, no metadata would be consumed (and no reset would
		 * occur as part of the pre-consume) until the metadata size
		 * exceeded the cache size.
		 */
		if (stream->metadata_version != stream->chan->metadata_cache->version) {
			metadata_stream_reset_cache_consumed_position(stream);
			consumer_stream_metadata_set_version(stream,
							     stream->chan->metadata_cache->version);
		} else {
			ret = 0;
			goto end;
		}
	}

	write_len = lttng_ust_ctl_write_one_packet_to_channel(
		stream->chan->uchan,
		&stream->chan->metadata_cache->contents.data[stream->ust_metadata_pushed],
		stream->chan->metadata_cache->contents.size - stream->ust_metadata_pushed);
	LTTNG_ASSERT(write_len != 0);
	if (write_len < 0) {
		ERR("Writing one metadata packet");
		ret = write_len;
		goto end;
	}

	stream->ust_metadata_pushed += write_len;
	stream->chan->metadata_pushed_wait_queue.wake_all();

	LTTNG_ASSERT(stream->chan->metadata_cache->contents.size >= stream->ust_metadata_pushed);
	ret = write_len;

	/*
	 * Switch packet (but don't open the next one) on every commit of
	 * a metadata packet. Since the subbuffer is fully filled (with padding,
	 * if needed), the stream is "quiescent" after this commit.
	 */
	if (lttng_ust_ctl_flush_buffer(stream->ustream, 1)) {
		ERR("Failed to flush buffer while committing one metadata packet");
		ret = -EIO;
	} else {
		stream->quiescent = true;
	}
end:
	pthread_mutex_unlock(&stream->chan->metadata_cache->lock);
	return ret;
}

/*
 * Sync metadata meaning request them to the session daemon and snapshot to the
 * metadata thread can consumer them.
 *
 * Metadata stream lock is held here, but we need to release it when
 * interacting with sessiond, else we cause a deadlock with live
 * awaiting on metadata to be pushed out.
 *
 * The RCU read side lock must be held by the caller.
 */
enum sync_metadata_status
lttng_ustconsumer_sync_metadata(struct lttng_consumer_local_data *ctx,
				struct lttng_consumer_stream *metadata_stream)
{
	int ret;
	enum sync_metadata_status status;
	struct lttng_consumer_channel *metadata_channel;

	LTTNG_ASSERT(ctx);
	LTTNG_ASSERT(metadata_stream);
	ASSERT_RCU_READ_LOCKED();

	metadata_channel = metadata_stream->chan;
	pthread_mutex_unlock(&metadata_stream->lock);
	/*
	 * Request metadata from the sessiond, but don't wait for the flush
	 * because we locked the metadata thread.
	 */
	ret = lttng_ustconsumer_request_metadata(
		*metadata_channel, ctx->metadata_socket, ctx->consumer_error_socket, false, 0);
	pthread_mutex_lock(&metadata_stream->lock);
	if (ret < 0) {
		status = SYNC_METADATA_STATUS_ERROR;
		goto end;
	}

	/*
	 * The metadata stream and channel can be deleted while the
	 * metadata stream lock was released. The streamed is checked
	 * for deletion before we use it further.
	 *
	 * Note that it is safe to access a logically-deleted stream since its
	 * existence is still guaranteed by the RCU read side lock. However,
	 * it should no longer be used. The close/deletion of the metadata
	 * channel and stream already guarantees that all metadata has been
	 * consumed. Therefore, there is nothing left to do in this function.
	 */
	if (consumer_stream_is_deleted(metadata_stream)) {
		DBG("Metadata stream %" PRIu64 " was deleted during the metadata synchronization",
		    metadata_stream->key);
		status = SYNC_METADATA_STATUS_NO_DATA;
		goto end;
	}

	ret = commit_one_metadata_packet(metadata_stream);
	if (ret < 0) {
		status = SYNC_METADATA_STATUS_ERROR;
		goto end;
	} else if (ret > 0) {
		status = SYNC_METADATA_STATUS_NEW_DATA;
	} else /* ret == 0 */ {
		status = SYNC_METADATA_STATUS_NO_DATA;
		goto end;
	}

	ret = lttng_ust_ctl_snapshot(metadata_stream->ustream);
	if (ret < 0) {
		ERR("Failed to take a snapshot of the metadata ring-buffer positions, ret = %d",
		    ret);
		status = SYNC_METADATA_STATUS_ERROR;
		goto end;
	}

end:
	return status;
}

/*
 * Return 0 on success else a negative value.
 */
static int notify_if_more_data(struct lttng_consumer_stream *stream,
			       struct lttng_consumer_local_data *ctx)
{
	int ret;
	struct lttng_ust_ctl_consumer_stream *ustream;

	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(ctx);

	ustream = stream->ustream;

	/*
	 * First, we are going to check if there is a new subbuffer available
	 * before reading the stream wait_fd.
	 */
	/* Get the next subbuffer */
	ret = lttng_ust_ctl_get_next_subbuf(ustream);
	if (ret) {
		/* No more data found, flag the stream. */
		stream->has_data = 0;
		ret = 0;
		goto end;
	}

	ret = lttng_ust_ctl_put_subbuf(ustream);
	LTTNG_ASSERT(!ret);

	/* This stream still has data. Flag it and wake up the data thread. */
	stream->has_data = 1;

	if (stream->monitor && !stream->hangup_flush_done && !ctx->has_wakeup) {
		ssize_t writelen;

		writelen = lttng_pipe_write(ctx->consumer_wakeup_pipe, "!", 1);
		if (writelen < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			ret = writelen;
			goto end;
		}

		/* The wake up pipe has been notified. */
		ctx->has_wakeup = 1;
	}
	ret = 0;

end:
	return ret;
}

static int consumer_stream_ust_on_wake_up(struct lttng_consumer_stream *stream)
{
	int ret = 0;

	/*
	 * We can consume the 1 byte written into the wait_fd by
	 * UST. Don't trigger error if we cannot read this one byte
	 * (read returns 0), or if the error is EAGAIN or EWOULDBLOCK.
	 *
	 * This is only done when the stream is monitored by a thread,
	 * before the flush is done after a hangup and if the stream
	 * is not flagged with data since there might be nothing to
	 * consume in the wait fd but still have data available
	 * flagged by the consumer wake up pipe.
	 */
	if (stream->monitor && !stream->hangup_flush_done && !stream->has_data) {
		char dummy;
		ssize_t readlen;

		readlen = lttng_read(stream->wait_fd, &dummy, 1);
		if (readlen < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
			ret = readlen;
		}
	}

	return ret;
}

static int extract_common_subbuffer_info(struct lttng_consumer_stream *stream,
					 struct stream_subbuffer *subbuf)
{
	int ret;

	ret = lttng_ust_ctl_get_subbuf_size(stream->ustream, &subbuf->info.data.subbuf_size);
	if (ret) {
		goto end;
	}

	ret = lttng_ust_ctl_get_padded_subbuf_size(stream->ustream,
						   &subbuf->info.data.padded_subbuf_size);
	if (ret) {
		goto end;
	}

end:
	return ret;
}

static int extract_metadata_subbuffer_info(struct lttng_consumer_stream *stream,
					   struct stream_subbuffer *subbuf)
{
	int ret;

	ret = extract_common_subbuffer_info(stream, subbuf);
	if (ret) {
		goto end;
	}

	subbuf->info.metadata.version = stream->metadata_version;

end:
	return ret;
}

static int extract_data_subbuffer_info(struct lttng_consumer_stream *stream,
				       struct stream_subbuffer *subbuf)
{
	int ret;

	ret = extract_common_subbuffer_info(stream, subbuf);
	if (ret) {
		goto end;
	}

	ret = lttng_ust_ctl_get_packet_size(stream->ustream, &subbuf->info.data.packet_size);
	if (ret < 0) {
		PERROR("Failed to get sub-buffer packet size");
		goto end;
	}

	ret = lttng_ust_ctl_get_content_size(stream->ustream, &subbuf->info.data.content_size);
	if (ret < 0) {
		PERROR("Failed to get sub-buffer content size");
		goto end;
	}

	ret = lttng_ust_ctl_get_timestamp_begin(stream->ustream,
						&subbuf->info.data.timestamp_begin);
	if (ret < 0) {
		PERROR("Failed to get sub-buffer begin timestamp");
		goto end;
	}

	ret = lttng_ust_ctl_get_timestamp_end(stream->ustream, &subbuf->info.data.timestamp_end);
	if (ret < 0) {
		PERROR("Failed to get sub-buffer end timestamp");
		goto end;
	}

	ret = lttng_ust_ctl_get_events_discarded(stream->ustream,
						 &subbuf->info.data.events_discarded);
	if (ret) {
		PERROR("Failed to get sub-buffer events discarded count");
		goto end;
	}

	ret = lttng_ust_ctl_get_sequence_number(stream->ustream,
						&subbuf->info.data.sequence_number.value);
	if (ret) {
		/* May not be supported by older LTTng-modules. */
		if (ret != -ENOTTY) {
			PERROR("Failed to get sub-buffer sequence number");
			goto end;
		}
	} else {
		subbuf->info.data.sequence_number.is_set = true;
	}

	ret = lttng_ust_ctl_get_stream_id(stream->ustream, &subbuf->info.data.stream_id);
	if (ret < 0) {
		PERROR("Failed to get stream id");
		goto end;
	}

	ret = lttng_ust_ctl_get_instance_id(stream->ustream,
					    &subbuf->info.data.stream_instance_id.value);
	if (ret) {
		/* May not be supported by older LTTng-modules. */
		if (ret != -ENOTTY) {
			PERROR("Failed to get stream instance id");
			goto end;
		}
	} else {
		subbuf->info.data.stream_instance_id.is_set = true;
	}
end:
	return ret;
}

static int get_next_subbuffer_common(struct lttng_consumer_stream *stream,
				     struct stream_subbuffer *subbuffer)
{
	int ret;
	const char *addr;

	ret = stream->read_subbuffer_ops.extract_subbuffer_info(stream, subbuffer);
	if (ret) {
		goto end;
	}

	ret = get_current_subbuf_addr(stream, &addr);
	if (ret) {
		goto end;
	}

	subbuffer->buffer.buffer =
		lttng_buffer_view_init(addr, 0, subbuffer->info.data.padded_subbuf_size);
	LTTNG_ASSERT(subbuffer->buffer.buffer.data != nullptr);
end:
	return ret;
}

static enum get_next_subbuffer_status get_next_subbuffer(struct lttng_consumer_stream *stream,
							 struct stream_subbuffer *subbuffer)
{
	int ret;
	enum get_next_subbuffer_status status;

	ret = lttng_ust_ctl_get_next_subbuf(stream->ustream);
	switch (ret) {
	case 0:
		status = GET_NEXT_SUBBUFFER_STATUS_OK;
		break;
	case -ENODATA:
	case -EAGAIN:
		/*
		 * The caller only expects -ENODATA when there is no data to
		 * read, but the kernel tracer returns -EAGAIN when there is
		 * currently no data for a non-finalized stream, and -ENODATA
		 * when there is no data for a finalized stream. Those can be
		 * combined into a -ENODATA return value.
		 */
		status = GET_NEXT_SUBBUFFER_STATUS_NO_DATA;
		goto end;
	default:
		status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
		goto end;
	}

	ret = get_next_subbuffer_common(stream, subbuffer);
	if (ret) {
		status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
		goto end;
	}
end:
	return status;
}

static enum get_next_subbuffer_status
get_next_subbuffer_metadata(struct lttng_consumer_stream *stream,
			    struct stream_subbuffer *subbuffer)
{
	int ret;
	bool cache_empty;
	bool got_subbuffer;
	bool coherent;
	bool buffer_empty;
	unsigned long consumed_pos, produced_pos;
	enum get_next_subbuffer_status status;

	do {
		ret = lttng_ust_ctl_get_next_subbuf(stream->ustream);
		if (ret == 0) {
			got_subbuffer = true;
		} else {
			got_subbuffer = false;
			if (ret != -EAGAIN) {
				/* Fatal error. */
				status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
				goto end;
			}
		}

		/*
		 * Determine if the cache is empty and ensure that a sub-buffer
		 * is made available if the cache is not empty.
		 */
		if (!got_subbuffer) {
			ret = commit_one_metadata_packet(stream);
			if (ret < 0 && ret != -ENOBUFS) {
				status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
				goto end;
			} else if (ret == 0) {
				/* Not an error, the cache is empty. */
				cache_empty = true;
				status = GET_NEXT_SUBBUFFER_STATUS_NO_DATA;
				goto end;
			} else {
				cache_empty = false;
			}
		} else {
			pthread_mutex_lock(&stream->chan->metadata_cache->lock);
			cache_empty = stream->chan->metadata_cache->contents.size ==
				stream->ust_metadata_pushed;
			pthread_mutex_unlock(&stream->chan->metadata_cache->lock);
		}
	} while (!got_subbuffer);

	/* Populate sub-buffer infos and view. */
	ret = get_next_subbuffer_common(stream, subbuffer);
	if (ret) {
		status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
		goto end;
	}

	ret = lttng_ustconsumer_sample_snapshot_positions(stream);
	if (ret < 0) {
		/*
		 * -EAGAIN is not expected since we got a sub-buffer and haven't
		 * pushed the consumption position yet (on put_next).
		 */
		PERROR("Failed to take a snapshot of metadata buffer positions");
		status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
		goto end;
	}

	ret = lttng_ustconsumer_get_consumed_snapshot(stream, &consumed_pos);
	if (ret) {
		PERROR("Failed to get metadata consumed position");
		status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
		goto end;
	}

	ret = lttng_ustconsumer_get_produced_snapshot(stream, &produced_pos);
	if (ret) {
		PERROR("Failed to get metadata produced position");
		status = GET_NEXT_SUBBUFFER_STATUS_ERROR;
		goto end;
	}

	/* Last sub-buffer of the ring buffer ? */
	buffer_empty = (consumed_pos + stream->max_sb_size) == produced_pos;

	/*
	 * The sessiond registry lock ensures that coherent units of metadata
	 * are pushed to the consumer daemon at once. Hence, if a sub-buffer is
	 * acquired, the cache is empty, and it is the only available sub-buffer
	 * available, it is safe to assume that it is "coherent".
	 */
	coherent = got_subbuffer && cache_empty && buffer_empty;

	LTTNG_OPTIONAL_SET(&subbuffer->info.metadata.coherent, coherent);
	status = GET_NEXT_SUBBUFFER_STATUS_OK;
end:
	return status;
}

static int put_next_subbuffer(struct lttng_consumer_stream *stream,
			      struct stream_subbuffer *subbuffer __attribute__((unused)))
{
	const int ret = lttng_ust_ctl_put_next_subbuf(stream->ustream);

	LTTNG_ASSERT(ret == 0);
	return ret;
}

static int signal_metadata(struct lttng_consumer_stream *stream,
			   struct lttng_consumer_local_data *ctx __attribute__((unused)))
{
	ASSERT_LOCKED(stream->metadata_rdv_lock);
	return pthread_cond_broadcast(&stream->metadata_rdv) ? -errno : 0;
}

static int lttng_ustconsumer_set_stream_ops(struct lttng_consumer_stream *stream)
{
	int ret = 0;

	stream->read_subbuffer_ops.on_wake_up = consumer_stream_ust_on_wake_up;
	if (stream->metadata_flag) {
		stream->read_subbuffer_ops.get_next_subbuffer = get_next_subbuffer_metadata;
		stream->read_subbuffer_ops.extract_subbuffer_info = extract_metadata_subbuffer_info;
		stream->read_subbuffer_ops.reset_metadata =
			metadata_stream_reset_cache_consumed_position;
		if (stream->chan->is_live) {
			stream->read_subbuffer_ops.on_sleep = signal_metadata;
			ret = consumer_stream_enable_metadata_bucketization(stream);
			if (ret) {
				goto end;
			}
		}
	} else {
		stream->read_subbuffer_ops.get_next_subbuffer = get_next_subbuffer;
		stream->read_subbuffer_ops.extract_subbuffer_info = extract_data_subbuffer_info;
		stream->read_subbuffer_ops.on_sleep = notify_if_more_data;
		if (stream->chan->is_live) {
			stream->read_subbuffer_ops.send_live_beacon = stream_send_live_beacon;
		}
	}

	stream->read_subbuffer_ops.put_next_subbuffer = put_next_subbuffer;
end:
	return ret;
}

/*
 * Called when a stream is created.
 *
 * Return 0 on success or else a negative value.
 */
int lttng_ustconsumer_on_recv_stream(struct lttng_consumer_stream *stream)
{
	int ret;

	LTTNG_ASSERT(stream);

	/*
	 * Don't create anything if this is set for streaming or if there is
	 * no current trace chunk on the parent channel.
	 */
	if (stream->net_seq_idx == (uint64_t) -1ULL && stream->chan->monitor &&
	    stream->chan->trace_chunk) {
		ret = consumer_stream_create_output_files(stream, true);
		if (ret) {
			goto error;
		}
	}

	lttng_ustconsumer_set_stream_ops(stream);
	ret = 0;

error:
	return ret;
}

/*
 * Check if data is still being extracted from the buffers for a specific
 * stream. Consumer data lock MUST be acquired before calling this function
 * and the stream lock.
 *
 * Return 1 if the traced data are still getting read else 0 meaning that the
 * data is available for trace viewer reading.
 */
int lttng_ustconsumer_data_pending(struct lttng_consumer_stream *stream)
{
	int err;

	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream->ustream);
	ASSERT_LOCKED(stream->lock);

	DBG("UST consumer checking data pending");

	if (stream->endpoint_status != CONSUMER_ENDPOINT_ACTIVE) {
		return 0;
	}

	if (stream->chan->type == CONSUMER_CHANNEL_TYPE_METADATA) {
		uint64_t contiguous, pushed;

		/* Ease our life a bit. */
		pthread_mutex_lock(&stream->chan->metadata_cache->lock);
		contiguous = stream->chan->metadata_cache->contents.size;
		pthread_mutex_unlock(&stream->chan->metadata_cache->lock);
		pushed = stream->ust_metadata_pushed;

		/*
		 * We can simply check whether all contiguously available data
		 * has been pushed to the ring buffer, since the push operation
		 * is performed within get_next_subbuf(), and because both
		 * get_next_subbuf() and put_next_subbuf() are issued atomically
		 * thanks to the stream lock within
		 * lttng_ustconsumer_read_subbuffer(). This basically means that
		 * whetnever ust_metadata_pushed is incremented, the associated
		 * metadata has been consumed from the metadata stream.
		 */
		DBG("UST consumer metadata pending check: contiguous %" PRIu64
		    " vs pushed %" PRIu64,
		    contiguous,
		    pushed);
		LTTNG_ASSERT(((int64_t) (contiguous - pushed)) >= 0);
		if ((contiguous != pushed) ||
		    (((int64_t) contiguous - pushed) > 0 || contiguous == 0)) {
			return 1; /* Data is pending */
		}
	} else {
		err = lttng_ust_ctl_get_next_subbuf(stream->ustream);
		if (err == 0) {
			/*
			 * There is still data so let's put back this
			 * subbuffer.
			 */
			err = lttng_ust_ctl_put_subbuf(stream->ustream);
			LTTNG_ASSERT(err == 0);
			return 1; /* Data is pending */
		}

		/* Check for pending unreadable data. */
		err = lttng_ustconsumer_sample_snapshot_positions(stream);
		if (err == 0) {
			unsigned long reserve_pos, consume_pos;
			err = lttng_ustconsumer_get_produced_snapshot(stream, &reserve_pos);
			if (err == 0) {
				err = lttng_ustconsumer_get_consumed_snapshot(stream, &consume_pos);
				if (err == 0) {
					if (reserve_pos != consume_pos) {
						return 1; /* Unreadable data is pending. */
					}
				}
			}
		}
	}

	/* Data is NOT pending so ready to be read. */
	return 0;
}

/*
 * Stop a given metadata channel timer if enabled and close the wait fd which
 * is the poll pipe of the metadata stream.
 *
 * This MUST be called with the metadata channel lock acquired.
 */
void lttng_ustconsumer_close_metadata(struct lttng_consumer_channel *metadata)
{
	int ret;

	LTTNG_ASSERT(metadata);
	LTTNG_ASSERT(metadata->type == CONSUMER_CHANNEL_TYPE_METADATA);

	DBG("Closing metadata channel key %" PRIu64, metadata->key);

	if (metadata->metadata_switch_timer_task) {
		consumer_timer_switch_stop(metadata);
	}

	if (!metadata->metadata_stream) {
		goto end;
	}

	/*
	 * Closing write side so the thread monitoring the stream wakes up if any
	 * and clean the metadata stream.
	 */
	if (metadata->metadata_stream->ust_metadata_poll_pipe[1] >= 0) {
		ret = close(metadata->metadata_stream->ust_metadata_poll_pipe[1]);
		if (ret < 0) {
			PERROR("closing metadata pipe write side");
		}
		metadata->metadata_stream->ust_metadata_poll_pipe[1] = -1;
	}

end:
	return;
}

/*
 * Close every metadata stream wait fd of the metadata hash table. This
 * function MUST be used very carefully so not to run into a race between the
 * metadata thread handling streams and this function closing their wait fd.
 *
 * For UST, this is used when the session daemon hangs up. Its the metadata
 * producer so calling this is safe because we are assured that no state change
 * can occur in the metadata thread for the streams in the hash table.
 */
void lttng_ustconsumer_close_all_metadata(struct lttng_ht *metadata_ht)
{
	LTTNG_ASSERT(metadata_ht);
	LTTNG_ASSERT(metadata_ht->ht);

	DBG("UST consumer closing all metadata streams");

	for (auto *stream :
	     lttng::urcu::lfht_iteration_adapter<lttng_consumer_stream,
						 decltype(lttng_consumer_stream::node),
						 &lttng_consumer_stream::node>(*metadata_ht->ht)) {
		health_code_update();

		pthread_mutex_lock(&stream->chan->lock);
		pthread_mutex_lock(&stream->lock);
		lttng_ustconsumer_close_metadata(stream->chan);
		pthread_mutex_unlock(&stream->lock);
		pthread_mutex_unlock(&stream->chan->lock);
	}
}

void lttng_ustconsumer_close_stream_wakeup(struct lttng_consumer_stream *stream)
{
	int ret;

	ret = lttng_ust_ctl_stream_close_wakeup_fd(stream->ustream);
	if (ret < 0) {
		ERR("Unable to close wakeup fd");
	}
}

/*
 * Please refer to consumer-timer.c before adding any lock within this
 * function or any of its callees. Timers have a very strict locking
 * semantic with respect to teardown. Failure to respect this semantic
 * introduces deadlocks.
 *
 * DON'T hold the metadata lock when calling this function, else this
 * can cause deadlock involving consumer awaiting for metadata to be
 * pushed out due to concurrent interaction with the session daemon.
 */
int lttng_ustconsumer_request_metadata(lttng_consumer_channel& channel,
				       protected_socket& sessiond_metadata_socket,
				       protected_socket& consumer_error_socket,
				       bool invoked_by_timer,
				       int wait)
{
	struct lttcomm_metadata_request_msg request;
	struct lttcomm_consumer_msg msg;
	const lttcomm_return_code ret_code = LTTCOMM_CONSUMERD_SUCCESS;
	uint64_t len, key, offset, version;
	int ret;

	LTTNG_ASSERT(!channel.is_deleted);
	LTTNG_ASSERT(channel.metadata_cache);

	memset(&request, 0, sizeof(request));

	/* send the metadata request to sessiond */
	switch (the_consumer_data.type) {
	case LTTNG_CONSUMER64_UST:
		request.bits_per_long = 64;
		break;
	case LTTNG_CONSUMER32_UST:
		request.bits_per_long = 32;
		break;
	default:
		request.bits_per_long = 0;
		break;
	}

	request.session_id = channel.session_id;
	request.session_id_per_pid = channel.session_id_per_pid;
	/*
	 * Request the application UID here so the metadata of that application can
	 * be sent back. The channel UID corresponds to the user UID of the session
	 * used for the rights on the stream file(s).
	 */
	request.uid = channel.ust_app_uid;
	request.key = channel.key;

	DBG("Sending metadata request to sessiond, session id %" PRIu64 ", per-pid %" PRIu64
	    ", app UID %u and channel key %" PRIu64,
	    request.session_id,
	    request.session_id_per_pid,
	    request.uid,
	    request.key);

	lttng::pthread::lock_guard metadata_socket_lock(sessiond_metadata_socket.lock);

	health_code_update();

	ret = lttcomm_send_unix_sock(sessiond_metadata_socket.fd, &request, sizeof(request));
	if (ret < 0) {
		ERR("Asking metadata to sessiond");
		goto end;
	}

	health_code_update();

	/* Receive the metadata from sessiond */
	ret = lttcomm_recv_unix_sock(sessiond_metadata_socket.fd, &msg, sizeof(msg));
	if (ret != sizeof(msg)) {
		DBG("Consumer received unexpected message size %d (expects %zu)", ret, sizeof(msg));
		lttng_consumer_send_error(consumer_error_socket, LTTCOMM_CONSUMERD_ERROR_RECV_CMD);
		/*
		 * The ret value might 0 meaning an orderly shutdown but this is ok
		 * since the caller handles this.
		 */
		goto end;
	}

	health_code_update();

	if (msg.cmd_type == LTTNG_ERR_UND) {
		/* No registry found */
		(void) consumer_send_status_msg(sessiond_metadata_socket.fd, ret_code);
		ret = 0;
		goto end;
	} else if (msg.cmd_type != LTTNG_CONSUMER_PUSH_METADATA) {
		ERR("Unexpected cmd_type received %d", msg.cmd_type);
		ret = -1;
		goto end;
	}

	len = msg.u.push_metadata.len;
	key = msg.u.push_metadata.key;
	offset = msg.u.push_metadata.target_offset;
	version = msg.u.push_metadata.version;

	LTTNG_ASSERT(key == channel.key);
	if (len == 0) {
		DBG("No new metadata to receive for key %" PRIu64, key);
	}

	health_code_update();

	/* Tell session daemon we are ready to receive the metadata. */
	ret = consumer_send_status_msg(sessiond_metadata_socket.fd, LTTCOMM_CONSUMERD_SUCCESS);
	if (ret < 0 || len == 0) {
		/*
		 * Somehow, the session daemon is not responding anymore or there is
		 * nothing to receive.
		 */
		goto end;
	}

	health_code_update();

	ret = lttng_ustconsumer_recv_metadata(sessiond_metadata_socket.fd,
					      key,
					      offset,
					      len,
					      version,
					      &channel,
					      invoked_by_timer,
					      wait);
	if (ret >= 0) {
		/*
		 * Only send the status msg if the sessiond is alive meaning a positive
		 * ret code.
		 */
		(void) consumer_send_status_msg(sessiond_metadata_socket.fd, ret);
	}

	ret = 0;

end:
	health_code_update();
	return ret;
}

/*
 * Return the ustctl call for the get stream id.
 */
int lttng_ustconsumer_get_stream_id(struct lttng_consumer_stream *stream, uint64_t *stream_id)
{
	LTTNG_ASSERT(stream);
	LTTNG_ASSERT(stream_id);

	return lttng_ust_ctl_get_stream_id(stream->ustream, stream_id);
}

void lttng_ustconsumer_sigbus_handle(void *addr)
{
	lttng_ust_ctl_sigbus_handle(addr);
}

/* Return the number of pending reclamations. */
uint32_t lttng_ustconsumer_reclaim_session_owner_id(uint64_t session_id, uint32_t owner_id)
{
	const auto ht = the_consumer_data.channels_by_session_id_ht;
	const lttng::pthread::lock_guard consumer_lock(the_consumer_data.lock);

	auto pending_reclamations = 0U;

	for (auto *channel : lttng::urcu::lfht_filtered_iteration_adapter<
		     lttng_consumer_channel,
		     decltype(lttng_consumer_channel::channels_by_session_id_ht_node),
		     &lttng_consumer_channel::channels_by_session_id_ht_node,
		     std::uint64_t>(*ht->ht,
				    &session_id,
				    ht->hash_fct(&session_id, lttng_ht_seed),
				    ht->match_fct)) {
		const std::lock_guard<std::mutex> channel_lock(
			channel->owners_pending_reclamation_lock);

		try {
			channel->owners_pending_reclamation.insert(owner_id);
			pending_reclamations += 1;
		} catch (const std::bad_alloc&) {
			ERR_FMT("Failed to allocate while adding owner-id reclamation to channel: "
				"channel=`{}` session_id={} owner_id={}",
				channel->name,
				session_id,
				owner_id);
		}
	}

	return pending_reclamations;
}

static bool is_subbuf_state_stalled(const struct stream_subbuffer_transaction_state& old_state,
				    const struct stream_subbuffer_transaction_state& new_state)
{
	if (old_state.owner_id != new_state.owner_id) {
		return false;
	}

	if (old_state.hot_commit_count != new_state.hot_commit_count) {
		return false;
	}

	if (old_state.cold_commit_count != new_state.cold_commit_count) {
		return false;
	}

	return true;
}

static int take_stream_stall_snapshot(struct lttng_consumer_stream& stream,
				      std::set<uint32_t>& observed_owner_ids)
{
	/* The iterator is initially invalid; call [...]_next() before accessing its value. */
	auto subbuf_iter = lttng::make_unique_wrapper<lttng_ust_ctl_subbuf_iter,
						      destroy_subbuf_iter>([&stream]() {
		lttng_ust_ctl_subbuf_iter *it = nullptr;

		const auto err = lttng_ust_ctl_subbuf_iter_create(
			stream.ustream, LTTNG_UST_CTL_SUBBUF_ITER_UNCONSUMED, &it);
		if (err) {
			ERR_FMT("Failed to create sub-buffer iterator for stream: stream_name=`{}`, stream_key={}, error={}",
				stream.name,
				stream.key,
				-err);
		}

		return it;
	}());

	if (!subbuf_iter) {
		LTTNG_THROW_ALLOCATION_FAILURE_ERROR(fmt::format(
			"Failed to create sub-buffer iterator for stream: channel_name=`{}`, stream_key={}",
			stream.chan->name,
			stream.key));
	}

	while (true) {
		int err = lttng_ust_ctl_subbuf_iter_next(subbuf_iter.get());
		if (err == 0) {
			break;
		} else if (err < 0) {
			ERR_FMT("Failed to move sub-buffer iterator forwards for stream: stream_name=`{}`, stream_key={}, error={}",
				stream.name,
				stream.key,
				-err);
			return err;
		}

		size_t idx;
		err = lttng_ust_ctl_subbuf_iter_index(subbuf_iter.get(), &idx);
		if (err) {
			ERR_FMT("Failed to get sub-buffer index from sub-buffer iterator for stream: stream_name=`{}`, stream_key={}, error={}",
				stream.name,
				stream.key,
				-err);
			return err;
		}

		stream_subbuffer_transaction_state new_state;
		err = lttng_ust_ctl_subbuf_iter_owner(subbuf_iter.get(), &new_state.owner_id);
		if (err) {
			ERR_FMT("Failed to get sub-buffer owner from sub-buffer iterator for stream: stream_name=`{}`, stream_key={}, error={}",
				stream.name,
				stream.key,
				-err);
			return err;
		}

		/*
		 * If UNSET, skip the sub-buffer because we don't want to check
		 * for stalled when no producers is being present.
		 *
		 * If CONSUMER, then we are the owner and we assume we can never
		 * stall. This can happen if we are fixing or flushing the
		 * sub-bufffer.
		 */
		if ((new_state.owner_id == LTTNG_UST_ABI_OWNER_ID_UNSET) ||
		    (new_state.owner_id == LTTNG_UST_ABI_OWNER_ID_CONSUMER)) {
			continue;
		}

		if (idx < stream.subbuffer_transaction_states.size()) {
			err = lttng_ust_ctl_subbuf_iter_cc_hot(subbuf_iter.get(),
							       &new_state.hot_commit_count);

			if (err) {
				ERR_FMT("Failed to get sub-buffer hot commit counter from sub-buffer iterator for stream: stream_name=`{}`, stream_key={}, error={}",
					stream.name,
					stream.key,
					-err);
				return err;
			}

			err = lttng_ust_ctl_subbuf_iter_cc_cold(subbuf_iter.get(),
								&new_state.cold_commit_count);

			if (err) {
				ERR_FMT("Failed to get sub-buffer cold commit counter from sub-buffer iterator for stream: stream_name=`{}`, stream_key={}, error={}",
					stream.name,
					stream.key - err);
				return err;
			}

			if (is_subbuf_state_stalled(stream.subbuffer_transaction_states[idx],
						    new_state)) {
				WARN_FMT(
					"Possible stalled sub-buffer in stream: stream_name=`{}`, stream_key={}, subbuffer_index={}, subbuffer_owner={}, subbuffer_hot={}, subbuffer_cold={}",
					stream.name,
					stream.key,
					idx,
					new_state.owner_id,
					new_state.hot_commit_count,
					new_state.cold_commit_count);
			}

			stream.subbuffer_transaction_states[idx] = new_state;
		} else {
			ERR_FMT("Invalid sub-buffer state index provided by ust-ctl: stream_name=`{}`, stream_key={}, index={}, expected_max_index={}",
				stream.name,
				stream.key,
				idx,
				stream.subbuffer_transaction_states.size());
		}

		observed_owner_ids.insert(new_state.owner_id);
	}

	return 0;
}

static int take_channel_stall_snapshot(struct lttng_consumer_channel *channel,
				       std::set<uint32_t>& observed_owner_ids)
{
	const struct lttng_ht *ht = the_consumer_data.stream_per_chan_id_ht;

	const lttng::urcu::read_lock_guard read_lock;
	for (auto *stream : lttng::urcu::lfht_filtered_iteration_adapter<
		     lttng_consumer_stream,
		     decltype(lttng_consumer_stream::node_channel_id),
		     &lttng_consumer_stream::node_channel_id,
		     std::uint64_t>(*ht->ht,
				    &channel->key,
				    ht->hash_fct(&channel->key, lttng_ht_seed),
				    ht->match_fct)) {
		const lttng::pthread::lock_guard consumer_lock(stream->lock);

		if (cds_lfht_is_node_deleted(&stream->node.node)) {
			continue;
		}

		if (stream->metadata_flag) {
			continue;
		}

		int err = take_stream_stall_snapshot(*stream, observed_owner_ids);

		if (err != 0) {
			return err;
		}
	}

	return 0;
}

static int fixup_stalled_stream(struct lttng_consumer_stream& stream, std::set<uint32_t>& stalled)
{
	LTTNG_ASSERT(!stream.metadata_flag);

	int err;

	err = lttng_ustconsumer_sample_snapshot_positions(&stream);
	if ((err < 0) && (err != -EAGAIN)) {
		if (err != -EAGAIN) {
			ERR_FMT("Taking UST snapshot: {}", -err);
		}
		return err;
	}

	unsigned long consumed_pos;

	err = lttng_ustconsumer_get_consumed_snapshot(&stream, &consumed_pos);
	if (err) {
		ERR_FMT("Failed to get consumed position of stream: {}", stream.name);
		return err;
	}

	unsigned long produced_pos;

	err = lttng_ustconsumer_get_produced_snapshot(&stream, &produced_pos);
	if (err) {
		ERR_FMT("Failed to get produced position of stream: {}", stream.name);
		return err;
	}

	std::vector<uint32_t> stalled_array(stalled.size());

	std::copy(stalled.begin(), stalled.end(), stalled_array.begin());

	err = lttng_ust_ctl_fixup_stalled_stream(stream.ustream,
						 consumed_pos,
						 produced_pos,
						 stalled_array.data(),
						 stalled_array.size());

	/*
	 * If positive, this gives us the number of sub-buffer channel that
	 * failed to be fixup. This is not a fatal error and we can retry the
	 * operation later.
	 *
	 * For negative value, there was an unknown error, e.g. invalid
	 * arguments or un-mapped shared memory.
	 */
	if (err > 0) {
		WARN_FMT(
			"Failed to fixup {} stalled sub-buffer{} in stream: stream_name=`{}` stream_key={}",
			err,
			err > 1 ? "s" : "",
			stream.name,
			stream.key);
		return err;
	} else if (err < 0) {
		ERR_FMT("Unknown error while fixing stream: stream_name=`{}` stream_key={} error={}",
			stream.name,
			stream.key,
			-err);
		return err;
	}

	return 0;
}

static int fixup_stalled_channel(struct lttng_consumer_channel *channel,
				 std::set<uint32_t>& stalled)
{
	const struct lttng_ht *ht = the_consumer_data.stream_per_chan_id_ht;
	bool success = true;

	const lttng::urcu::read_lock_guard read_lock;
	for (auto *stream : lttng::urcu::lfht_filtered_iteration_adapter<
		     lttng_consumer_stream,
		     decltype(lttng_consumer_stream::node_channel_id),
		     &lttng_consumer_stream::node_channel_id,
		     std::uint64_t>(*ht->ht,
				    &channel->key,
				    ht->hash_fct(&channel->key, lttng_ht_seed),
				    ht->match_fct)) {
		const lttng::pthread::lock_guard consumer_lock(stream->lock);
		if (cds_lfht_is_node_deleted(&stream->node.node)) {
			continue;
		}

		if (stream->metadata_flag) {
			continue;
		}

		int err = fixup_stalled_stream(*stream, stalled);

		if (err != 0) {
			success = false;
		}
	}

	return success;
}

int lttng_ustconsumer_fixup_stalled_channel(struct lttng_consumer_channel *channel,
					    std::set<uint32_t>& reclaimed_owner_ids,
					    size_t& observed_count)
{
	/*
	 * The fixup require to know the number of sub-buffer in the channel.
	 *
	 * For now, only UID userspace channels need to be fixup.
	 */
	LTTNG_ASSERT(channel->subbuffer_count.has_value());

	/*
	 * Copy the pending reclamations. We will remove the copied element
	 * later if all stalled buffers have been resolved.
	 */
	std::set<uint32_t> owner_ids_ready_for_reclamation;
	{
		const std::lock_guard<std::mutex> channel_lock(
			channel->owners_pending_reclamation_lock);
		std::copy(channel->owners_pending_reclamation.begin(),
			  channel->owners_pending_reclamation.end(),
			  std::inserter(owner_ids_ready_for_reclamation,
					owner_ids_ready_for_reclamation.begin()));
	}

	std::set<uint32_t> observed_owner_ids{};

	/*
	 * The goal here is to list the owner-ids observed across all streams of
	 * the channel. It also updates the last seen state for individual
	 * sub-buffers of each stream.
	 *
	 * The observed owner-ids are then passed to UST-ctl for the fixup.
	 */
	if (take_channel_stall_snapshot(channel, observed_owner_ids) != 0) {
		return -1;
	}

	DBG_FMT("Observed {} owners in channel {}", observed_owner_ids.size(), channel->name);

	observed_count = observed_owner_ids.size();

	/*
	 * Set of stalled owner-ids, that is IDs that were observed in the
	 * sub-buffers and that are ready for reclamation.
	 *
	 * We know that they are stalled because they are ready for reclamation
	 * (dead), yet they can be seen in memory.
	 */
	std::set<uint32_t> stalled;

	std::set_intersection(owner_ids_ready_for_reclamation.begin(),
			      owner_ids_ready_for_reclamation.end(),
			      observed_owner_ids.begin(),
			      observed_owner_ids.end(),
			      std::inserter(stalled, std::begin(stalled)));

	/*
	 * One might find it to be an optimization to not call
	 * `fixup_stall_channel` if `stalled` has a size of zero. However, it is
	 * possible to have stalled buffers with UNSET owners. Thus, always call
	 * this function so that `lttng_ust_ctl_fixup_stalled_stream` gets
	 * called for every stream of the channel.
	 */
	const bool success = fixup_stalled_channel(channel, stalled);

	/* If the channel was successfully fixup, that is no sub-buffer of every
	 * streams is not stalled with one of the owner IDs in `stalled`, then
	 * the set of IDs to reclaim is the set of IDs ready for
	 * reclamation. Otherwise, it is the difference between the set of owner
	 * IDs ready to be reclaimed and the set of potentially stalled owner
	 * IDs.
	 */
	if (success) {
		reclaimed_owner_ids = std::move(owner_ids_ready_for_reclamation);
	} else {
		/*
		 * reclaimed_owner_ids = owner_ids_ready_for_reclamation - stalled
		 */
		std::set_difference(owner_ids_ready_for_reclamation.begin(),
				    owner_ids_ready_for_reclamation.end(),
				    stalled.begin(),
				    stalled.end(),
				    std::inserter(reclaimed_owner_ids,
						  std::begin(reclaimed_owner_ids)));
	}

	/* Remove the owner_ids_ready_for_reclamation from the owners set */
	{
		const std::lock_guard<std::mutex> channel_lock(
			channel->owners_pending_reclamation_lock);
		for (auto owner : reclaimed_owner_ids) {
			channel->owners_pending_reclamation.erase(owner);
		}
	}

	return 0;
}

void lttng_ustconsumer_quiescent_stalled_channel(struct lttng_consumer_channel& channel)
{
	const auto stall_watchdog = channel.stall_watchdog_timer_task;

	if (!stall_watchdog) {
		return;
	}

	const auto watchdog =
		reinterpret_cast<lttng::consumer::watchdog_timer_task *>(stall_watchdog.get());

	LTTNG_ASSERT(channel.subbuffer_count);

	/*
	 * Loop until all sub-buffer are fixed.
	 *
	 * We limit the number of loops to the number of
	 * sub-buffers. There is no real relation here except that the
	 * number of turn to fixup can be proportional to the number of
	 * sub-buffers, although it is not guaranteed that everything
	 * will be fixed within that number of turn.
	 *
	 * In the worst case, all sub-buffers are expected to be
	 * fixed-up after `subbuffer_count` attempts. However, in the
	 * unlikely case that a "stall" case isn't handled by the
	 * recovery algorithm, an error is logged.
	 */
	ssize_t observed_owners = 0;
	for (size_t i = 0; i < channel.subbuffer_count.value(); ++i) {
		observed_owners = watchdog->run();

		if (observed_owners == 0) {
			return;
		}

		if (observed_owners == -1) {
			ERR_FMT("Error while trying to reach quiescent state: "
				"channel_name=`{}`, channel_key={}, session_id={}",
				channel.name,
				channel.key,
				channel.session_id);

			(void) poll(nullptr, 0, 10);
		} else {
			WARN_FMT(
				"Owners found in streams while trying to reach quiescent state: "
				"channel_name=`{}`, channel_key={}, session_id={}, observed_owners={}",
				channel.name,
				channel.key,
				channel.session_id,
				observed_owners);
		}
	}

	ERR_FMT("Owners found in streams while trying to reach quiescent state after exceed number of attempts: "
		"channel_name=`{}`, channel_key={}, session_id={}, observed_owners={}, max_attempts={}",
		channel.name,
		channel.key,
		channel.session_id,
		observed_owners,
		channel.subbuffer_count.value());
}
