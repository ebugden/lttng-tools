/*
 * Copyright (C) 2021 EfficiOS Inc.
 * Copyright (C) 2025 Erica Bugden <ebugden@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "../command.hpp"
#include "../exception.hpp"

#include <common/argpar-utils/argpar-utils.hpp>
#include <common/error.hpp>
#include <common/format.hpp>
#include <common/macros.hpp>
#include <common/make-unique.hpp>

#include <vendor/argpar/argpar.hpp>
#include <vendor/nlohmann/json.hpp>
#include <vendor/optional.hpp>

#include <ctl-utils/utils.hpp>
#include <iostream> // Just for testing
#include <signal.h>
#include <string>
#include <sys/time.h>
#include <unistd.h>
#include <vector>

namespace json = nlohmann;

namespace {
/* Argument parsing */
enum {
	OPT_HELP,
	OPT_LIST_OPTIONS,
	OPT_SIGNAL_WHEN_READY,
};

const struct argpar_opt_descr listen_options[] = {
	{ OPT_HELP, 'h', "help", false },
	{ OPT_LIST_OPTIONS, '\0', "list-options", false },
	{ OPT_SIGNAL_WHEN_READY, '\0', "signal-when-ready", false },
	ARGPAR_OPT_DESCR_SENTINEL,
};

/* Event field printing */
class event_field_value_formatter {
public:
	virtual ~event_field_value_formatter() = default;

	/*
	 * Format and print the captured event fields.
	 *
	 * The first level of fields can be described by the
	 * user-defined capture descriptors. Those labels are printed
	 * alongside the first level of fields before recursively
	 * printing the remaining fields.
	 *
	 * The remaining fields are not labeled since notifications do
	 * not provide the descriptions for these — we do not know their
	 * field names from the tracepoint description.
	 */
	void print_event_field_values(const struct lttng_condition *condition,
				      const struct lttng_event_field_value *field_value)
	{
		/* The top level of values should always be an array. */
		const enum lttng_event_field_value_type type =
			lttng_event_field_value_get_type(field_value);
		LTTNG_ASSERT(type == LTTNG_EVENT_FIELD_VALUE_TYPE_ARRAY);

		enum lttng_event_field_value_status status;
		unsigned int length;

		status = lttng_event_field_value_array_get_length(field_value, &length);
		if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
			ERR_FMT("Failed to get event field value array length.");
			LTTNG_THROW_ERROR(
				lttng::format("Failed to get event field value array length."));
		}

		this->format_array_start();

		for (unsigned int i = 0; i < length; i++) {
			const struct lttng_event_expr *event_expression =
				lttng_condition_event_rule_matches_get_capture_descriptor_at_index(
					condition, i);
			LTTNG_ASSERT(event_expression);

			print_one_event_expr(event_expression);

			const struct lttng_event_field_value *element;
			status = lttng_event_field_value_array_get_element_at_index(
				field_value, i, &element);
			LTTNG_ASSERT(status == LTTNG_EVENT_FIELD_VALUE_STATUS_OK);
			LTTNG_ASSERT(element != nullptr);

			this->format_array_element(element);
		}

		this->format_array_end();
	}

