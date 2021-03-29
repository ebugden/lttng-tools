/*
 * Copyright (C) 2021 EfficiOS Inc.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "../command.hpp"
#include "../exception.hpp"

#include "common/argpar/argpar.hpp"
#include "common/argpar-utils/argpar-utils.hpp"
#include "common/error.hpp"
#include "common/format.hpp"
#include "common/macros.hpp"
#include "vendor/optional.hpp"

#include <unistd.h>
#include <signal.h>
#include <string>
#include <sys/time.h>
#include <vector>

namespace {
/* Argument parsing */
enum {
	OPT_HELP,
	OPT_LIST_OPTIONS,
};

const struct argpar_opt_descr listen_options[] = {
	{OPT_HELP, 'h', "help", false},
	{OPT_LIST_OPTIONS, '\0', "list-options", false},
	ARGPAR_OPT_DESCR_SENTINEL,
};

/* Event field printing */
class event_field_value_formatter {
public:
	virtual ~event_field_value_formatter() = default;

	/*
	 * Format and print the captured event fields.
	 *
	 * The first level of fields is described by the user-defined
	 * capture descriptors. Those labels are printed alongside the first
	 * level of fields before recursively printing the remaining fields.
	 */
	void print_event_field_values(const struct lttng_condition *condition,
			const struct lttng_event_field_value *field_value) {
		enum lttng_event_field_value_type type =
			lttng_event_field_value_get_type(field_value);
		LTTNG_ASSERT(type == LTTNG_EVENT_FIELD_VALUE_TYPE_ARRAY);

		enum lttng_event_field_value_status status;
		unsigned int length;

		status = lttng_event_field_value_array_get_length(field_value, &length);
		if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
			ERR_FMT("Failed to get event field value array length.");
			throw std::runtime_error(lttng::format(
					"Failed to get event field value array length."));
		}

		this->format_array_start();

		for (unsigned int i = 0; i < length; i++) {
			const struct lttng_event_expr *expr =
				lttng_condition_event_rule_matches_get_capture_descriptor_at_index(condition, i);
			LTTNG_ASSERT(expr);

			print_one_event_expr(expr);

			const struct lttng_event_field_value *elem;
			status = lttng_event_field_value_array_get_element_at_index(field_value, i, &elem);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value array element.");
				throw std::runtime_error(lttng::format(
						"Failed to get event field value array element."));
			}

			this->format_array_element(elem);
		}

		this->format_array_end();
	}

