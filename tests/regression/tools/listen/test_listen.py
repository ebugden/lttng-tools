#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Erica Bugden <ebugden@efficios.com>
# SPDX-License-Identifier: GPL-2.0-only
#
"""
Validate the lttng-listen command.

This command listens for a user-specified set of LTTng trigger
notifications. If a notification is received, the listener displays it.
"""

import difflib
import json
import pathlib
import selectors
import sys
import textwrap
import time

# Import in-tree test utils
test_utils_import_path = pathlib.Path(__file__).absolute().parents[3] / "utils"
sys.path.insert(0, str(test_utils_import_path))

import lttngtest
# Should this be included in lttngtest? Because the code there uses it?
import bt2  # Debug

# Timeout is set to 5 minutes to avoid timing out simply because the machine
# is very slow.
#
# When debugging, set the timeout to something short, e.g. 4 seconds.
# TODO: Put this variable somewhere more general?
TIMEOUT_DURATION_SECONDS = 4 #5 * 60

NUMBER_PLACEHOLDER_STR = "XXX"


def wait_for_n_lines(file_object, n_lines, timeout):
    lines = []
    file_listener = selectors.DefaultSelector()
    file_listener.register(file_object, selectors.EVENT_READ)
    deadline = time.monotonic() + timeout

    while len(lines) < n_lines:
        remaining = deadline - time.monotonic()

        # Wait for output or timeout
        # TODO: Raise a more specific exception
        if not file_listener.select(timeout=remaining):
            raise Exception(
                f"Timed out while waiting for trigger notification "
                f"listener output. Current output:\n{lines}"
            )

        # I don't know why this approach doesn't cause problems too. In my mind there could be more than one line ready at once.
        lines.append(file_object.readline().decode("utf-8").rstrip())
        # TODO: Pass tap as parameter too so that I can print as diagnostic?
        print(f"Output line {len(lines)}: {lines[-1]}")

    return lines