protected:
	/* Function that can handle and print any supported captured data types. */
	void format_anything(const struct lttng_event_field_value *field_value)
	{
		const enum lttng_event_field_value_type type =
			lttng_event_field_value_get_type(field_value);

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

			status = lttng_event_field_value_unsigned_int_get_value(field_value,
										&unsigned_int);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value: unsigned integer.");
				LTTNG_THROW_ERROR(lttng::format(
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

			status = lttng_event_field_value_signed_int_get_value(field_value,
									      &signed_int);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value: signed integer.");
				LTTNG_THROW_ERROR(lttng::format(
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
				LTTNG_THROW_ERROR(
					lttng::format("Failed to get event field value: real."));
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
				LTTNG_THROW_ERROR(
					lttng::format("Failed to get event field value: string."));
			}

			this->format_string(str);

			break;
		}

		case LTTNG_EVENT_FIELD_VALUE_TYPE_ARRAY:
		{
			this->format_array_start();

			enum lttng_event_field_value_status status;
			unsigned int length;

			status = lttng_event_field_value_array_get_length(field_value, &length);
			if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
				ERR_FMT("Failed to get event field value array length.");
				LTTNG_THROW_ERROR(lttng::format(
					"Failed to get event field value array length."));
			}

			for (unsigned int i = 0; i < length; i++) {
				const struct lttng_event_field_value *element;
				status = lttng_event_field_value_array_get_element_at_index(
					field_value, i, &element);
				if (status != LTTNG_EVENT_FIELD_VALUE_STATUS_OK) {
					ERR_FMT("Failed to get event field value array element.");
					LTTNG_THROW_ERROR(lttng::format(
						"Failed to get event field value array element."));
				}

				this->format_array_element(element);
			}

			this->format_array_end();

			break;
		}

		default:
			ERR_FMT("Unexpected event field value type ({})", int(type));
			LTTNG_THROW_CLI_UNEXPECTED_TYPE("event field value", int(type));
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
	virtual void format_string(const char *str) = 0;
	virtual void format_array_start() = 0;
	virtual void format_array_element(const struct lttng_event_field_value *element) = 0;
	virtual void format_array_end() = 0;

private:
	void print_one_event_expr(const struct lttng_event_expr *event_expression)
	{
		enum lttng_event_expr_type type;
		type = lttng_event_expr_get_type(event_expression);

		switch (type) {
		case LTTNG_EVENT_EXPR_TYPE_EVENT_PAYLOAD_FIELD:
		{
			const char *name;

			name = lttng_event_expr_event_payload_field_get_name(event_expression);
			LTTNG_ASSERT(name != nullptr);

			fmt::print("{}: ", name);

			break;
		}

		case LTTNG_EVENT_EXPR_TYPE_CHANNEL_CONTEXT_FIELD:
		{
			const char *name;

			name = lttng_event_expr_channel_context_field_get_name(event_expression);
			LTTNG_ASSERT(name != nullptr);

			fmt::print("$ctx.{}: ", name);

			break;
		}

		case LTTNG_EVENT_EXPR_TYPE_APP_SPECIFIC_CONTEXT_FIELD:
		{
			const char *provider_name;
			const char *type_name;

			provider_name =
				lttng_event_expr_app_specific_context_field_get_provider_name(
					event_expression);
			LTTNG_ASSERT(provider_name != nullptr);

			type_name = lttng_event_expr_app_specific_context_field_get_type_name(
				event_expression);
			LTTNG_ASSERT(type_name != nullptr);

			fmt::print("$app.{}:{}: ", provider_name, type_name);

			break;
		}

		case LTTNG_EVENT_EXPR_TYPE_ARRAY_FIELD_ELEMENT:
		{
			const struct lttng_event_expr *parent_expression =
				lttng_event_expr_array_field_element_get_parent_expr(
					event_expression);
			LTTNG_ASSERT(parent_expression != nullptr);

			print_one_event_expr(parent_expression);

			unsigned int index;
			enum lttng_event_expr_status status =
				lttng_event_expr_array_field_element_get_index(event_expression,
									       &index);
			LTTNG_ASSERT(status == LTTNG_EVENT_EXPR_STATUS_OK);

			fmt::print("[{}]: ", index);

			break;
		}

		default:
			ERR_FMT("Unexpected event expression type ({})", int(type));
			LTTNG_THROW_CLI_UNEXPECTED_TYPE("event expression", int(type));
		}
	}
};

class event_field_value_human_formatter : public event_field_value_formatter {
protected:
	void format_unsigned_int(std::uint64_t value) override
	{
		fmt::println("{}", value);
	}

	void format_signed_int(std::int64_t value) override
	{
		fmt::println("{}", value);
	}

	void format_real(double value) override
	{
		fmt::println("{}", value);
	}

	void format_string(const char *str) override
	{
		fmt::println("{}", str);
	}

	void format_array_start() override
	{
		fmt::println("[");
	}

	void format_array_element(const struct lttng_event_field_value *element) override
	{
		this->format_anything(element);
	}

	void format_array_end() override
	{
		fmt::println("]");
	}
};

// TODO: Print values in machine format rather than human-readable
class event_field_value_machine_formatter : public event_field_value_formatter {
protected:
	void format_unsigned_int(std::uint64_t value) override
	{
		fmt::println("{}", value);
	}

	void format_signed_int(std::int64_t value) override
	{
		fmt::println("{}", value);
	}

	void format_real(double value) override
	{
		fmt::println("{}", value);
	}

	void format_string(const char *str) override
	{
		fmt::println("{}", str);
	}

	void format_array_start() override
	{
		fmt::println("[");
	}

	void format_array_element(const struct lttng_event_field_value *element) override
	{
		this->format_anything(element);
	}

	void format_array_end() override
	{
		fmt::println("]");
	}
};

/* Notification printing */
class trigger_notification_formatter {
public:
	explicit trigger_notification_formatter(struct lttng_notification *trigger_notification) :
		_trigger_notification(trigger_notification)
	{
		/* Unpack trigger notification */

		/*
		 * Retrieve the condition that determined when the
		 * trigger notification was sent.
		 *
		 * The 'condition' contains the labels (capture descriptors)
		 * that describe any values captured alongside the trigger.
		 */
		_trigger_condition = lttng_notification_get_condition(trigger_notification);
		LTTNG_ASSERT(_trigger_condition != nullptr);

		_trigger_evaluation = lttng_notification_get_evaluation(trigger_notification);
		LTTNG_ASSERT(_trigger_evaluation != nullptr);

		_trigger_type = lttng_condition_get_type(_trigger_condition);
		LTTNG_ASSERT(_trigger_type == lttng_evaluation_get_type(_trigger_evaluation));
	}

	virtual ~trigger_notification_formatter() = default;

	void print_trigger_notification()
	{
		switch (_trigger_type) {
		case LTTNG_CONDITION_TYPE_SESSION_CONSUMED_SIZE:
		{
			/*
			 * “Recording session consumed data size becomes
			 * greater than” trigger type
			 */
			_unpack_and_print_session_consumed_size_trigger_notification();
			break;
		}

		case LTTNG_CONDITION_TYPE_BUFFER_USAGE_HIGH:
		case LTTNG_CONDITION_TYPE_BUFFER_USAGE_LOW:
			/*
			 * “Channel buffer usage becomes greater/less
			 * than” trigger type
			 */
			_unpack_and_print_buffer_usage_trigger_notification();
			break;

		case LTTNG_CONDITION_TYPE_SESSION_ROTATION_ONGOING:
			_print_session_rotation_ongoing_trigger_notification();
			break;

		case LTTNG_CONDITION_TYPE_SESSION_ROTATION_COMPLETED:
			_print_session_rotation_completed_trigger_notification();
			break;

		case LTTNG_CONDITION_TYPE_EVENT_RULE_MATCHES:
		{
			_print_event_rule_matches_trigger_notification();
			break;
		}
		default:
			ERR_FMT("Unknown notification type ({})", int(_trigger_type));
			LTTNG_THROW_CLI_UNEXPECTED_TYPE("notification", int(_trigger_type));
		}

		std::fflush(stdout);
	}

protected:
	struct lttng_notification *_trigger_notification = nullptr;
	std::string _origin_trigger_name;
	const struct lttng_condition *_trigger_condition = nullptr;
	const struct lttng_evaluation *_trigger_evaluation = nullptr;
	enum lttng_condition_type _trigger_type = LTTNG_CONDITION_TYPE_UNKNOWN;

	const struct lttng_event_field_value *_trigger_captured_values = nullptr;

private:
	void _unpack_and_print_session_consumed_size_trigger_notification()
	{
		const char *session_name_char_array;
		enum lttng_condition_status get_session_name_status =
			lttng_condition_session_consumed_size_get_session_name(
				_trigger_condition, &session_name_char_array);
		if (get_session_name_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get session name associated with the trigger.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get session name associated with the trigger."));
		}
		LTTNG_ASSERT(session_name_char_array != nullptr);
		std::string session_name = std::string(session_name_char_array);

		uint64_t consumed_threshold_bytes;
		enum lttng_condition_status get_threshold_status =
			lttng_condition_session_consumed_size_get_threshold(
				_trigger_condition, &consumed_threshold_bytes);
		if (get_threshold_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get threshold for data consumed by the session.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get threshold for data consumed by the session."));
		}
		LTTNG_ASSERT(consumed_threshold_bytes);

		uint64_t consumed_size;
		const enum lttng_evaluation_status get_consumed_size_status =
			lttng_evaluation_session_consumed_size_get_consumed_size(
				_trigger_evaluation, &consumed_size);
		if (get_consumed_size_status != LTTNG_EVALUATION_STATUS_OK) {
			ERR_FMT("Failed to get size of data consumed by the session.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get size of data consumed by the session."));
		}
		LTTNG_ASSERT(consumed_size);

		_print_session_consumed_size_trigger_notification(
			session_name, consumed_threshold_bytes, consumed_size);
	}

	void _unpack_and_print_buffer_usage_trigger_notification()
	{
		const char *channel_name_char_array;
		enum lttng_condition_status get_channel_name_status =
			lttng_condition_buffer_usage_get_channel_name(_trigger_condition,
								      &channel_name_char_array);
		if (get_channel_name_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get channel name associated with the trigger.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get channel name associated with the trigger."));
		}
		LTTNG_ASSERT(channel_name_char_array != nullptr);
		std::string channel_name = std::string(channel_name_char_array);

		const char *session_name_char_array;
		enum lttng_condition_status get_session_name_status =
			lttng_condition_buffer_usage_get_session_name(_trigger_condition,
								      &session_name_char_array);
		if (get_session_name_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get session name associated with the trigger.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get session name associated with the trigger."));
		}
		LTTNG_ASSERT(session_name_char_array != nullptr);
		std::string session_name = std::string(session_name_char_array);

		enum lttng_domain_type tracer_domain_type = LTTNG_DOMAIN_NONE;
		enum lttng_condition_status get_domain_type_status =
			lttng_condition_buffer_usage_get_domain_type(_trigger_condition,
								     &tracer_domain_type);
		if (get_domain_type_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get tracer domain type associated with the trigger.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get tracer domain type associated with the trigger."));
		}
		LTTNG_ASSERT(tracer_domain_type != LTTNG_DOMAIN_NONE);

		/* Get trigger's buffer usage threshold */
		/*
		 * A buffer usage trigger's threshold can be expressed
		 * either as a ratio or a number of bytes.
		 */
		bool is_threshold_ratio = true;
		double threshold_ratio;
		enum lttng_condition_status get_threshold_status =
			lttng_condition_buffer_usage_get_threshold_ratio(_trigger_condition,
									 &threshold_ratio);
		uint64_t threshold;
		if (get_threshold_status == LTTNG_CONDITION_STATUS_UNSET) {
			/*
			 * The trigger has a specific threshold set
			 * instead of a ratio.
			 */
			is_threshold_ratio = false;
			get_threshold_status = lttng_condition_buffer_usage_get_threshold(
				_trigger_condition, &threshold);
		}
		if (get_threshold_status != LTTNG_CONDITION_STATUS_OK) {
			ERR_FMT("Failed to get buffer usage threshold associated with the trigger.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get tracer buffer usage threshold associated with the trigger."));
		}

		/* Get trigger's buffer usage */
		// TODO

		if (_trigger_type == LTTNG_CONDITION_TYPE_BUFFER_USAGE_LOW) {
			_print_buffer_usage_low_trigger_notification(
				channel_name,
				session_name,
				tracer_domain_type /*, threshold, usage*/);
		} else if (_trigger_type == LTTNG_CONDITION_TYPE_BUFFER_USAGE_HIGH) {
			_print_buffer_usage_high_trigger_notification(
				channel_name,
				session_name,
				tracer_domain_type /*, threshold, usage*/);
		}
	}

	void _print_event_rule_matches_trigger_notification()
	{
		/*
		 * Retrieve the name of the trigger from which the
		 * notification originated.
		 */
		const struct lttng_trigger *origin_trigger;
		origin_trigger = lttng_notification_get_trigger(_trigger_notification);
		LTTNG_ASSERT(origin_trigger != nullptr);

		const char *origin_trigger_name_char_array;
		enum lttng_trigger_status trigger_status;
		trigger_status =
			lttng_trigger_get_name(origin_trigger, &origin_trigger_name_char_array);
		if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
			ERR_FMT("Failed to get name origin trigger from notification.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get name origin trigger from notification."));
		}

		_origin_trigger_name = std::string(origin_trigger_name_char_array);

		/* Retrieve values captured alongside the trigger */
		const enum lttng_evaluation_event_rule_matches_status
			evaluation_event_rule_matches_status =
				lttng_evaluation_event_rule_matches_get_captured_values(
					_trigger_evaluation, &(_trigger_captured_values));

		if (evaluation_event_rule_matches_status !=
			    LTTNG_EVALUATION_EVENT_RULE_MATCHES_STATUS_OK &&
		    evaluation_event_rule_matches_status !=
			    LTTNG_EVALUATION_EVENT_RULE_MATCHES_STATUS_NONE) {
			ERR_FMT("Failed to get captured values from on-event notification.");
			LTTNG_THROW_ERROR(lttng::format(
				"Failed to get captured values from on-event notification."));
		}

		_print_event_rule_matches_trigger_notification_output_specific();
	}

	virtual void _print_event_rule_matches_trigger_notification_output_specific() = 0;

	/*
	 * Moved the non-event notification printing code to the parent
	 * class for now since there currently is no distinction between
	 * how things are printed in human/mi.
	 *
	 * TODO: Distinguish between how they're printed in mi/human?
	 */
	void _print_session_consumed_size_trigger_notification(std::string session_name,
							       uint64_t consumed_threshold_bytes,
							       uint64_t consumed_size)
	{
		fmt::println(
			"Trigger notification: Session `{}` has consumed {} bytes of data. Notification threshold: {} bytes.",
			session_name,
			consumed_size,
			consumed_threshold_bytes);
	}

	void _print_buffer_usage_low_trigger_notification(
		std::string channel_name,
		std::string session_name,
		lttng_domain_type tracer_domain_type /*, threshold, usage*/)
	{
		double threshold_placeholder = 0;
		double usage_placeholder = 0;
		fmt::println(
			"Trigger notification: The amount of data in a buffer belonging to channel `{}` of session `{}` is lower than the set minimum threshold of {} bytes. Buffer usage: {} bytes)",
			channel_name,
			session_name,
			threshold_placeholder,
			usage_placeholder);
	}

	void _print_buffer_usage_high_trigger_notification(
		std::string channel_name,
		std::string session_name,
		lttng_domain_type tracer_domain_type /*, threshold, usage*/)
	{
		double threshold_placeholder = 0;
		double usage_placeholder = 0;
		fmt::println(
			"Trigger notification: The amount of data in a buffer belonging to channel `{}` of session `{}` is higher than the set maximum threshold of {} bytes. Buffer usage: {} bytes)",
			channel_name,
			session_name,
			threshold_placeholder,
			usage_placeholder);
	}

	void _print_session_rotation_ongoing_trigger_notification()
	{
		fmt::println("Session rotation ongoing");
	}

	void _print_session_rotation_completed_trigger_notification()
	{
		fmt::println("Session rotation completed");
	}
};