protected:
	/* Function that can handle and print any supported captured data types. */
	void format_anything(const struct lttng_event_field_value *field_value) {
		enum lttng_event_field_value_type type;
		type = lttng_event_field_value_get_type(field_value);

		/*
		 * TODO: Remove the field_name variable here and in the
		 * functions.
		 *  - Hand the name down to the other function through a class
		 *     attribute if necessary later to facilitate machine
		 *     interface.
		 */

		switch (type) {
		/*
		 * The enumeration types (UNSIGNED_ENUM, SIGNED_ENUM) are
		 * treated the same as integers when printing since the current
		 * implementation does not provide a way to know the enumeration
		 * labels.
		 */
		case LTTNG_EVENT_FIELD_VALUE_TYPE_UNSIGNED_INT:
		case LTTNG_EVENT_FIELD_VALUE_TYPE_UNSIGNED_ENUM:
		{
			enum lttng_event_field_value_status status;
			std::uint64_t unsigned_int;

			status = lttng_event_field_value_unsigned_int_get_value(field_value, &unsigned_int);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value: unsigned integer.");
				throw std::runtime_error(lttng::format(
						"Failed to get event field value: unsigned integer."));
			}

			this->format_unsigned_int(unsigned_int);

			break;
		}

		case LTTNG_EVENT_FIELD_VALUE_TYPE_SIGNED_INT:
		case LTTNG_EVENT_FIELD_VALUE_TYPE_SIGNED_ENUM:
		{
			enum lttng_event_field_value_status status;
			std::int64_t signed_int;

			status = lttng_event_field_value_signed_int_get_value(field_value, &signed_int);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value: signed integer.");
				throw std::runtime_error(lttng::format(
						"Failed to get event field value: signed integer."));
			}

			this->format_signed_int(signed_int);

			break;
		}

		case LTTNG_EVENT_FIELD_VALUE_TYPE_REAL:
		{
			enum lttng_event_field_value_status status;
			double double_value;

			status = lttng_event_field_value_real_get_value(field_value, &double_value);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value: real.");
				throw std::runtime_error(lttng::format("Failed to get event field value: real."));
			}

			this->format_real(double_value);

			break;
		}

		case LTTNG_EVENT_FIELD_VALUE_TYPE_STRING:
		{
			enum lttng_event_field_value_status status;
			const char *str;

			status = lttng_event_field_value_string_get_value(field_value, &str);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value: string.");
				throw std::runtime_error(lttng::format(
						"Failed to get event field value: string."));
			}

			this->format_string(str);

			break;
		}

		case LTTNG_EVENT_FIELD_VALUE_TYPE_ARRAY:
		{
			this->format_array_start();

			enum lttng_event_field_value_status status;
			unsigned int length;

			status = lttng_event_field_value_array_get_length(
					field_value, &length);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value array length.");
				throw std::runtime_error(lttng::format(
						"Failed to get event field value array length."));
			}

			for (unsigned int i = 0; i < length; i++) {
				const struct lttng_event_field_value *elem;
				status = lttng_event_field_value_array_get_element_at_index(
						field_value, i, &elem);
				if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
					ERR_FMT("Failed to get event field value array element.");
					throw std::runtime_error(lttng::format(
							"Failed to get event field value array element."));
				}

				this->format_array_element(elem);
			}

			this->format_array_end();

			break;
		}

		default:
			ERR_FMT("Unexpected event field value type ({})", int(type));
			throw lttng::cli::unexpected_type("event field value", int(type), __FILE__, __func__, __LINE__);
		}
	}

	/*
	 * Unsigned and signed enumeration values are printed with the
	 * functions `format_unsigned_int()` and `format_signed_int()`
	 * respectively.
	 */
	virtual void format_unsigned_int(std::uint64_t value) = 0;
	virtual void format_signed_int(std::int64_t value) = 0;
	virtual void format_real(double value) = 0;
	virtual void format_string(const char* str) = 0;
	virtual void format_array_start(void) = 0;
	virtual void format_array_element(const struct lttng_event_field_value *elem) = 0;
	virtual void format_array_end(void) = 0;

private:
	void print_one_event_expr(const struct lttng_event_expr *event_expr)
	{
		enum lttng_event_expr_type type;
		type = lttng_event_expr_get_type(event_expr);

		switch (type) {
		case LTTNG_EVENT_EXPR_TYPE_EVENT_PAYLOAD_FIELD:
		{
			const char *name;

			name = lttng_event_expr_event_payload_field_get_name(event_expr);
			LTTNG_ASSERT(name != nullptr);

			fmt::print("{}: ", name);

			break;
		}

		case LTTNG_EVENT_EXPR_TYPE_CHANNEL_CONTEXT_FIELD:
		{
			const char *name;

			name = lttng_event_expr_channel_context_field_get_name(event_expr);
			LTTNG_ASSERT(name != nullptr);

			fmt::print("$ctx.{}: ", name);

			break;
		}

		case LTTNG_EVENT_EXPR_TYPE_APP_SPECIFIC_CONTEXT_FIELD:
		{
			const char *provider_name;
			const char *type_name;

			provider_name =
				lttng_event_expr_app_specific_context_field_get_provider_name(event_expr);
			LTTNG_ASSERT(provider_name != nullptr);
			type_name = lttng_event_expr_app_specific_context_field_get_type_name(event_expr);
			LTTNG_ASSERT(type_name != nullptr);

			fmt::print("$app.{}:{}: ", provider_name, type_name);

			break;
		}

		case LTTNG_EVENT_EXPR_TYPE_ARRAY_FIELD_ELEMENT:
		{
			unsigned int index;
			const struct lttng_event_expr *parent_expr;
			enum lttng_event_expr_status status;

			parent_expr = lttng_event_expr_array_field_element_get_parent_expr(event_expr);
			LTTNG_ASSERT(parent_expr != nullptr);

			print_one_event_expr(parent_expr);

			status = lttng_event_expr_array_field_element_get_index(event_expr, &index);
			LTTNG_ASSERT(status == LTTNG_EVENT_EXPR_STATUS_OK);

			fmt::print("[{}]: ", index);

			break;
		}

		default:
			ERR_FMT("Unexpected event expression type ({})", int(type));
			throw lttng::cli::unexpected_type("event expression", int(type), __FILE__, __func__, __LINE__);
		}
	}
};