def test_listen_all_triggers(tap, test_env):
    """
    Validate that all triggers are listened for when no arguments are
    used. Validate that trigger notifications are successfully received.
    """

    test_description = "Successfully receive two trigger notifications"
    tap.diagnostic("Start test: " + test_description)

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)

    # Add triggers
    tap.diagnostic("Add triggers")
    trigger1_name = "trigger1"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --action notify"
    )
    trigger2_name = "trigger2"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:end --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Artificially produce the desired event-rule condition
    #
    # The passed arguments should result in two events:
    # • One emitted during a standard generation iteration
    # • One emitted just at the end
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, emit_end_event=True, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    trace_event_generator_app.trace()

    # Waiting for the app to finish emitting events and to exit
    # is not necessary for synchronization since later we wait
    # for a specific number of lines.

    # Define expected output
    expected_output = textwrap.dedent("""
        Listening for notifications from all existing triggers.
        Event (trigger trigger1)
        Event (trigger trigger2)""").strip()

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Wait for processes to exit
    trace_event_generator_app.wait_for_exit()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # Check expected output
    #
    # If the outputs are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_two_triggers_with_event_field_captures(tap, test_env):
    """
    Validate that all triggers are listened for when no arguments are
    used. Validate that trigger notifications and captured event field
    values are successfully received.
    """

    test_description = "Successfully receive two trigger notifications with captured event field values"
    tap.diagnostic("Start test: " + test_description)

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)

    # Add triggers
    tap.diagnostic("Add triggers")

    trigger1_name = "trigger1"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --capture 'intfield' --capture 'doublefield' "
        "--action notify"
    )

    trigger2_name = "trigger2"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:end --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False, triggers=[trigger1_name, trigger2_name]
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Artificially produce the desired event-rule condition
    #
    # The passed arguments should result in two events:
    # • One emitted during a standard generation iteration
    # • One emitted just at the end
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, emit_end_event=True, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    trace_event_generator_app.trace()

    # Waiting for the app to finish emitting events and to exit
    # is not necessary for synchronization since later we wait
    # for a specific number of lines.

    # Define expected output
    expected_output = textwrap.dedent("""
        Listening for notifications from the specified triggers.
        Event (trigger trigger1)
        [
        intfield: 0
        doublefield: 2
        ]
        Event (trigger trigger2)""").strip()

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Wait for processes to exit
    trace_event_generator_app.wait_for_exit()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # Check expected output
    #
    # If the outputs are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_trigger_consumed_data_size_becomes_greater_than_machine_output(
    tap, test_env
):
    """
    Validate that notifications from "Recording session consumed data
    size becomes greater than" triggers are received and displayed
    correctly.
    """

    test_description = "Successfully display 'consumed data size' trigger notification (machine output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add trigger
    tap.diagnostic("Add trigger")
    trigger_name = "recording-session-consumed-data-size-becomes-greater-than-trigger"
    threshold_bytes = 1
    client._run_cmd(
        f"add-trigger --name '{trigger_name}' --condition=session-consumed-size-ge "
        f"--session={tracing_session.name} --threshold-size={threshold_bytes} "
        "--action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    registered_triggers = [trigger_name]
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=True, triggers=registered_triggers
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    expected_output = {
        "trigger": {
            "name": trigger_name,
            "owner-uid": NUMBER_PLACEHOLDER_STR,
        },
        "condition": {
            "type": "session-consumed-size",
            "session-name": tracing_session.name,
            "threshold-bytes": threshold_bytes,
        },
        "evaluation": {"consumed-bytes": NUMBER_PLACEHOLDER_STR},
    }

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(registered_triggers),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to stop tracing or explicitly remove the triggers since
    # the session daemon is torn down after each test.

    # TODO: Validate that the output is valid JSON Lines. Attempt to
    # parse each line as a JSON object?

    try:
        trigger_message_json = json.loads(actual_output[0])
    except json.decoder.JSONDecodeError as e:
        tap.diagnostic("Error while decoding the received output")
        tap.diagnostic(type(e).__name__ + ": " + str(e))
        tap.test(False, test_description)
        return

    # Check that the number of bytes consumed is greater than the set
    # threshold.
    try:
        bytes_consumed = trigger_message_json["evaluation"]["consumed-bytes"]
    except KeyError as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return
    tap.test(
        bytes_consumed > threshold_bytes,
        "Number of bytes consumed is greater than the set threshold",
    )

    # Since we cannot predict the exact number of bytes consumed, remove
    # the number of bytes from the actual output when comparing the
    # message.
    trigger_message_json["evaluation"]["consumed-bytes"] = NUMBER_PLACEHOLDER_STR

    # Check expected message
    #
    # If the JSON objects are equivalent, then no diff output will be
    # generated. If they are different, the diff will show.
    # TODO: Find a better strategy for showing differences. The order of
    # JSON items is not deterministic. Using a straight diff adds noise.
    if trigger_message_json != expected_output:
        for line in difflib.context_diff(
            json.dumps(expected_output, indent=4).split("\n"),
            json.dumps(trigger_message_json, indent=4).split("\n"),
            "expected output",
            "actual output",
        ):
            tap.diagnostic(line)

    tap.test(trigger_message_json == expected_output, test_description)


def test_listen_trigger_consumed_data_size_becomes_greater_than_human_output(
    tap, test_env
):
    """
    Validate that notifications from "Recording session consumed data
    size becomes greater than" triggers are received and displayed
    correctly.
    """

    test_description = (
        "Successfully display 'consumed data size' trigger notification (human output)"
    )
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add trigger
    tap.diagnostic("Add trigger")
    trigger_name = "recording-session-consumed-data-size-becomes-greater-than-trigger"
    threshold_bytes = 1
    client._run_cmd(
        f"add-trigger --name '{trigger_name}' --condition=session-consumed-size-ge "
        f"--session={tracing_session.name} --threshold-size={threshold_bytes} "
        "--action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False, triggers=[trigger_name]
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    expected_output = (
        "Listening for notifications from the specified triggers.\n"
        f"Trigger notification: Session `{tracing_session.name}` has consumed "
        f"{NUMBER_PLACEHOLDER_STR} bytes of data. Notification threshold: "
        f"{threshold_bytes} bytes."
    )

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to stop tracing or explicitly remove the triggers since
    # the session daemon is torn down after each test.

    trigger_message = actual_output[1].split()

    # Check that the number of bytes consumed is greater than the set
    # threshold.
    try:
        bytes_consumed = int(trigger_message[trigger_message.index("bytes") - 1])
    except (ValueError, IndexError) as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return
    tap.test(
        bytes_consumed > threshold_bytes,
        "Number of bytes consumed is greater than the set threshold",
    )

    # Since we cannot predict the exact number of bytes consumed, remove
    # the number of bytes from the actual output when comparing the
    # message.
    trigger_message[(trigger_message.index("bytes") - 1)] = NUMBER_PLACEHOLDER_STR
    actual_output[1] = " ".join(trigger_message)

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_trigger_buffer_usage_becomes_greater_than_machine_output(tap, test_env):
    """
    Validate that notifications from "Channel buffer usage becomes
    greater than" triggers are received and displayed correctly.
    """

    test_description = "Successfully display 'buffer usage greater than' trigger notification (machine output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add trigger
    tap.diagnostic("Add trigger")
    trigger_name = "channel-buffer-usage-becomes-greater-than-trigger"
    tracing_domain = "user"
    threshold_bytes = 1
    client._run_cmd(
        f"add-trigger --name '{trigger_name}' --condition=channel-buffer-usage-ge "
        f"--session={tracing_session.name} --channel={channel.name} "
        f"--domain={tracing_domain} --threshold-size={threshold_bytes} --action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    registered_triggers = [trigger_name]
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=True, triggers=registered_triggers
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    expected_output = {
        "trigger": {
            "name": trigger_name,
            "owner-uid": NUMBER_PLACEHOLDER_STR,
        },
        "condition": {
            "type": "buffer-usage-high",
            "session-name": tracing_session.name,
            "channel-name": channel.name,
            "tracing-domain": (tracing_domain + " space"),
            "threshold-bytes": threshold_bytes,
        },
        "evaluation": {
            "usage-bytes": NUMBER_PLACEHOLDER_STR,
            "usage-ratio": NUMBER_PLACEHOLDER_STR,
        },
    }

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(registered_triggers),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to stop tracing or explicitly remove the triggers since
    # the session daemon is torn down after each test.

    # TODO: Validate that the output is valid JSON Lines. Attempt to
    # parse each line as a JSON object?

    try:
        trigger_message_json = json.loads(actual_output[0])
    except json.decoder.JSONDecodeError as e:
        tap.diagnostic("Error while decoding the received output")
        tap.diagnostic(type(e).__name__ + ": " + str(e))
        tap.test(False, test_description)
        return

    # Check that the number of bytes consumed is greater than the set
    # threshold.
    try:
        buffer_usage = trigger_message_json["evaluation"]["usage-bytes"]
    except (ValueError, IndexError) as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return
    tap.test(
        buffer_usage > threshold_bytes,
        "Channel buffer usage is greater than the set threshold",
    )

    # Since we cannot predict the exact number for the usage ratio and
    # buffer usage, remove these numbers from the actual output when
    # comparing the message.
    trigger_message_json["evaluation"]["usage-ratio"] = NUMBER_PLACEHOLDER_STR
    trigger_message_json["evaluation"]["usage-bytes"] = NUMBER_PLACEHOLDER_STR

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    # TODO: Find a better strategy for showing differences. The order of
    # JSON items is not deterministic. Using a straight diff adds noise.
    if trigger_message_json != expected_output:
        for line in difflib.context_diff(
            json.dumps(expected_output, indent=4).split("\n"),
            json.dumps(trigger_message_json, indent=4).split("\n"),
            "expected output",
            "actual output",
        ):
            tap.diagnostic(line)

    tap.test(trigger_message_json == expected_output, test_description)


def test_listen_trigger_buffer_usage_becomes_greater_than_human_output(tap, test_env):
    """
    Validate that notifications from "Channel buffer usage becomes
    greater than" triggers are received and displayed correctly.
    """

    test_description = "Successfully display 'buffer usage greater than' trigger notification (human output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add trigger
    tap.diagnostic("Add trigger")
    trigger1_name = "trigger1"
    tracing_domain = "user"
    threshold_bytes = 1
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition=channel-buffer-usage-ge "
        f"--session={tracing_session.name} --channel={channel.name} "
        f"--domain={tracing_domain} --threshold-size={threshold_bytes} --action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False, triggers=[trigger1_name]
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    expected_output = (
        "Listening for notifications from the specified triggers.\n"
        "Trigger notification: The amount of data in a buffer belonging to "
        f"channel `{channel.name}` of session `{tracing_session.name}` is "
        f"higher than the set maximum threshold of {threshold_bytes} bytes. "
        f"Usage ratio: {NUMBER_PLACEHOLDER_STR}, Buffer usage: "
        f"{NUMBER_PLACEHOLDER_STR} bytes, Tracing domain: "
        f"{(tracing_domain + " space")}."
    )

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to stop tracing or explicitly remove the triggers since
    # the session daemon is torn down after each test.

    trigger_message = actual_output[1].split()

    # Check that the number of bytes consumed is greater than the set
    # threshold.
    try:
        buffer_usage = int(trigger_message[trigger_message.index("usage:") + 1])
    except (ValueError, IndexError) as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return
    tap.test(
        buffer_usage > threshold_bytes,
        "Channel buffer usage is greater than the set threshold",
    )

    # Since we cannot predict the exact number for the usage ratio and
    # buffer usage, remove these numbers from the actual output when
    # comparing the message.
    trigger_message[(trigger_message.index("Buffer") - 1)] = (
        NUMBER_PLACEHOLDER_STR + ","
    )
    trigger_message[(trigger_message.index("usage:") + 1)] = NUMBER_PLACEHOLDER_STR
    actual_output[1] = " ".join(trigger_message)

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_trigger_buffer_usage_becomes_less_than_machine_output(tap, test_env):
    """
    Validate that notifications from "Channel buffer usage becomes
    less than" triggers are received and displayed correctly.
    """

    test_description = "Successfully display 'buffer usage less than' trigger notification (machine output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add trigger
    tap.diagnostic("Add trigger")
    trigger_name = "channel-buffer-usage-becomes-less-than-trigger"
    tracing_domain = "user"
    threshold_bytes = 5000
    client._run_cmd(
        f"add-trigger --name '{trigger_name}' --condition=channel-buffer-usage-le "
        f"--session={tracing_session.name} --channel={channel.name} "
        f"--domain={tracing_domain} --threshold-size={threshold_bytes} --action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    registered_triggers = [trigger_name]
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=True, triggers=registered_triggers
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=10, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    expected_output = {
        "trigger": {
            "name": trigger_name,
            "owner-uid": NUMBER_PLACEHOLDER_STR,
        },
        "condition": {
            "type": "buffer-usage-low",
            "session-name": tracing_session.name,
            "channel-name": channel.name,
            "tracing-domain": (tracing_domain + " space"),
            "threshold-bytes": threshold_bytes,
        },
        "evaluation": {
            "usage-bytes": NUMBER_PLACEHOLDER_STR,
            "usage-ratio": NUMBER_PLACEHOLDER_STR,
        },
    }

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(registered_triggers),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to stop tracing or explicitly remove the triggers since
    # the session daemon is torn down after each test.

    # TODO: Validate that the output is valid JSON Lines. Attempt to
    # parse each line as a JSON object?

    try:
        trigger_message_json = json.loads(actual_output[0])
    except json.decoder.JSONDecodeError as e:
        tap.diagnostic("Error while decoding the received output")
        tap.diagnostic(type(e).__name__ + ": " + str(e))
        tap.test(False, test_description)
        return

    # Check that the number of bytes consumed is less than the set
    # threshold.
    try:
        buffer_usage = trigger_message_json["evaluation"]["usage-bytes"]
    except (ValueError, IndexError) as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return
    tap.test(
        buffer_usage < threshold_bytes,
        "Channel buffer usage is less than the set threshold",
    )

    # Since we cannot predict the exact number for the usage ratio and
    # buffer usage, remove these numbers from the actual output when
    # comparing the message.
    trigger_message_json["evaluation"]["usage-ratio"] = NUMBER_PLACEHOLDER_STR
    trigger_message_json["evaluation"]["usage-bytes"] = NUMBER_PLACEHOLDER_STR

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    # TODO: Find a better strategy for showing differences. The order of
    # JSON items is not deterministic. Using a straight diff adds noise.
    if trigger_message_json != expected_output:
        for line in difflib.context_diff(
            json.dumps(expected_output, indent=4).split("\n"),
            json.dumps(trigger_message_json, indent=4).split("\n"),
            "expected output",
            "actual output",
        ):
            tap.diagnostic(line)

    tap.test(trigger_message_json == expected_output, test_description)


def test_listen_trigger_buffer_usage_becomes_less_than_human_output(tap, test_env):
    """
    Validate that notifications from "Channel buffer usage becomes
    less than" triggers are received and displayed correctly.
    """

    test_description = "Successfully display 'buffer usage less than' trigger notification (human output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add trigger
    tap.diagnostic("Add trigger")
    trigger_name = "channel-buffer-usage-becomes-less-than-trigger"
    tracing_domain = "user"
    threshold_bytes = 5000
    client._run_cmd(
        f"add-trigger --name '{trigger_name}' --condition=channel-buffer-usage-le "
        f"--session={tracing_session.name} --channel={channel.name} "
        f"--domain={tracing_domain} --threshold-size={threshold_bytes} --action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False, triggers=[trigger_name]
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=10, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    expected_output = (
        "Listening for notifications from the specified triggers.\n"
        "Trigger notification: The amount of data in a buffer belonging to "
        f"channel `{channel.name}` of session `{tracing_session.name}` is "
        f"lower than the set minimum threshold of {threshold_bytes} bytes. "
        f"Usage ratio: {NUMBER_PLACEHOLDER_STR}, Buffer usage: "
        f"{NUMBER_PLACEHOLDER_STR} bytes, Tracing domain: "
        f"{(tracing_domain + " space")}."
    )

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to stop tracing or explicitly remove the triggers since
    # the session daemon is torn down after each test.

    trigger_message = actual_output[1].split()

    # Check that the number of bytes consumed is less than the set
    # threshold.
    try:
        buffer_usage = int(trigger_message[trigger_message.index("usage:") + 1])
    except (ValueError, IndexError) as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return
    tap.test(
        buffer_usage < threshold_bytes,
        "Channel buffer usage is less than the set threshold",
    )

    # Since we cannot predict the exact number for the usage ratio and
    # buffer usage, remove these numbers from the actual output when
    # comparing the message.
    trigger_message[(trigger_message.index("Buffer") - 1)] = (
        NUMBER_PLACEHOLDER_STR + ","
    )
    trigger_message[(trigger_message.index("usage:") + 1)] = NUMBER_PLACEHOLDER_STR
    actual_output[1] = " ".join(trigger_message)

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_trigger_rotation_starts_and_finishes_machine_output(tap, test_env):
    """
    Validate that notifications from "Recording session rotation
    starts/finishes" triggers are received and displayed correctly.
    """

    test_description = "Successfully display 'rotation starts/finishes' trigger notifications (machine output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add triggers
    tap.diagnostic("Add triggers")
    trigger1_name = "rotation-starts"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition=session-rotation-starts "
        f"--session={tracing_session.name} --action=notify"
    )
    trigger2_name = "rotation-ends"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition=session-rotation-finishes "
        f"--session={tracing_session.name} --action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    registered_triggers = [trigger1_name, trigger2_name]
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=True, triggers=registered_triggers
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    trace_event_generator_app.trace()

    # Produce the trigger conditions
    tap.diagnostic("Rotate the tracing session")
    tracing_session.rotate()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    # TODO: Do not hardcode rotation id?
    # TODO: Include trace chunk location
    rotation_id = 0
    expected_output_line_1 = {
        "trigger": {
            "name": trigger1_name,
            "owner-uid": NUMBER_PLACEHOLDER_STR,
        },
        "condition": {
            "type": "session-rotation-ongoing",
            "session-name": tracing_session.name,
        },
        "evaluation": {"rotation-id": rotation_id},
    }
    expected_output_line_2 = {
        "trigger": {
            "name": trigger2_name,
            "owner-uid": NUMBER_PLACEHOLDER_STR,
        },
        "condition": {
            "type": "session-rotation-completed",
            "session-name": tracing_session.name,
        },
        "evaluation": {
            "rotation-id": rotation_id,
            "trace_chunk_archive_location": "TODO",
        },
    }

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(registered_triggers),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session daemon
    # is torn down after each test.

    # TODO: Validate that the output is valid JSON Lines. Attempt to
    # parse each line as a JSON object?

    try:
        trigger_message_json_line_1 = json.loads(actual_output[0])
        trigger_message_json_line_2 = json.loads(actual_output[1])
    except json.decoder.JSONDecodeError as e:
        tap.diagnostic("Error while decoding the received output")
        tap.diagnostic(type(e).__name__ + ": " + str(e))
        tap.test(False, test_description)
        return

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    # TODO: Find a better strategy for showing differences. The order of
    # JSON items is not deterministic. Using a straight diff adds noise.
    if trigger_message_json_line_1 != expected_output_line_1:
        for line in difflib.context_diff(
            json.dumps(expected_output_line_1, indent=4).split("\n"),
            json.dumps(trigger_message_json_line_1, indent=4).split("\n"),
            "expected output line 1",
            "actual output line 1",
        ):
            tap.diagnostic(line)
    if trigger_message_json_line_2 != expected_output_line_2:
        for line in difflib.context_diff(
            json.dumps(expected_output_line_2, indent=4).split("\n"),
            json.dumps(trigger_message_json_line_2, indent=4).split("\n"),
            "expected output line 2",
            "actual output line 2",
        ):
            tap.diagnostic(line)

    tap.test(
        (trigger_message_json_line_1 == expected_output_line_1)
        and (trigger_message_json_line_2 == expected_output_line_2),
        test_description,
    )


def test_listen_trigger_rotation_starts_and_finishes_human_output(tap, test_env):
    """
    Validate that notifications from "Recording session rotation
    starts/finishes" triggers are received and displayed correctly.
    """

    test_description = "Successfully display 'rotation starts/finishes' trigger notifications (human output)"
    tap.diagnostic("Start test: " + test_description)

    output_path = test_env.create_temporary_directory("trace")  # Debug

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = client.create_session(
        output=lttngtest.LocalSessionOutputLocation(output_path)  # Debug
    )
    channel = tracing_session.add_channel(lttngtest.lttngctl.TracingDomain.User)
    channel.add_recording_rule(lttngtest.lttngctl.UserTracepointEventRule("tp:tptest"))

    tracing_session.start()

    # Add triggers
    tap.diagnostic("Add triggers")
    trigger1_name = "rotation-starts"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition=session-rotation-starts "
        f"--session={tracing_session.name} --action=notify"
    )
    trigger2_name = "rotation-ends"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition=session-rotation-finishes "
        f"--session={tracing_session.name} --action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False, triggers=[trigger1_name, trigger2_name]
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    trace_event_generator_app.trace()

    # Produce the trigger conditions
    tap.diagnostic("Rotate the tracing session")
    tracing_session.rotate()

    trace_event_generator_app.wait_for_tracing_done()

    # Because there is not enough data to fill the subbuffer, stop
    # explicitly to force consuming the buffers. Otherwise, the trigger
    # will not fire.
    tracing_session.stop()

    # Define expected output
    # TODO: Do not hardcode rotation id?
    # TODO: Include trace chunk location
    rotation_id = 0
    expected_output = (
        "Listening for notifications from the specified triggers.\n"
        f"Trigger notification: Session rotation (id: {rotation_id}) "
        f"started for session `{tracing_session.name}`.\n"
        f"Trigger notification: Session rotation (id: {rotation_id}) "
        f"completed for session `{tracing_session.name}`. The "
        "resulting trace chunk is located here:\n"
        f"Trace chunk archive for session {tracing_session.name} is "
        "now readable at "
    )

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Cleanup
    trace_event_generator_app.wait_for_exit()
    tracing_session.destroy()  # Debug

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session daemon
    # is torn down after each test.

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


# TODO: There are multiple elements in this test that need to be updated
# (e.g. description)
def test_listen_trigger_event_field_matches_machine_output(tap, test_env):
    """
    Validate that all triggers are listened for when no arguments are
    used. Validate that trigger notifications and captured event field
    values are received and displayed correctly.
    """

    test_description = "Successfully receive two trigger notifications with captured event field values"
    tap.diagnostic("Start test: " + test_description)

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)

    # Add triggers
    tap.diagnostic("Add triggers")

    trigger1_name = "trigger1"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --capture 'intfield' --capture 'doublefield' "
        "--action notify"
    )

    trigger2_name = "trigger2"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:end --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    registered_triggers = [trigger1_name, trigger2_name]
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            triggers=registered_triggers
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Artificially produce the desired event-rule condition
    #
    # The passed arguments should result in two events:
    # • One emitted during a standard generation iteration
    # • One emitted just at the end
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, emit_end_event=True, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    trace_event_generator_app.trace()

    # Waiting for the app to finish emitting events and to exit
    # is not necessary for synchronization since later we wait
    # for a specific number of lines.

    # Define expected output
    expected_output_line_1 = {
        "trigger": {
            "name": "trigger1",
            "owner-uid": NUMBER_PLACEHOLDER_STR
        },
        "condition": {
            "type": "event-rule-matches",
            "event-rule": {
                "filter": "none",
                "type": "user tracepoint"
            },
            "capture-descriptors": [
                {
                    "type": "event-payload-field",
                    "name": "intfield"
                },
                {
                    "type": "event-payload-field",
                    "name": "doublefield"
                }
            ]
        },
        "evaluation": {
            "captured-values": [
                0,
                2.0
            ]
        },
    }
    expected_output_line_2 = {
        "trigger": {
            "name": "trigger2",
            "owner-uid": NUMBER_PLACEHOLDER_STR
        },
        "condition": {
            "type": "event-rule-matches",
            "event-rule": {
                "filter": "none",
                "type": "user tracepoint"
            }
        }
    }

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(registered_triggers),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Wait for processes to exit
    trace_event_generator_app.wait_for_exit()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # TODO: Validate that the output is valid JSON Lines. Attempt to
    # parse each line as a JSON object?

    try:
        trigger_message_json_line_1 = json.loads(actual_output[0])
        trigger_message_json_line_2 = json.loads(actual_output[1])
    except json.decoder.JSONDecodeError as e:
        tap.diagnostic("Error while decoding the received output")
        tap.diagnostic(type(e).__name__ + ": " + str(e))
        tap.test(False, test_description)
        return

    # Check expected message
    #
    # If the messages are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    # TODO: Find a better strategy for showing differences. The order of
    # JSON items is not deterministic. Using a straight diff adds noise.
    if trigger_message_json_line_1 != expected_output_line_1:
        for line in difflib.context_diff(
            json.dumps(expected_output_line_1, indent=4).split("\n"),
            json.dumps(trigger_message_json_line_1, indent=4).split("\n"),
            "expected output line 1",
            "actual output line 1",
        ):
            tap.diagnostic(line)
    if trigger_message_json_line_2 != expected_output_line_2:
        for line in difflib.context_diff(
            json.dumps(expected_output_line_2, indent=4).split("\n"),
            json.dumps(trigger_message_json_line_2, indent=4).split("\n"),
            "expected output line 2",
            "actual output line 2",
        ):
            tap.diagnostic(line)

    tap.test(
        (trigger_message_json_line_1 == expected_output_line_1)
        and (trigger_message_json_line_2 == expected_output_line_2),
        test_description,
    )


def test_listen_two_triggers_same_event_rule(tap, test_env):
    """
    Validate that it is possible to listen for notifications from two triggers
    that correspond to the same tracepoint.
    """

    test_description = "Successfully receive two notifications from triggers registered to same tracepoint"
    tap.diagnostic("Start test: " + test_description)

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)

    # Add triggers
    tap.diagnostic("Add triggers")
    trigger1_name = "trigger1"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --action notify"
    )
    trigger2_name = "trigger2"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Launch the event generating app
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    # Produce the trigger condition
    trace_event_generator_app.trace()

    # Waiting for the app to finish emitting events and to exit
    # is not necessary for synchronization since later we wait
    # for a specific number of lines.

    # Define expected output
    expected_output = textwrap.dedent("""
        Listening for notifications from all existing triggers.
        Event (trigger trigger1)
        Event (trigger trigger2)""").strip()

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Wait for processes to exit
    trace_event_generator_app.wait_for_exit()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # Check expected output
    #
    # If the outputs are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_without_notify_action(tap, test_env):
    """
    Validate that you can subscribe to listen to triggers that do not have the
    notify action. These triggers will never communicate a notification so it
    does not make sense to subscribe to them, but for the moment we allow it.

    Validate that you can still subscribe to other triggers in the list.
    """

    test_description = "Can subscribe to triggers without notify actions"
    tap.diagnostic("Start test: " + test_description)

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    client.create_session()

    # Add triggers
    tap.diagnostic("Add triggers")
    trigger1_name = "without-notify-action"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --action start-session '{session.name}'"
    )

    trigger2_name = "with-notify-action"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:end --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited unexpectedly")
        tap.test(False, test_description)
        return

    # Artificially produce the desired event-rule condition
    #
    # The passed arguments should result in two events:
    # • One emitted during a standard generation iteration
    # • One emitted just at the end
    tap.diagnostic("Launch the test application")
    trace_event_generator_app = test_env.launch_wait_trace_test_application(
        event_count=1, emit_end_event=True, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
    trace_event_generator_app.trace()

    # Waiting for the app to finish emitting events and to exit
    # is not necessary for synchronization since later we wait
    # for a specific number of lines.

    # Define expected output
    expected_output = textwrap.dedent("""
        Listening for notifications from all existing triggers.
        Event (trigger with-notify-action)""").strip()

    # Check if the trigger notifications were received
    tap.diagnostic("Check if the output was received")
    actual_output = []
    try:
        actual_output = wait_for_n_lines(
            trigger_notification_listener.stdout,
            n_lines=len(expected_output.splitlines()),
            timeout=TIMEOUT_DURATION_SECONDS,
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.test(False, test_description)
        return

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Temporary workaround.
    #
    # There is a bug in `handle_event_notification_pipe` where
    # notifications can be missed (because errors are handled before
    # checking if there is available data). Remove when bug is fixed:
    # • wait_before_exit=True option
    # • touch_exit_file()
    trace_event_generator_app.touch_exit_file()

    # Wait for processes to exit
    trace_event_generator_app.wait_for_exit()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # Check expected output
    #
    # If the outputs are identical, then no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_trigger_does_not_exist(tap, test_env):
    """
    Validate that you can only subscribe to listen to triggers that exist.
    """

    test_description = "Cannot subscribe to a trigger that does not exist"
    tap.diagnostic("Start test: " + test_description)

    client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)

    # Add triggers
    trigger1_name = "non-existant-trigger"

    trigger2_name = "trigger2"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:end --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            output_as_mi=False, triggers=[trigger1_name, trigger2_name]
        )
    except Exception as e:
        tap.diagnostic(e)
        tap.diagnostic("The trigger notification listener exited as expected")
        tap.test(True, test_description)
        return

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Wait for process to exit
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # The listener should not have launched successfully
    tap.test(False, test_description)


