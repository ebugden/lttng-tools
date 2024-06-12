/*
 * Copyright (C) 2021 EfficiOS Inc.
 * Does Simon want his name as well?
 * Copyright (C) 2024 Erica Bugden <ebugden@efficios.com>
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 */

#include "../command.hpp"
#include "../exception.hpp"
// Will lttng listen only work for UST? e.g. not for kernel tracepoints?
// TODO: Run the check Simon was talking about to see if there are any unused headers included
#include "common/argpar/argpar.hpp"
#include "common/argpar-utils/argpar-utils.hpp"
// Don't necessarily need to include what I use because it will be included anyways?
#include "common/error.hpp"
#include "common/format.hpp"
#include "common/macros.hpp"
#include "common/make-unique.hpp"
#include "vendor/optional.hpp"

// Not sure whether to include these here...
#include <signal.h>
#include <string>
#include <sys/time.h>
#include <unistd.h>
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

// TODO: Put things that can be on one line (max 99 characters) on one line
// TODO: Check if anything should be a function rather than inline
// Better term for `event_expr`? Captured field descriptions?
/* Event field printing */
// TODO: Chuck in a separate file? Nah. It's only useful here.
// TODO: Call it a printer instead of a formatter? I feel like print is more explicit than format here

// We only have access to the descriptions stated by the user to describe the capture
// The capture condition. We don't have access to the field names from the tracepoint description
// Only the top level of data will have labels (because only those captures are described)

// TODO: Hand the name down to the other functions through a class attribute if handing through return values is a pain
// TSDL trace visitor .cpp has an example of using class attributes to handle exceptions and hand things down
class event_field_value_formatter {
public:
	// TODO: Hand the condition to the constructor instead?
	// I forget why there needs to be a destructor. I think it's because otherwise there will be a leak? But I don't remember in what context
	virtual ~event_field_value_formatter() = default;

	// Add more information about what is happening? Are there more specific terms that
	// could be used?
	// TODO: Indent things?
	/*
	 * Format and print the captured event fields.
	 *
	 * The first level of fields is described by the user-defined
	 * capture descriptors. Those labels are printed alongside the first
	 * level of fields before recursively printing the remaining fields.
	 */
	void print_event_field_values(const struct lttng_condition *condition,
			const struct lttng_event_field_value *field_value) {
		/* The top level should only ever be an array */
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

		// Why is it an arrow?
		// I need to make sure I'm calling the child class method and not the parent one
		// Should I not print the array start and end here? Is it more implementation information than something useful for the user? At least for human readable
		// When do I need to put `this`? how does it know which function i'm talking about? When I'm worried about it using the correct attributes? i.e. for functions it doesn't matter?
		// I like that it's explicit
		this->format_array_start();

		for (unsigned int i = 0; i < length; i++) {
			// How many tabs should wrapped lines be indended by? Sometimes it's one something it's two in the existing code. check with clang format later
			const struct lttng_event_expr *expr =
				lttng_condition_event_rule_matches_get_capture_descriptor_at_index(condition, i);
			LTTNG_ASSERT(expr);

			// Also what happens if there are several arrays in arrays?
			print_one_event_expr(expr);

			const struct lttng_event_field_value *elem;
			status = lttng_event_field_value_array_get_element_at_index(field_value, i, &elem);
			// Below in `print_one_event_expr` the status is checked with an assertion. Should I do that here? If the i is correct, it should never fail.
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
	// How to align wrapped arrays?
	// Call this function `format_generic` instead? format_anything is more cringe, but more clear imo
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
		// It's possible that holding on to the field name for reuse may be useful in the future if the machine interfaces and human interface differences don't easily allow for using the same display means

		switch (type) {
		/*
		 * The enumeration types (UNSIGNED_ENUM, SIGNED_ENUM) are
		 * treated the same as integers when printing since the current
		 * implementation does not provide a way to know the enumeration
		 * labels.
		 * 
		 * More details available in this code's commit message.
		 */
		case LTTNG_EVENT_FIELD_VALUE_TYPE_UNSIGNED_INT:
		case LTTNG_EVENT_FIELD_VALUE_TYPE_UNSIGNED_ENUM:
		{
			//fmt::print("unsigned_int or enum\n");
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
			//fmt::print("signed_int or enum\n");
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
			//fmt::print("double");
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
			// Why is it an arrow?
			// I need to make sure I'm calling the child class method and not the parent one
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
			// Why `__func__` not highlighting correctly?
			throw lttng::cli::unexpected_type("event field value", int(type), __FILE__, __func__, __LINE__);
		}
	}

	/*
	 * Unsigned and signed enumeration values are printed with the
	 * functions `format_unsigned_int()` and `format_signed_int()`
	 * respectively.
	 */
	// TODO: Call the following functions "print" instead of "format"? More clear I think
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
			// Declare while assigning? Instead of declaring above here
			unsigned int index;
			const struct lttng_event_expr *parent_expr;
			enum lttng_event_expr_status status;

			// No idea what's happening here lol. Maybe a good idea to understand.
			parent_expr = lttng_event_expr_array_field_element_get_parent_expr(event_expr);
			LTTNG_ASSERT(parent_expr != nullptr);

			// Why does this recurse if there are only titles at the top level...
			print_one_event_expr(parent_expr);

			status = lttng_event_expr_array_field_element_get_index(event_expr, &index);
			// Why assertion here vs. Throwing an exception? More condensed and shouldn't happen frequently?
			// In other similar situations the status is checked and an error is expressed
			LTTNG_ASSERT(status == LTTNG_EVENT_EXPR_STATUS_OK);

			fmt::print("[{}]: ", index);

			break;
		}

		default:
			ERR_FMT("Unexpected event expression type ({})", int(type));
			// Why `__func__` not highlighting correctly?
			throw lttng::cli::unexpected_type("event expression", int(type), __FILE__, __func__, __LINE__);
		}
	}
};