class event_field_value_human_formatter : public event_field_value_formatter {
protected:
	void format_unsigned_int(std::uint64_t value) {
		fmt::print("{}\n", value);
	}

	void format_signed_int(std::int64_t value) {
		fmt::print("{}\n", value);
	}

	void format_real(double value) {
		fmt::print("{}\n", value);
	}

	void format_string(const char* str) {
		fmt::print("{}\n", str);
	}

	void format_array_start(void) {
		fmt::print("[\n");
	}

	void format_array_element(const struct lttng_event_field_value *elem) {
		event_field_value_formatter::format_anything(elem);
	}

	void format_array_end(void) {
		fmt::print("]\n");
	}
};

class event_field_value_machine_formatter : public event_field_value_formatter {
	// TODO!
};

/* Notification printing */
class trigger_notification_formatter{
public:
	virtual ~trigger_notification_formatter() = default;

	void print_notification(struct lttng_notification *notification) {
		const struct lttng_evaluation *evaluation =
				lttng_notification_get_evaluation(notification);
		LTTNG_ASSERT(evaluation != nullptr);
		const enum lttng_condition_type type =
				lttng_evaluation_get_type(evaluation);

		switch (type) {
		case LTTNG_CONDITION_TYPE_SESSION_CONSUMED_SIZE:
		{
			uint64_t consumed_size;

			enum lttng_evaluation_status evaluation_status =
					lttng_evaluation_session_consumed_size_get_consumed_size(
							evaluation, &consumed_size);
			if (evaluation_status != LTTNG_EVALUATION_STATUS_OK) {
				ERR_FMT("Failed to get session consumed size.");
				throw std::runtime_error(lttng::format("Failed to get session consumed size."));
			}

			this->print_session_consumed_size_notification(consumed_size);

			break;
		}

		case LTTNG_CONDITION_TYPE_BUFFER_USAGE_LOW:
			// FIXME: is there a way to get information about which session,
			// the buffer usage vs buffer size?
			this->print_buffer_usage_low_notification();
			break;

		case LTTNG_CONDITION_TYPE_BUFFER_USAGE_HIGH:
			// FIXME: is there a way to get information about which session,
			// the buffer usage vs buffer size?
			this->print_buffer_usage_high_notification();
			break;

		case LTTNG_CONDITION_TYPE_SESSION_ROTATION_ONGOING:
			this->print_session_rotation_ongoing_notification();
			break;

		case LTTNG_CONDITION_TYPE_SESSION_ROTATION_COMPLETED:
			this->print_sessiond_rotation_completed_notification();
			break;

		case LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES:
		{
			/* Retrieve values captured alongside the trigger */
			const struct lttng_event_field_value *captures = NULL;
			enum lttng_evaluation_event_rule_matches_status evaluation_event_rule_matches_status;
			evaluation_event_rule_matches_status =
					lttng_evaluation_event_rule_matches_get_captured_values(
							evaluation, &captures);
			if (evaluation_event_rule_matches_status !=
							LTTNG_EVALUATION_EVENT_RULE_MATCHES_STATUS_OK &&
					evaluation_event_rule_matches_status !=
							LTTNG_EVALUATION_EVENT_RULE_MATCHES_STATUS_NONE) {
				ERR_FMT("Failed to get captured values from on-event notification.");
				throw std::runtime_error(lttng::format(
						"Failed to get captured values from on-event notification."));
			}

			this->print_event_notification(notification, captures);

			break;
		}
		default:
			ERR_FMT("Unknown notification type ({})", int(type));
			throw lttng::cli::unexpected_type("notification", int(type), __FILE__, __func__, __LINE__);
		}
	}

protected:
	virtual void print_session_consumed_size_notification(uint64_t consumed_size) = 0;
	virtual void print_buffer_usage_low_notification(void) = 0;
	virtual void print_buffer_usage_high_notification(void) = 0;
	virtual void print_session_rotation_ongoing_notification(void) = 0;
	virtual void print_sessiond_rotation_completed_notification(void) = 0;
	virtual void print_event_notification(struct lttng_notification *notification,
			const struct lttng_event_field_value *captures) = 0;
};