class trigger_notification_machine_formatter : public trigger_notification_formatter {
public:
	explicit trigger_notification_machine_formatter(struct lttng_notification *notification) :
		trigger_notification_formatter(notification)
	{
	}

private:
	void _print_event_rule_matches_trigger_notification_output_specific() override
	{
		json::json json_notification;
		json_notification["trigger-name"] = _origin_trigger_name;
		std::cout << json_notification.dump(4) << std::endl;

		return;

		/* TODO: The following section is WIP */
		/* Print values captured alongside trigger */
		if (_trigger_captured_values) {
			try {
				// TODO: Add machine interface
				event_field_value_human_formatter human_formatter;
				human_formatter.print_event_field_values(_trigger_condition,
									 _trigger_captured_values);
			} catch (const lttng::cli::unexpected_type& type_exception) {
				ERR_FMT("Failed to print values captured alongside trigger.");
			} catch (const std::runtime_error& runtime_exception) {
				ERR_FMT("Failed to print values captured alongside trigger.");
			}
		}
	}
};

class trigger_notification_human_formatter : public trigger_notification_formatter {
public:
	explicit trigger_notification_human_formatter(struct lttng_notification *notification) :
		trigger_notification_formatter(notification)
	{
	}

private:
	void _print_event_rule_matches_trigger_notification_output_specific() override
	{
		/* Print trigger that sent the notification. */
		fmt::println("Event (trigger {})", _origin_trigger_name);

		/* Print values captured alongside trigger */
		if (_trigger_captured_values) {
			try {
				event_field_value_human_formatter human_formatter;
				human_formatter.print_event_field_values(_trigger_condition,
									 _trigger_captured_values);
			} catch (const lttng::cli::unexpected_type& type_exception) {
				ERR_FMT("Failed to print values captured alongside trigger.");
			} catch (const std::runtime_error& runtime_exception) {
				ERR_FMT("Failed to print values captured alongside trigger.");
			}
		}
	}
};

