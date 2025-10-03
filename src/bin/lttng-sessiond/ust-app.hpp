/*
 * SPDX-FileCopyrightText: 2011 EfficiOS Inc.
 * SPDX-FileCopyrightText: 2016 Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#ifndef _LTT_UST_APP_H
#define _LTT_UST_APP_H

#include "lttng-ust-ctl.hpp"
#include "trace-class.hpp"
#include "trace-ust.hpp"
#include "ust-field-quirks.hpp"

#include <common/format.hpp>
#include <common/index-allocator.hpp>
#include <common/optional.hpp>
#include <common/reference.hpp>
#include <common/scope-exit.hpp>
#include <common/uuid.hpp>

#include <list>
#include <stdint.h>
#include <vector>

#define UST_APP_EVENT_LIST_SIZE 32

/* Process name (short). */
#define UST_APP_PROCNAME_LEN 16

struct lttng_bytecode;
struct lttng_ust_filter_bytecode;

namespace lttng {
namespace sessiond {
namespace ust {
class registry_session;
} /* namespace ust */
} /* namespace sessiond */
} /* namespace lttng */

extern int the_ust_consumerd64_fd, the_ust_consumerd32_fd;

/*
 * Object used to close the notify socket in a call_rcu(). Since the
 * application might not be found, we need an independant object containing the
 * notify socket fd.
 */
struct ust_app_notify_sock_obj {
	int fd;
	struct rcu_head head;
};

struct ust_app_ht_key {
	const char *name;
	const struct lttng_bytecode *filter;
	enum lttng_ust_abi_loglevel_type loglevel_type;
	int loglevel_value;
	const struct lttng_event_exclusion *exclusion;
};

/*
 * Application registration data structure.
 */
struct ust_register_msg {
	enum lttng_ust_ctl_socket_type type;
	uint32_t major;
	uint32_t minor;
	uint32_t abi_major;
	uint32_t abi_minor;
	pid_t pid;
	pid_t ppid;
	uid_t uid;
	gid_t gid;
	uint32_t bits_per_long;
	uint32_t uint8_t_alignment;
	uint32_t uint16_t_alignment;
	uint32_t uint32_t_alignment;
	uint32_t uint64_t_alignment;
	uint32_t long_alignment;
	int byte_order; /* BIG_ENDIAN or LITTLE_ENDIAN */
	char name[LTTNG_UST_ABI_PROCNAME_LEN];
};

/*
 * Global applications HT used by the session daemon. This table is indexed by
 * PID using the pid_n node and pid value of an ust_app.
 */
extern struct lttng_ht *ust_app_ht;

/*
 * Global applications HT used by the session daemon. This table is indexed by
 * socket using the sock_n node and sock value of an ust_app.
 *
 * The 'sock' in question here is the 'command' socket.
 */
extern struct lttng_ht *ust_app_ht_by_sock;

/*
 * Global applications HT used by the session daemon. This table is indexed by
 * socket using the notify_sock_n node and notify_sock value of an ust_app.
 */
extern struct lttng_ht *ust_app_ht_by_notify_sock;

/* Stream list containing ust_app_stream. */
struct ust_app_stream_list {
	unsigned int count;
	struct cds_list_head head;
};

struct ust_app_ctx {
	int handle;
	struct lttng_ust_context_attr ctx;
	struct lttng_ust_abi_object_data *obj;
	struct lttng_ht_node_ulong node;
};

struct ust_app_event {
	bool enabled;
	int handle;
	struct lttng_ust_abi_object_data *obj;
	struct lttng_ust_abi_event attr;
	char name[LTTNG_UST_ABI_SYM_NAME_LEN];
	struct lttng_ht_node_str node;
	struct lttng_bytecode *filter;
	struct lttng_event_exclusion *exclusion;
};