class trigger_notification_human_formatter : public trigger_notification_formatter {
protected:
	void print_session_consumed_size_notification(uint64_t consumed_size) {
		fmt::print("Session consumed size: {} bytes\n", consumed_size);
	}

	void print_buffer_usage_low_notification(void) {
		fmt::print("Buffer usage low\n");
	}

	void print_buffer_usage_high_notification(void) {
		fmt::print("Buffer usage high\n");
	}

	void print_session_rotation_ongoing_notification(void) {
		fmt::print("Session rotation ongoing\n");
	}

	void print_sessiond_rotation_completed_notification(void) {
		fmt::print("Session rotation completed\n");
	}

	void print_event_notification(struct lttng_notification *notification,
			const struct lttng_event_field_value *captures)
	{
		/* Print trigger that sent the notification. */
		const struct lttng_trigger *origin_trigger;
		origin_trigger = lttng_notification_get_trigger(notification);
		LTTNG_ASSERT(origin_trigger != nullptr);

		const char *origin_trigger_name;
		enum lttng_trigger_status trigger_status;
		trigger_status = lttng_trigger_get_name(origin_trigger, &origin_trigger_name);
		if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
			ERR_FMT("Failed to get name origin trigger from notification.");
			throw std::runtime_error(lttng::format(
					"Failed to get name origin trigger from notification."));
		}

		fmt::print("Event (trigger {})\n", origin_trigger_name);

		const struct lttng_condition *condition = lttng_notification_get_condition(notification);
		LTTNG_ASSERT(condition != nullptr);

		/* Check if we expect captured values to print */
		enum lttng_condition_status condition_status;
		unsigned int expected_capture_field_count;
		condition_status = lttng_condition_event_rule_matches_get_capture_descriptor_count(
			condition, &expected_capture_field_count);
		if (condition_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get capture descriptor count from on-event notification.");
			throw std::runtime_error(lttng::format(
					"Failed to get capture descriptor count from on-event notification."));
		}

		if (expected_capture_field_count == 0) {
			/* There are no captured values to print. */
			return;
		}

		/* Print values captured alongside trigger */
		if (captures) {
			try {
				// TODO: Add machine interface
				event_field_value_human_formatter human_formatter;
				human_formatter.print_event_field_values(condition, captures);
			} catch (const lttng::cli::unexpected_type& type_exception) {
				ERR_FMT("Failed to print values captured alongside trigger.");
			} catch (const std::runtime_error& runtime_exception) {
				ERR_FMT("Failed to print values captured alongside trigger.");
			}
		}
	}
};

/* Trigger subscription */
bool trigger_action_has_notify(const struct lttng_action *action) {
	enum lttng_action_type action_type;
	action_type = lttng_action_get_type(action);

	bool has_notify = false;

	if (action_type == LTTNG_ACTION_TYPE_NOTIFY) {
		has_notify = true;
	} else if (action_type == LTTNG_ACTION_TYPE_LIST) {
		/* More than one action is associated with the trigger */
		unsigned int count;
		enum lttng_action_status status =
				lttng_action_list_get_count(action, &count);

		if (status != LTTNG_ACTION_STATUS_OK) {
			ERR_FMT("Failed to get action count from action group.");
			throw std::runtime_error(lttng::format(
					"Failed to get action count from action group."));
		}

		for (unsigned int i = 0; i < count; i++) {
			const struct lttng_action *action_item =
					lttng_action_list_get_at_index(
							action, i);
			LTTNG_ASSERT(action_item != nullptr);

			has_notify = trigger_action_has_notify(action_item);

			if (has_notify) {
				break;
			}
		}
	}

	return has_notify;
}