/* Notification reception structure */
struct notification_result {
	enum lttng_notification_channel_status notification_channel_status;
	lttng::ctl::notification notification;
};

/* Trigger subscription */
bool trigger_action_has_notify(const struct lttng_action *action)
{
	enum lttng_action_type action_type = lttng_action_get_type(action);
	bool has_notify = false;

	if (action_type == LTTNG_ACTION_TYPE_NOTIFY) {
		has_notify = true;
	} else if (action_type == LTTNG_ACTION_TYPE_LIST) {
		/* More than one action is associated with the trigger */
		unsigned int count;

		const enum lttng_action_status status = lttng_action_list_get_count(action, &count);
		if (status != LTTNG_ACTION_STATUS_OK) {
			ERR_FMT("Failed to get action count from action group.");
			LTTNG_THROW_ERROR(
				lttng::format("Failed to get action count from action group."));
		}

		for (unsigned int i = 0; i < count; i++) {
			const struct lttng_action *action_item =
				lttng_action_list_get_at_index(action, i);
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
		LTTNG_THROW_ERROR(lttng::format("Failed to get trigger name."));
	}

	const struct lttng_action *action;
	action = lttng_trigger_get_const_action(trigger);
	LTTNG_ASSERT(action != nullptr);

	if (check_for_notify_action) {
		bool trigger_has_notify = false;
		trigger_has_notify = trigger_action_has_notify(action);

		if (!trigger_has_notify) {
			WARN_FMT(
				"Subscribing to trigger `{}`, but it does not contain a notify action.",
				trigger_name);
		}
	}

	const struct lttng_condition *trigger_condition =
		lttng_trigger_get_const_condition(trigger);
	LTTNG_ASSERT(trigger_condition != nullptr);

	enum lttng_notification_channel_status notification_channel_status;
	notification_channel_status =
		lttng_notification_channel_subscribe(notification_channel, trigger_condition);
	if (notification_channel_status != LTTNG_NOTIFICATION_CHANNEL_STATUS_OK) {
		ERR_FMT("Failed to subscribe to notifications of trigger `{}`.", trigger_name);
		LTTNG_THROW_CLI_TRIGGER_NOTIFICATION_SUBSCRIPTION_ERROR(trigger_name);
	}
}
} /* namespace */