struct ust_app_event_notifier_rule {
	bool enabled;
	uint64_t error_counter_index;
	int handle;
	struct lttng_ust_abi_object_data *obj;
	/* Holds a strong reference. */
	struct lttng_trigger *trigger;
	/* Unique ID returned by the tracer to identify this event notifier. */
	uint64_t token;
	struct lttng_ht_node_u64 node;
	/* The trigger object owns the filter. */
	const struct lttng_bytecode *filter;
	/* Owned by this. */
	struct lttng_event_exclusion *exclusion;
	/* For delayed reclaim. */
	struct rcu_head rcu_head;
};

struct ust_app_stream {
	int handle;
	char pathname[PATH_MAX];
	/* Format is %s_%d respectively channel name and CPU number. */
	char name[DEFAULT_STREAM_NAME_LEN];
	struct lttng_ust_abi_object_data *obj;
	/* Using a list of streams to keep order. */
	struct cds_list_head list;
};

struct ust_app_channel {
	bool enabled;
	int handle;
	/*
	 * Unique key used to identify the channel on the consumer side.
	 * 0 is a reserved 'invalid' value used to indicate that the consumer
	 * does not know about this channel (i.e. an error occurred).
	 */
	uint64_t key;
	/* Id of the tracing channel set on creation. */
	uint64_t tracing_channel_id;
	/* Number of stream that this channel is expected to receive. */
	unsigned int expected_stream_count;
	char name[LTTNG_UST_ABI_SYM_NAME_LEN];
	struct lttng_ust_abi_object_data *obj;
	struct lttng_ust_ctl_consumer_channel_attr attr;
	struct ust_app_stream_list streams;
	/* Session pointer that owns this object. */
	struct ust_app_session *session;
	/* Hashtable of ust_app_ctx instances. */
	struct lttng_ht *ctx;
	/* Hashtable of ust_app_event instances. */
	struct lttng_ht *events;
	uint64_t tracefile_size;
	uint64_t tracefile_count;
	uint64_t monitor_timer_interval;
	LTTNG_OPTIONAL(uint64_t) watchdog_timer_interval;
	lttng::sessiond::recording_channel_configuration::buffer_preallocation_policy_t
		preallocation_policy;
	/*
	 * Node indexed by channel name in the channels' hash table of a session.
	 */
	struct lttng_ht_node_str node;
	/*
	 * Node indexed by UST channel object descriptor (handle). Stored in the
	 * ust_objd hash table in the ust_app object.
	 */
	struct lttng_ht_node_ulong ust_objd_node;
	/* For delayed reclaim */
	struct rcu_head rcu_head;
};

struct ust_app_session {
private:
	static void _session_unlock(ust_app_session *session)
	{
		_const_session_unlock(session);
	}

	static void _const_session_unlock(const ust_app_session *session)
	{
		pthread_mutex_unlock(&session->_lock);
	}

public:
	using locked_weak_ref = lttng::non_copyable_reference<
		ust_app_session,
		lttng::memory::create_deleter_class<ust_app_session,
						    ust_app_session::_session_unlock>::deleter>;
	using const_locked_weak_ref = lttng::non_copyable_reference<
		const ust_app_session,
		lttng::memory::create_deleter_class<const ust_app_session,
						    ust_app_session::_const_session_unlock>::deleter>;

	static locked_weak_ref make_locked_weak_ref(ust_app_session& ua_session)
	{
		return lttng::make_non_copyable_reference<locked_weak_ref::referenced_type,
							  locked_weak_ref::deleter>(ua_session);
	}

	static const_locked_weak_ref make_locked_weak_ref(const ust_app_session& ua_session)
	{
		return lttng::make_non_copyable_reference<const_locked_weak_ref::referenced_type,
							  const_locked_weak_ref::deleter>(
			ua_session);
	}

	ust_app_session::const_locked_weak_ref lock() const noexcept
	{
		pthread_mutex_lock(&_lock);
		return ust_app_session::make_locked_weak_ref(*this);
	}

	ust_app_session::locked_weak_ref lock() noexcept
	{
		pthread_mutex_lock(&_lock);
		return ust_app_session::make_locked_weak_ref(*this);
	}

	struct identifier {
		enum class application_abi : std::uint8_t { ABI_32 = 32, ABI_64 = 64 };
		enum class buffer_allocation_policy : std::uint8_t { PER_PID, PER_UID };