void subscribe(struct lttng_notification_channel *notification_channel,
		const struct lttng_trigger *trigger,
		bool check_for_notify_action)
{
	const char *trigger_name;
	enum lttng_trigger_status trigger_status;

	trigger_status = lttng_trigger_get_name(trigger, &trigger_name);
	if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
		ERR_FMT("Failed to get trigger name.");
		throw std::runtime_error(lttng::format("Failed to get trigger name."));
	}

	const struct lttng_action *action;
	action = lttng_trigger_get_const_action(trigger);
	LTTNG_ASSERT(action != NULL);

	if (check_for_notify_action) {
		bool trigger_has_notify = false;
		trigger_has_notify = trigger_action_has_notify(action);

		if (!trigger_has_notify) {
			WARN_FMT("Subscribing to trigger `{}`, but it does not contain a notify action.",
					trigger_name);
		}
	}

	const struct lttng_condition *condition;
	condition = lttng_trigger_get_const_condition(trigger);
	LTTNG_ASSERT(condition != NULL);

	enum lttng_notification_channel_status notification_channel_status;
	notification_channel_status = lttng_notification_channel_subscribe(
			notification_channel, condition);
	if (notification_channel_status != LTTNG_NOTIFICATION_CHANNEL_STATUS_OK) {
		ERR_FMT("Failed to subscribe to notifications of trigger `{}`.",
				trigger_name);
		throw lttng::cli::trigger_notification_subscription_error(trigger_name, __FILE__, __func__,
				__LINE__);
	}
}
} /* namespace */