int cmd_listen(int argc, const char **argv)
{
	/*
	 * If machine interface (MI) output is requested, the output
	 * format must be JSON Lines.
	 */
	bool output_is_mi = lttng_opt_mi > 0;
	if (output_is_mi && (lttng_opt_mi != LTTNG_MI_JSON_LINES)) {
		ERR_FMT("listen: Requested machine interface (MI) output format is unsupported for this command.");
		return CMD_ERROR;
	}

	/* Skip "listen" in the list of arguments */
	const int my_argc = argc - 1;
	const char **my_argv = argv + 1;

	/* Process the command arguments */
	bool signal_when_ready = false;
	std::vector<std::string> requested_trigger_names;
	argpar::Iter<nonstd::optional<argpar::Item>> argpar_iter(my_argc, my_argv, listen_options);

	while (true) {
		const nonstd::optional<argpar::Item> argpar_item = argpar_iter.next();

		if (!argpar_item) {
			/* No more arguments to parse. */
			break;
		}

		if (argpar_item->isOpt()) {
			/* If the argument is a command option */
			const argpar::OptItemView option = argpar_item->asOpt();
			switch (option.descr().id) {
			case OPT_HELP:
				/*
				 * The `SHOW_HELP` macro assumes that an integer
				 * named `ret` exists.
				 */
				int ret;
				SHOW_HELP();
				return CMD_SUCCESS;

			case OPT_LIST_OPTIONS:
				list_cmd_options_argpar(stdout, listen_options);
				return CMD_SUCCESS;

			case OPT_SIGNAL_WHEN_READY:
				signal_when_ready = true;
			}
		} else {
			/* If the argument is a trigger */
			requested_trigger_names.emplace_back(argpar_item->asNonOpt().arg());
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
	 * Fetch all triggers from sessiond.
	 *
	 * Assign all_triggers using a lambda function to restrict the scope
	 * of the raw list of triggers and force use of the unique pointer
	 * list.
	 *
	 * Use an alias for the trigger list type because otherwise the
	 * declaration would be enormously verbose.
	 */
	const lttng::ctl::triggers all_triggers = []() {
		lttng_triggers *raw_triggers = nullptr;

		const enum lttng_error_code error_code = lttng_list_triggers(&raw_triggers);
		if (error_code != LTTNG_OK) {
			LTTNG_THROW_CTL("Failed to list triggers.", error_code);
		}

		return lttng::ctl::triggers(raw_triggers);
	}();

	unsigned int all_triggers_count;
	enum lttng_trigger_status trigger_status;
	trigger_status = lttng_triggers_get_count(all_triggers.get(), &all_triggers_count);
	if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
		ERR_FMT("Failed to get trigger count.");
		return CMD_ERROR;
	}

	const lttng::ctl::notification_channel notification_channel = []() {
		struct lttng_notification_channel *raw_notification_channel =
			lttng_notification_channel_create(
				lttng_session_daemon_notification_endpoint);

		if (!raw_notification_channel) {
			// TODO: Put a hardcoded error code that is more specific?
			const enum lttng_error_code error_code = LTTNG_ERR_UNK;
			LTTNG_THROW_CTL("Failed to create notification channel.", error_code);
		}

		return lttng::ctl::notification_channel(raw_notification_channel);
	}();

	/* Subscribe to notifications from the triggers we want */
	bool check_for_notify_action;
	if (listen_all_triggers) {
		/* Listen to all triggers. */
		const struct lttng_trigger *trigger;

		for (unsigned int trigger_i = 0; trigger_i < all_triggers_count; trigger_i++) {
			trigger = lttng_triggers_get_at_index(all_triggers.get(), trigger_i);

			check_for_notify_action = false;
			try {
				subscribe(notification_channel.get(),
					  trigger,
					  check_for_notify_action);
			} catch (const lttng::cli::trigger_notification_subscription_error&
					 subscription_exception) {
				ERR_FMT(subscription_exception.what());
				return CMD_ERROR;
			}
		}

		// TODO: Add a --quiet option instead of reusing the MI option which doesn't
		// necessarily mean suppress the output?
		if (!output_is_mi) {
			fmt::println("Listening for notifications from all existing triggers.");
			std::fflush(stdout);
		}
	} else {
		/*
		 * Listen to specific triggers.
		 *
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

			for (trigger_i = 0; trigger_i < all_triggers_count; trigger_i++) {
				trigger =
					lttng_triggers_get_at_index(all_triggers.get(), trigger_i);
				const char *trigger_name_char_array;

				trigger_status =
					lttng_trigger_get_name(trigger, &trigger_name_char_array);
				if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
					ERR_FMT("Failed to get trigger name.");
					return CMD_ERROR;
				}

				const std::string trigger_name = trigger_name_char_array;
				if (trigger_name.compare(requested_trigger_name) == 0) {
					break;
				}
			}

			if (trigger_i == all_triggers_count) {
				ERR_FMT("Couldn't find a trigger with name `{}`.",
					requested_trigger_name);
				return CMD_ERROR;
			}

			check_for_notify_action = true;
			try {
				subscribe(notification_channel.get(),
					  trigger,
					  check_for_notify_action);
			} catch (const lttng::cli::trigger_notification_subscription_error&
					 subscription_exception) {
				ERR_FMT(subscription_exception.what());
				return CMD_ERROR;
			}
		}

		if (!output_is_mi) {
			fmt::println("Listening for the specified triggers.");
			std::fflush(stdout);
		}
	}

	/* Signal that listen is ready (i.e. subscriptions are complete). */
	if (signal_when_ready) {
		::kill(getppid(), SIGUSR2);
	}

	/* Loop forever, listening for trigger notifications. */
	for (;;) {
		const struct notification_result next_notification_result =
			[](struct lttng_notification_channel *notif_channel) {
				enum lttng_notification_channel_status notification_channel_status;
				struct lttng_notification *raw_notification = nullptr;

				notification_channel_status =
					lttng_notification_channel_get_next_notification(
						notif_channel, &raw_notification);

				notification_result notification_result;
				notification_result.notification_channel_status =
					notification_channel_status;
				notification_result.notification =
					lttng::ctl::notification(raw_notification);

				return notification_result;
			}(notification_channel.get());

		switch (next_notification_result.notification_channel_status) {
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_NOTIFICATIONS_DROPPED:
			fmt::println("Dropped notification.");
			std::fflush(stdout);
			break;
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_INTERRUPTED:
			return CMD_SUCCESS;
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_OK:
			LTTNG_ASSERT(next_notification_result.notification.get() != nullptr);

			if (lttng_opt_mi == LTTNG_MI_JSON_LINES) {
				/* Machine interface (MI) output */
				trigger_notification_machine_formatter machine_formatter(
					next_notification_result.notification.get());
				machine_formatter.print_trigger_notification();

			} else {
				/* Human-readable output */
				try {
					trigger_notification_human_formatter human_formatter(
						next_notification_result.notification.get());
					human_formatter.print_trigger_notification();
				} catch (const lttng::cli::unexpected_type& type_exception) {
					return CMD_ERROR;
				} catch (const std::runtime_error& runtime_exception) {
					return CMD_ERROR;
				}
			}

			break;
		case LTTNG_NOTIFICATION_CHANNEL_STATUS_CLOSED:
			fmt::println("Notification channel was closed by peer.");
			std::fflush(stdout);
			return CMD_SUCCESS;
		default:
			ERR_FMT("A communication error occurred on the notification channel.");
			return CMD_ERROR;
		}
	}
}