		/* Unique identifier of the ust_app_session. */
		std::uint64_t id;
		/* Unique identifier of the ltt_session. */
		std::uint64_t session_id;
		/* Credentials of the application which owns the ust_app_session. */
		lttng_credentials app_credentials;
		application_abi abi;
		buffer_allocation_policy allocation_policy;
	};

	identifier get_identifier() const noexcept
	{
		/*
		 * To work around synchro design issues, this method allows the sampling
		 * of a ust_app_session's identifying properties without taking its lock.
		 *
		 * Since those properties are immutable, it is safe to sample them without
		 * holding the lock (as long as the existence of the instance is somehow
		 * guaranteed).
		 *
		 * The locking issue that motivates this method is that the application
		 * notitication handling thread needs to access the registry_session in response to
		 * a message from the application. The ust_app_session's ID is needed to look-up the
		 * registry session.
		 *
		 * The application's message can be emited in response to a command from the
		 * session daemon that is emited by the client thread.
		 *
		 * During that command, the client thread holds the ust_app_session lock until
		 * the application replies to the command. This causes the notification thread
		 * to block when it attempts to sample the ust_app_session's ID properties.
		 */
		LTTNG_ASSERT(bits_per_long == 32 || bits_per_long == 64);
		LTTNG_ASSERT(buffer_type == LTTNG_BUFFER_PER_PID ||
			     buffer_type == LTTNG_BUFFER_PER_UID);

		return { .id = id,
			 .session_id = tracing_id,
			 .app_credentials = real_credentials,
			 .abi = bits_per_long == 32 ? identifier::application_abi::ABI_32 :
						      identifier::application_abi::ABI_64,
			 .allocation_policy = buffer_type == LTTNG_BUFFER_PER_PID ?
				 identifier::buffer_allocation_policy::PER_PID :
				 identifier::buffer_allocation_policy::PER_UID };
	}

	bool enabled = false;
	/* started: has the session been in started state at any time ? */
	bool started = false; /* allows detection of start vs restart. */
	int handle = 0; /* used has unique identifier for app session */

	bool deleted = false; /* Session deleted flag. Check with lock held. */

	/*
	 * Tracing session ID. Multiple ust app session can have the same tracing
	 * session id making this value NOT unique to the object.
	 */
	uint64_t tracing_id = 0;
	uint64_t id = 0; /* Unique session identifier */
	struct lttng_ht *channels = nullptr; /* Registered channels */
	struct lttng_ht_node_u64 node = {};
	/*
	 * Node indexed by UST session object descriptor (handle). Stored in the
	 * ust_sessions_objd hash table in the ust_app object.
	 */
	struct lttng_ht_node_ulong ust_objd_node = {};
	/* Starts with 'ust'; no leading slash. */
	char path[PATH_MAX] = {};
	/* UID/GID of the application owning the session */
	struct lttng_credentials real_credentials = {};
	/* Effective UID and GID. Same as the tracing session. */
	struct lttng_credentials effective_credentials = {};
	/*
	 * Once at least *one* session is created onto the application, the
	 * corresponding consumer is set so we can use it on unregistration.
	 */
	struct consumer_output *consumer = nullptr;
	enum lttng_buffer_type buffer_type = LTTNG_BUFFER_PER_PID;
	/* ABI of the session. Same value as the application. */
	uint32_t bits_per_long = 0;
	/* For delayed reclaim */
	struct rcu_head rcu_head = {};
	/* If the channel's streams have to be outputed or not. */
	unsigned int output_traces = 0;
	unsigned int live_timer_interval = 0; /* usec */

	/* Metadata channel attributes. */
	struct lttng_ust_ctl_consumer_channel_attr metadata_attr = {};

	char root_shm_path[PATH_MAX] = {};
	char shm_path[PATH_MAX] = {};

private:
	/*
	 * Lock protecting this session's ust app interaction. Held
	 * across command send/recv to/from app. Never nests within the
	 * session registry lock.
	 */
	mutable pthread_mutex_t _lock = PTHREAD_MUTEX_INITIALIZER;
};