int cmd_listen(int argc, const char **argv)
{
	/* Skip "listen" in the list of arguments */
	int my_argc = argc - 1;
	const char **my_argv = argv + 1;

	/* Process the command arguments */
	int ret;
	std::vector<std::string> requested_trigger_names;
	argpar::Iter<nonstd::optional<argpar::Item>> argpar_iter(my_argc, my_argv, listen_options);

	while (true) {
		nonstd::optional<argpar::Item> argpar_item = argpar_iter.next();

		if (!argpar_item) {
			/* No more arguments to parse. */
			break;
		}

		if (argpar_item->isOpt()) {
			/* If the argument is a command option */
			const argpar::OptItemView option = argpar_item->asOpt();
			switch (option.descr().id) {
				case OPT_HELP:
					SHOW_HELP();
					return 0;

				case OPT_LIST_OPTIONS:
					list_cmd_options_argpar(stdout, listen_options);
					return 0;
			}
		} else {
			/* If the argument is a trigger */
			const argpar::NonOptItemView trigger_name = argpar_item->asNonOpt();
			requested_trigger_names.push_back(trigger_name.arg());
		}
	}

	/* Determine whether to listen to all triggers */
	bool listen_all_triggers = false;
	unsigned int trigger_name_count;
	trigger_name_count = requested_trigger_names.size();
	if (trigger_name_count == 0) {
		listen_all_triggers = true;
	}

	/*
	 * TODO: Move the following two declarations as low as possible
	 * after gotos no longer used to destroy them.
	 */
	struct lttng_notification_channel *notification_channel = NULL;
	struct lttng_notification *notification = NULL;

	/* Fetch all triggers from sessiond. */
	struct lttng_triggers *all_triggers = NULL;
	enum lttng_error_code error_code;
	error_code = lttng_list_triggers(&all_triggers);
	if (error_code != LTTNG_OK) {
		ERR_FMT("Failed to list triggers.");
		goto error;
	}

	unsigned int all_triggers_count;
	enum lttng_trigger_status trigger_status;
	trigger_status = lttng_triggers_get_count(
			all_triggers, &all_triggers_count);
	if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
		ERR_FMT("Failed to get trigger count.");
		goto error;
	}

	notification_channel = lttng_notification_channel_create(
			lttng_session_daemon_notification_endpoint);
	if (!notification_channel) {
		ERR_FMT("Failed to create notification channel.");
		goto error;
	}

	/* Subscribe to notifications from the triggers we want */
	bool check_for_notify_action;
	if (listen_all_triggers) {
		const struct lttng_trigger *trigger;

		for (unsigned int trigger_i = 0; trigger_i < all_triggers_count;
				trigger_i++) {
			trigger = lttng_triggers_get_at_index(all_triggers, trigger_i);

			check_for_notify_action = false;
			try {
				subscribe(notification_channel, trigger, check_for_notify_action);
			} catch (const lttng::cli::trigger_notification_subscription_error& subscription_exception) {
				ERR_FMT(subscription_exception.what());
				goto error;
			}
		}

		// FIXME: this is WARN so that it's on stderr, such that a
		// program reading the notifications output by "lttng listen"
		// doesn't receive it.  It's not really a warning though.
		// Another option would be to output it using MSG, but
		// have a -q / --quiet option to suppress it.
		WARN_FMT("Listening for notifications from all existing triggers.");
	} else {
		/*
		 * Go through each requested trigger name, then through each
		 * existing trigger.
		 */
		for (unsigned int requested_trigger_name_i = 0;
				requested_trigger_name_i < trigger_name_count;
				requested_trigger_name_i++) {
			const std::string requested_trigger_name =
					requested_trigger_names[requested_trigger_name_i];
			unsigned int trigger_i;
			const struct lttng_trigger *trigger;

			for (trigger_i = 0; trigger_i < all_triggers_count;
					trigger_i++) {
				trigger = lttng_triggers_get_at_index(
						all_triggers, trigger_i);
				const char *trigger_name_char_array;

				trigger_status = lttng_trigger_get_name(
						trigger, &trigger_name_char_array);
				if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
					ERR_FMT("Failed to get trigger name.");
					goto error;
				}

				std::string trigger_name = trigger_name_char_array;
				if (trigger_name.compare(requested_trigger_name) ==	0) {
					break;
				}
			}

			if (trigger_i == all_triggers_count) {
				ERR_FMT("Couldn't find a trigger with name `{}`.",
						requested_trigger_name);
				goto error;
			}

			check_for_notify_action = true;
			try {
				subscribe(notification_channel, trigger, check_for_notify_action);
			} catch (const lttng::cli::trigger_notification_subscription_error& subscription_exception) {
				ERR_FMT(subscription_exception.what());
				goto error;
			}
		}

		WARN_FMT("Listening on the specified triggers.");
	}

	/* Signal that listen is ready (i.e. subscriptions are complete). */
	::kill(getppid(), SIGUSR1);

	/* Loop forever, listening for trigger notifications. */
	for (;;) {
		enum lttng_notification_channel_status
				notification_channel_status;

		lttng_notification_destroy(notification);
		notification = nullptr;
		notification_channel_status =
				lttng_notification_channel_get_next_notification(
						notification_channel,
						&notification);

		/*
		 * TODO: Move the human_formatter variable creation in to the
		 * case when the gotos are gone. At the moment it needs to be
		 * here because otherwise compiler complains that initialization
		 * can be skipped.
		 */
		trigger_notification_human_formatter human_formatter;
		switch (notification_channel_status) {
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_NOTIFICATIONS_DROPPED:
			fmt::print("Dropped notification.\n");
			break;
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_INTERRUPTED:
			ret = 0;
			goto end;
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_OK:
			LTTNG_ASSERT(notification != nullptr);

			try {
				// TODO: Add machine interface
				human_formatter.print_notification(notification);
			} catch (const lttng::cli::unexpected_type& type_exception) {
				goto error;
			} catch (const std::runtime_error& runtime_exception) {
				goto error;
			}

			break;
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_CLOSED:
			fmt::print("Notification channel was closed by peer.\n");
			ret = 0;
			goto end;
		default:
			ERR_FMT("A communication error occurred on the notification channel.");
			goto error;
		}
	}

	ret = 0;
	goto end;

error:
	ret = 1;

end:
	lttng_triggers_destroy(all_triggers);
	lttng_notification_channel_destroy(notification_channel);
	lttng_notification_destroy(notification);
	return ret;
}