# TODO: Make tests for the different types of intrumentation that exist? e..g kernel syscall, log4j, python

if __name__ == "__main__":
    tests = [
        test_listen_all_triggers,
        test_listen_two_triggers_with_event_field_captures,
        test_listen_trigger_consumed_data_size_becomes_greater_than_machine_output,
        test_listen_trigger_consumed_data_size_becomes_greater_than_human_output,
        test_listen_trigger_buffer_usage_becomes_greater_than_machine_output,
        test_listen_trigger_buffer_usage_becomes_greater_than_human_output,
        test_listen_trigger_buffer_usage_becomes_less_than_machine_output,
        test_listen_trigger_buffer_usage_becomes_less_than_human_output,
        test_listen_trigger_rotation_starts_and_finishes_machine_output,
        test_listen_trigger_rotation_starts_and_finishes_human_output,
        test_listen_trigger_event_field_matches_machine_output,
        test_listen_two_triggers_same_event_rule,
        test_listen_without_notify_action,
        test_listen_trigger_does_not_exist,
    ]
    # The 6 greater/less than tests each have one additional tap test
    additional_tests = 6
    tap = lttngtest.TapGenerator(total_test_count=(len(tests) + additional_tests))

    # Necessary?
    # Check for platform requirements, if necessary
    # has_platform_requirements = True
    # if not has_platform_requirements:
    # This function will exit either with skipping or with a bailout,
    # depending on the the environment configuration.
    #    tap.missing_platform_requirements("Need XXX")

    for test in tests:
        with lttngtest.test_environment(
            with_sessiond=True, log=tap.diagnostic
        ) as test_env:
            test(tap, test_env)

        # Empty line for legibility
        print()

    sys.exit(0 if tap.is_successful else 1)