/*
 * Registered traceable applications. Libust registers to the session daemon
 * and a linked list is kept of all running traceable app.
 */
struct ust_app {
	/*
	 * The lifetime of 'sock' holds a reference to the application; the
	 * application management thread will release a reference to the
	 * application if the application dies.
	 */
	urcu_ref ref = {};

	/* Traffic initiated from the session daemon to the application. */
	int sock = -1;
	pthread_mutex_t sock_lock = {}; /* Protects sock protocol. */

	/* Traffic initiated from the application to the session daemon. */
	int notify_sock = static_cast<int>(-1);
	pid_t pid = static_cast<pid_t>(-1);
	pid_t ppid = static_cast<pid_t>(-1);
	uid_t uid = static_cast<uid_t>(-1); /* User ID that owns the apps */
	gid_t gid = static_cast<gid_t>(-1); /* Group ID that owns the apps */

	/* App ABI. */
	lttng::sessiond::trace::abi abi = {};

	int compatible = 0; /* If the lttng-ust tracer version does not match the
					   supported version of the session daemon, this flag is
					   set to 0 (NOT compatible) else 1. */
	struct lttng_ust_abi_tracer_version version = {};
	uint32_t v_major = static_cast<uint32_t>(-1); /* Version major number */
	uint32_t v_minor = static_cast<uint32_t>(-1); /* Version minor number */
	/* Extra for the NULL byte. */
	char name[UST_APP_PROCNAME_LEN + 1] = {};

	struct lttng_ht *sessions = nullptr;
	struct lttng_ht_node_ulong pid_n = {};
	struct lttng_ht_node_ulong sock_n = {};
	struct lttng_ht_node_ulong notify_sock_n = {};
	struct lttng_ht_node_u64 owner_id_n = {};
	/*
	 * This is a list of ust app session that, once the app is going into
	 * teardown mode, in the RCU call, each node in this list is removed and
	 * deleted.
	 *
	 * Element of the list are added when an application unregisters after each
	 * ht_del of ust_app_session associated to this app. This list is NOT used
	 * when a session is destroyed.
	 */
	std::list<ust_app_session *> sessions_to_teardown;
	/*
	 * Hash table containing ust_app_channel indexed by channel objd.
	 */
	struct lttng_ht *ust_objd = nullptr;
	/*
	 * Hash table containing ust_app_session indexed by objd.
	 */
	struct lttng_ht *ust_sessions_objd = nullptr;

	/*
	 * If this application is of the agent domain and this is non negative then
	 * a lookup MUST be done to acquire a read side reference to the
	 * corresponding agent app object. If the lookup fails, this should be set
	 * to a negative value indicating that the agent application is gone.
	 */
	int agent_app_sock = static_cast<int>(-1);
	/*
	 * Time at which the app is registred.
	 * Used for path creation
	 */
	time_t registration_time = static_cast<time_t>(-1);
	/*
	 * Event notifier
	 */
	struct {
		/*
		 * Handle to the lttng_ust object representing the event
		 * notifier group.
		 */
		struct lttng_ust_abi_object_data *object = nullptr;
		struct lttng_pipe *event_pipe = nullptr;
		struct lttng_ust_abi_object_data *counter = nullptr;
		struct lttng_ust_abi_object_data **counter_cpu = nullptr;
		int nr_counter_cpu = 0;
	} event_notifier_group;
	/*
	 * Hashtable indexing the application's event notifier rule's
	 * (ust_app_event_notifier_rule) by their token's value.
	 */
	struct lttng_ht *token_to_event_notifier_rule_ht = nullptr;

	lttng::sessiond::ust::ctl_field_quirks ctl_field_quirks() const;
};

/*
 * Due to a bug in g++ < 7.1, this specialization must be enclosed in the fmt namespace,
 * see https://gcc.gnu.org/bugzilla/show_bug.cgi?id=56480.
 */