class event_field_value_human_formatter : public event_field_value_formatter {
protected:
	void format_unsigned_int(std::uint64_t value) {
		//fmt::print("format dat unsigned_int!\n");
		// TODO: Swap all the prints that need a `\n` to println
		fmt::print("{}\n", value);
	}

	void format_signed_int(std::int64_t value) {
		//fmt::print("format dat signed_int!\n");
		fmt::print("{}\n", value);
	}

	void format_real(double value) {
		//fmt::print("format dat real!\n");
		fmt::print("{}\n", value);
	}

	void format_string(const char* str) {
		//fmt::print("format dat string!\n");
		fmt::print("{}\n", str);
	}

	void format_array_start(void) {
		fmt::print("[\n");
	}

	void format_array_element(const struct lttng_event_field_value *elem) {
		//fmt::print("format dat array_element!\n");
		// Does this call the right thing?
		event_field_value_formatter::format_anything(elem);
	}

	void format_array_end(void) {
		fmt::print("]\n");
	}
};

// JSON formatter?
class event_field_value_machine_formatter : public event_field_value_formatter {
	// TODO!
};

/* Notification printing */
class trigger_notification_formatter{
public:
	// I forget why I need an explicit destructor. Is it because the derived class would be destroyed but not this one?
	// Why does it need to be virtual?
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
			// No idea if I should retrieve the captures here or later in the print function conceptually speaking.
			// Rename field_val to captures? That's what it's called afterwards.
			// Or is it not captures yet because it may be empty?
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
	// format_on_event_notification instead?
	virtual void print_event_notification(struct lttng_notification *notification,
			const struct lttng_event_field_value *captures) = 0;
};