namespace fmt {
template <>
struct formatter<ust_app> : formatter<std::string> {
	template <typename FormatContextType>
	typename FormatContextType::iterator format(const ust_app& app,
						    FormatContextType& ctx) const
	{
		return format_to(
			ctx.out(),
			"{{ procname = `{}`, ppid = {}, pid = {}, uid = {}, gid = {}, version = {}.{}, registration time = {} }}",
			app.name,
			app.ppid,
			app.pid,
			app.uid,
			app.gid,
			app.v_major,
			app.v_minor,
			lttng::utils::time_to_iso8601_str(app.registration_time));
	}
};
} /* namespace fmt */

#ifdef HAVE_LIBLTTNG_UST_CTL

int ust_app_register(struct ust_register_msg *msg, int sock);
int ust_app_register_done(struct ust_app *app);
int ust_app_version(struct ust_app *app);
void ust_app_unregister_by_socket(int sock);
int ust_app_start_trace_all(struct ltt_ust_session *usess);
int ust_app_stop_trace_all(struct ltt_ust_session *usess);
int ust_app_destroy_trace_all(struct ltt_ust_session *usess);
int ust_app_list_events(struct lttng_event **events);
int ust_app_list_event_fields(struct lttng_event_field **fields);
int ust_app_create_event_glb(struct ltt_ust_session *usess,
			     struct ltt_ust_channel *uchan,
			     struct ltt_ust_event *uevent);
int ust_app_disable_channel_glb(struct ltt_ust_session *usess, struct ltt_ust_channel *uchan);
int ust_app_enable_channel_glb(struct ltt_ust_session *usess, struct ltt_ust_channel *uchan);
int ust_app_enable_event_glb(struct ltt_ust_session *usess,
			     struct ltt_ust_channel *uchan,
			     struct ltt_ust_event *uevent);
int ust_app_disable_event_glb(struct ltt_ust_session *usess,
			      struct ltt_ust_channel *uchan,
			      struct ltt_ust_event *uevent);
int ust_app_add_ctx_channel_glb(struct ltt_ust_session *usess,
				struct ltt_ust_channel *uchan,
				struct ltt_ust_context *uctx);
void ust_app_global_update(struct ltt_ust_session *usess, struct ust_app *app);
void ust_app_global_update_all(struct ltt_ust_session *usess);
void ust_app_global_update_event_notifier_rules(struct ust_app *app);
void ust_app_global_update_all_event_notifier_rules();

void ust_app_clean_list();
int ust_app_ht_alloc();
struct ust_app *ust_app_find_by_pid(pid_t pid);
struct ust_app_stream *ust_app_alloc_stream();
int ust_app_recv_registration(int sock, struct ust_register_msg *msg);
int ust_app_recv_notify(int sock);
void ust_app_add(struct ust_app *app);
struct ust_app *ust_app_create(struct ust_register_msg *msg, int sock);
void ust_app_notify_sock_unregister(int sock);

enum lttng_error_code ust_app_snapshot_record(const struct ltt_ust_session *usess,
					      const struct consumer_output *output,
					      uint64_t nb_packets_per_stream);
uint64_t ust_app_get_size_one_more_packet_per_stream(const struct ltt_ust_session *usess,
						     uint64_t cur_nr_packets);
struct ust_app *ust_app_find_by_sock(int sock);
int ust_app_uid_get_channel_runtime_stats(uint64_t ust_session_id,
					  struct cds_list_head *buffer_reg_uid_list,
					  struct consumer_output *consumer,
					  uint64_t uchan_id,
					  int overwrite,
					  uint64_t *discarded,
					  uint64_t *lost);
int ust_app_pid_get_channel_runtime_stats(struct ltt_ust_session *usess,
					  struct ltt_ust_channel *uchan,
					  struct consumer_output *consumer,
					  int overwrite,
					  uint64_t *discarded,
					  uint64_t *lost);
int ust_app_regenerate_statedump_all(struct ltt_ust_session *usess);
enum lttng_error_code ust_app_create_channel_subdirectories(const struct ltt_ust_session *session);
int ust_app_release_object(struct ust_app *app, struct lttng_ust_abi_object_data *data);

int ust_app_setup_event_notifier_group(struct ust_app *app);

static inline int ust_app_supported()
{
	return 1;
}

ust_app_session *ust_app_lookup_app_session(const struct ltt_ust_session *usess,
					    const struct ust_app *app);
lttng::sessiond::ust::registry_session *
ust_app_get_session_registry(const ust_app_session::identifier& identifier);

lttng_ht *ust_app_get_all();

bool ust_app_supports_notifiers(const struct ust_app *app);
bool ust_app_supports_counters(const struct ust_app *app);

bool ust_app_get(ust_app& app);
void ust_app_put(ust_app *app);

void ust_app_notify_reclaimed_owner_ids(const std::vector<uint32_t>& owners);

using ust_app_reference =
	std::unique_ptr<ust_app, lttng::memory::create_deleter_class<ust_app, ust_app_put>::deleter>;

#else /* HAVE_LIBLTTNG_UST_CTL */

static inline int ust_app_destroy_trace_all(struct ltt_ust_session *usess __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_start_trace(struct ltt_ust_session *usess __attribute__((unused)),
				      struct ust_app *app __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_start_trace_all(struct ltt_ust_session *usess __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_stop_trace_all(struct ltt_ust_session *usess __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_list_events(struct lttng_event **events __attribute__((unused)))
{
	return -ENOSYS;
}

static inline int ust_app_list_event_fields(struct lttng_event_field **fields
					    __attribute__((unused)))
{
	return -ENOSYS;
}

static inline int ust_app_register(struct ust_register_msg *msg __attribute__((unused)),
				   int sock __attribute__((unused)))
{
	return -ENOSYS;
}

static inline int ust_app_register_done(struct ust_app *app __attribute__((unused)))
{
	return -ENOSYS;
}

static inline int ust_app_version(struct ust_app *app __attribute__((unused)))
{
	return -ENOSYS;
}

static inline void ust_app_unregister_by_socket(int sock __attribute__((unused)))
{
}

static inline void ust_app_clean_list(void)
{
}

static inline struct ust_app_list *ust_app_get_list(void)
{
	return NULL;
}

static inline struct ust_app *ust_app_get_by_pid(pid_t pid __attribute__((unused)))
{
	return NULL;
}

static inline int ust_app_ht_alloc(void)
{
	return 0;
}

static inline void ust_app_global_update(struct ltt_ust_session *usess __attribute__((unused)),
					 struct ust_app *app __attribute__((unused)))
{
}

static inline void ust_app_global_update_event_notifier_rules(struct ust_app *app
							      __attribute__((unused)))
{
}

static inline void ust_app_global_update_all_event_notifier_rules(void)
{
}

static inline int ust_app_setup_event_notifier_group(struct ust_app *app __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_disable_channel_glb(struct ltt_ust_session *usess __attribute__((unused)),
					      struct ltt_ust_channel *uchan __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_enable_channel_glb(struct ltt_ust_session *usess __attribute__((unused)),
					     struct ltt_ust_channel *uchan __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_create_event_glb(struct ltt_ust_session *usess __attribute__((unused)),
					   struct ltt_ust_channel *uchan __attribute__((unused)),
					   struct ltt_ust_event *uevent __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_disable_event_glb(struct ltt_ust_session *usess __attribute__((unused)),
					    struct ltt_ust_channel *uchan __attribute__((unused)),
					    struct ltt_ust_event *uevent __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_enable_event_glb(struct ltt_ust_session *usess __attribute__((unused)),
					   struct ltt_ust_channel *uchan __attribute__((unused)),
					   struct ltt_ust_event *uevent __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_add_ctx_channel_glb(struct ltt_ust_session *usess __attribute__((unused)),
					      struct ltt_ust_channel *uchan __attribute__((unused)),
					      struct ltt_ust_context *uctx __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_enable_event_pid(struct ltt_ust_session *usess __attribute__((unused)),
					   struct ltt_ust_channel *uchan __attribute__((unused)),
					   struct ltt_ust_event *uevent __attribute__((unused)),
					   pid_t pid __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_recv_registration(int sock __attribute__((unused)),
					    struct ust_register_msg *msg __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_recv_notify(int sock __attribute__((unused)))
{
	return 0;
}

static inline struct ust_app *ust_app_create(struct ust_register_msg *msg __attribute__((unused)),
					     int sock __attribute__((unused)))
{
	return NULL;
}

static inline void ust_app_add(struct ust_app *app __attribute__((unused)))
{
}

static inline void ust_app_notify_sock_unregister(int sock __attribute__((unused)))
{
}

static inline enum lttng_error_code
ust_app_snapshot_record(struct ltt_ust_session *usess __attribute__((unused)),
			const struct consumer_output *output __attribute__((unused)),
			uint64_t max_stream_size __attribute__((unused)))
{
	return LTTNG_ERR_UNK;
}

static inline unsigned int ust_app_get_nb_stream(struct ltt_ust_session *usess
						 __attribute__((unused)))
{
	return 0;
}

static inline void ust_app_update_event_notifier_error_count(struct lttng_trigger *lttng_trigger
							     __attribute__((unused)))
{
	return;
}

static inline int ust_app_supported(void)
{
	return 0;
}

static inline bool ust_app_supports_notifiers(const struct ust_app *app __attribute__((unused)))
{
	return false;
}

static inline bool ust_app_supports_counters(const struct ust_app *app __attribute__((unused)))
{
	return false;
}

static inline struct ust_app *ust_app_find_by_sock(int sock __attribute__((unused)))
{
	return NULL;
}

static inline struct ust_app *ust_app_find_by_pid(pid_t pid __attribute__((unused)))
{
	return NULL;
}

static inline uint64_t
ust_app_get_size_one_more_packet_per_stream(const struct ltt_ust_session *usess
					    __attribute__((unused)),
					    uint64_t cur_nr_packets __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_uid_get_channel_runtime_stats(uint64_t ust_session_id
							__attribute__((unused)),
							struct cds_list_head *buffer_reg_uid_list
							__attribute__((unused)),
							struct consumer_output *consumer
							__attribute__((unused)),
							int overwrite __attribute__((unused)),
							uint64_t uchan_id __attribute__((unused)),
							uint64_t *discarded __attribute__((unused)),
							uint64_t *lost __attribute__((unused)))
{
	return 0;
}

static inline int
ust_app_pid_get_channel_runtime_stats(struct ltt_ust_session *usess __attribute__((unused)),
				      struct ltt_ust_channel *uchan __attribute__((unused)),
				      struct consumer_output *consumer __attribute__((unused)),
				      int overwrite __attribute__((unused)),
				      uint64_t *discarded __attribute__((unused)),
				      uint64_t *lost __attribute__((unused)))
{
	return 0;
}

static inline int ust_app_regenerate_statedump_all(struct ltt_ust_session *usess
						   __attribute__((unused)))
{
	return 0;
}

static inline enum lttng_error_code
ust_app_create_channel_subdirectories(const struct ltt_ust_session *session __attribute__((unused)))
{
	return LTTNG_ERR_UNK;
}

static inline ust_app_session *ust_app_lookup_app_session(const ltt_ust_session *, const ust_app *)
{
	return nullptr;
}

static inline lttng::sessiond::ust::registry_session *
ust_app_get_session_registry(const ust_app_session::identifier&)
{
	return nullptr;
}

static inline lttng_ht *ust_app_get_all()
{
	return nullptr;
}

static inline int ust_app_release_object(struct ust_app *app __attribute__((unused)),
					 struct lttng_ust_abi_object_data *data
					 __attribute__((unused)))
{
	return 0;
}

static inline void ust_app_get(ust_app& app __attribute__((unused)))
{
}

static inline void ust_app_put(ust_app *app __attribute__((unused)))
{
}

static inline void ust_app_notify_reclaimed_owner_ids(const std::vector<uint32_t>& owners
						      __attribute__((unused)))
{
}

#endif /* HAVE_LIBLTTNG_UST_CTL */

#endif /* _LTT_UST_APP_H */