class trigger_notification_human_formatter : public trigger_notification_formatter {
protected:
	// I don't know how to trigger these functions... types of notifications?
	// Do you need to subscribe to session rotation notifications?
	// TODO: Clarify listen's full feature set?
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
			/*
			 * TODO: Throw a special exception for this. Why? I dunno.
			 * Vibes that it's especially unusual/important. Why more
			 * important than other "get" functions?
			 */
			throw std::runtime_error(lttng::format(
					"Failed to get name origin trigger from notification."));
		}

		fmt::print("Event (trigger {})\n", origin_trigger_name);

		// Explain what's going on here? What the condition is and what's happening
		// Because I have no idea what's happening
		// Better names? e.g. instead of "condition"
		// Is a trigger condition or a notification condition? Is it why the notification was sent?
		const struct lttng_condition *condition = lttng_notification_get_condition(notification);
		LTTNG_ASSERT(condition != nullptr);

		/*
		 * TODO: Get rid of the the check for whether there are captured
		 * values before trying to retrieve them? (the next ~15 lines
		 * until `if (captures)`) Can't we just directly try to retrieve
		 * them and then if there are none there are none? I don't see
		 * why it would be useful unless it's possible to expect
		 * captures, but then not receive any.
		 */
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
		// TODO: Is it worth having more specific errors if I just always handle them the same way? I feel like it isn't worth it
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
// TODO: Write a test for this
bool trigger_action_has_notify(const struct lttng_action *action) {
	enum lttng_action_type action_type;
	action_type = lttng_action_get_type(action);

	// This doesn't feel super robust as it could lead to false negatives.
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
		// This can throw an exception, but I think we can just let it propagate? At least one more level up
		trigger_has_notify = trigger_action_has_notify(action);

		if (!trigger_has_notify) {
			// TODO: I'm not sure this should be the default behaviour
			// I don't think we should subscribe unless it's possible to assign a notify to a trigger after it is created
			// If it's not possible, we should just not subscribe
			WARN_FMT("Subscribing to trigger `{}`, but it does not contain a notify action.",
					trigger_name);
		}
	}

	const struct lttng_condition *condition;
	// I still don't know what a trigger condition is
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
	//sleep(45); // To facilitate attaching with GDB
	//static volatile int patate=0;
	//while (!patate);
	/* Skip "listen" in the list of arguments */
	int my_argc = argc - 1;
	const char **my_argv = argv + 1;

	/* Process the command arguments */
	int ret;
	std::vector<std::string> requested_trigger_names;
	// I don't think I do any error management yet in the argument parsing section
	// TODO: Catch any of the errors in the creation or interating over the arguments? Or let them propagate to the top? Check which commands can throw errors
	// TODO: Keep any of the assertions in the original code?
	// TODO: Swap variable type to auto when I understand the type more
	argpar::Iter<nonstd::optional<argpar::Item>> argpar_iter(my_argc, my_argv, listen_options);

	// TODO: Move the argument processing bits to a function?
	// TODO: Any error management necessary?
	// TODO: Clarify that the items are optional? Maybe some documentation should be made for this?
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
					// Isn't it better to have a single return point?
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

	/* Fetch all triggers from sessiond. */
	//struct lttng_triggers *all_triggers = NULL;
	// TODO: Need to initialize to NULL?
	std::unique_ptr<struct lttng_triggers*> all_triggers = lttng::make_unique<struct lttng_triggers*>();
	enum lttng_error_code error_code;
	// Need to transfer ownership of unique_ptr?
	error_code = lttng_list_triggers(all_triggers.get());
	if (error_code != LTTNG_OK) {
		// TODO: Print error code for more details?
		ERR_FMT("Failed to list triggers.");
		goto error;
	}

	unsigned int all_triggers_count;
	enum lttng_trigger_status trigger_status;
	// Can the trigger count be evaluated in the specific case below?
	trigger_status = lttng_triggers_get_count(
			*all_triggers, &all_triggers_count);
	if (trigger_status != LTTNG_TRIGGER_STATUS_OK) {
		ERR_FMT("Failed to get trigger count.");
		goto error;
	}

	// TODO: I want to create the unique ptr before setting it so the following line isn't 1000 years long. Can I do this?
	std::unique_ptr<struct lttng_notification_channel*> notification_channel = lttng::make_unique<struct lttng_notification_channel*>(lttng_notification_channel_create(
			lttng_session_daemon_notification_endpoint));
	if (!*notification_channel) {
		ERR_FMT("Failed to create notification channel.");
		goto error;
	}

	/* Subscribe to notifications from the triggers we want */
	/*
	 * TODO: At the moment we exit as soon as one subscription fails.
	 * Consider entering the loop if at least one trigger is
	 * successfully subscribed to.
	 */
	bool check_for_notify_action;
	if (listen_all_triggers) {
		/* Listen to all triggers. */
		const struct lttng_trigger *trigger;

		for (unsigned int trigger_i = 0; trigger_i < all_triggers_count;
				trigger_i++) {
			trigger = lttng_triggers_get_at_index(*all_triggers, trigger_i);

			// Why are we subscribing to triggers that may not contain notify? (explicit_notif_check is false in the subscribe() function call)
			// My understanding is that we will never receive news from those triggers if they don't have the notification action
			// Is it because we're subscribing to all the triggers and don't want to spam? Yeah we just listen to the ones that will send anyways we don't need to warn
			check_for_notify_action = false;
			try {
				subscribe(*notification_channel, trigger, check_for_notify_action);
			} catch (const lttng::cli::trigger_notification_subscription_error& subscription_exception) {
				// TODO: Call it a subscription error instead of subscription exception?
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

			for (trigger_i = 0; trigger_i < all_triggers_count;
					trigger_i++) {
				trigger = lttng_triggers_get_at_index(
						*all_triggers, trigger_i);
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
				// Fail as soon as one trigger is not valid/ doesn't exist? Why not continue?
				// This is the simplest for now. Make this work first and then do someting more complicated later.
				goto error;
			}

			check_for_notify_action = true;
			try {
				subscribe(*notification_channel, trigger, check_for_notify_action);
			} catch (const lttng::cli::trigger_notification_subscription_error& subscription_exception) {
				ERR_FMT(subscription_exception.what());
				goto error;
			}
		}

		// TODO: Specify the triggers that we're listening to?
		WARN_FMT("Listening on the specified triggers.");
	}

	/* Signal that listen is ready (i.e. subscriptions are complete). */
	::kill(getppid(), SIGUSR1);

	/* Loop forever, listening for trigger notifications. */
	for (;;) {
		enum lttng_notification_channel_status
				notification_channel_status;

		std::unique_ptr<struct lttng_notification*> notification = lttng::make_unique<struct lttng_notification*>();
		notification_channel_status =
				lttng_notification_channel_get_next_notification(
						*notification_channel,
						notification.get());

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
			LTTNG_ASSERT(*notification != nullptr);

			/*
			 * Exception handling: Do we want to die as soon as an error
			 * is thrown? Or keep going unless otherwise specified? Keep
			 * going would be my reflex. But then we would never end
			 * unless user explicitly kills us.
			 */
			// TODO: Are there static checks to know if I'm handling all the exceptions something can throw?
			try {
				// TODO: Add machine interface
				human_formatter.print_notification(*notification);
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
	return ret;
}
