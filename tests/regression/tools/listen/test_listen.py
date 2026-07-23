#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: Erica Bugden <ebugden@efficios.com>
# SPDX-License-Identifier: GPL-2.0-only
#
"""
Validate the lttng-listen command.

This command listens for a user-specified set of LTTng trigger notifications.
If a notification is received, the listener displays it.
"""

import difflib
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
TIMEOUT_DURATION_SECONDS = 5 * 60


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

        # Read all available lines
        # TODO: It's still getting blocked when reading I think. It reads the first two
        # lines and then blocks
        # TODO: Figure out why I need to read all the lines that are available when the
        # selector says the file is ready
        for line in file_object:
            lines.append(line.decode("utf-8").rstrip())
            print(f"Output line {len(lines)}: {lines[-1]}")

            # TODO: Once reading from the file correctly is resolved, move this back to
            # the top of the loop to handle the n_lines=0 or negative cases.
            if len(lines) >= n_lines:
                break

    return lines


def test_listen_all_triggers(tap, test_env):
    """
    Validate that all triggers are listened for when no arguments are used.
    Validate that trigger notifications are successfully received.
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
        trigger_notification_listener = client.launch_trigger_notification_listener()
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

    # TODO: Use wait_for_n_lines() instead?
    # Wait for the trigger notifications to be received
    tap.diagnostic("Wait for the output to be received")
    actual_output = []
    file_listener = selectors.DefaultSelector()
    file_listener.register(trigger_notification_listener.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + TIMEOUT_DURATION_SECONDS

    while len(actual_output) < len(expected_output.splitlines()):
        if trigger_notification_listener.poll() is not None:
            tap.diagnostic("The trigger notification listener has exited")
            break

        # Wait for output or timeout
        remaining = deadline - time.monotonic()
        if not file_listener.select(timeout=remaining):
            tap.diagnostic(
                "Timed out while waiting for trigger notification listener output."
            )
            break

        actual_output.append(
            trigger_notification_listener.stdout.readline().decode("utf-8").rstrip()
        )
        # print(f"lttng-listen output: Line {len(actual_output)}: {actual_output[-1]}")  # Debug

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
    # If the outputs are identical, the no diff output will be
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
    Validate that all triggers are listened for when no arguments are used.
    Validate that trigger notifications and captured event field values are
    successfully received.
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
            triggers=[trigger1_name, trigger2_name]
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

    # TODO: Use wait_for_n_lines() instead?
    # Wait for the trigger notifications to be received
    tap.diagnostic("Wait for the output to be received")
    actual_output = []
    file_listener = selectors.DefaultSelector()
    file_listener.register(trigger_notification_listener.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + TIMEOUT_DURATION_SECONDS

    while len(actual_output) < len(expected_output.splitlines()):
        if trigger_notification_listener.poll() is not None:
            tap.diagnostic("The trigger notification listener has exited")
            break

        # Wait for output or timeout
        remaining = deadline - time.monotonic()
        if not file_listener.select(timeout=remaining):
            tap.diagnostic(
                "Timed out while waiting for trigger notification listener output."
            )
            break

        actual_output.append(
            trigger_notification_listener.stdout.readline().decode("utf-8").rstrip()
        )
        # print(f"lttng-listen output: Line {len(actual_output)}: {actual_output[-1]}")  # Debug

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
    # If the outputs are identical, the no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


def test_listen_trigger_consumed_data_size_becomes_greater_than(tap, test_env):
    """
    Validate that notifications from "Recording session consumed data
    size becomes greater than" triggers are received and displayed
    correctly.
    """

    test_description = "Successfully display 'consumed data size' trigger notification"
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
    tap.diagnostic("Add trigger")
    trigger1_name = "trigger1"
    threshold_bytes = 1
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition=session-consumed-size-ge "
        f"--session={tracing_session.name} --threshold-size={threshold_bytes} "
        "--action=notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener(
            triggers=[trigger1_name]
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
        event_count=1, wait_before_exit=True
    )
    # Tell the test app to begin emitting events
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
        f"XXX bytes of data. Notification threshold: {threshold_bytes} bytes."
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
    bytes_consumed = int(trigger_message[trigger_message.index("bytes") - 1])
    # TODO: At the moment I specify just one test per function. Would need to change that if I want to have more than one test
    # tap.test(bytes_consumed > threshold_bytes, "Number of bytes consumed is greater than the set threshold")
    if not (bytes_consumed > threshold_bytes):
        tap.diagnostic(
            "Number of bytes consumed should be greater than the set threshold"
        )
        tap.test(False, test_description)
        return

    # Since we cannot predict the exact number of bytes consumed, remove
    # the number of bytes from the actual output when comparing the
    # message.
    # TODO: Handle exception if the output is not the expected stuff?
    byte_number_placeholder = "XXX"
    trigger_message[(trigger_message.index("bytes") - 1)] = byte_number_placeholder
    actual_output[1] = " ".join(trigger_message)

    # Check expected message
    #
    # If the messages are identical, the no diff output will be
    # generated. If they are different, the diff will show.
    diff_line_count = 0
    for line in difflib.context_diff(
        expected_output.splitlines(), actual_output, "expected output", "actual output"
    ):
        tap.diagnostic(line)
        diff_line_count += 1

    tap.test(diff_line_count == 0, test_description)


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
        trigger_notification_listener = client.launch_trigger_notification_listener()
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
        event_count=1, emit_end_event=True
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

    # TODO: Use wait_for_n_lines() instead?
    # Wait for the trigger notifications to be received
    tap.diagnostic("Wait for the output to be received")
    actual_output = []
    file_listener = selectors.DefaultSelector()
    file_listener.register(trigger_notification_listener.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + TIMEOUT_DURATION_SECONDS

    while len(actual_output) < len(expected_output.splitlines()):
        if trigger_notification_listener.poll() is not None:
            tap.diagnostic("The trigger notification listener has exited")
            break

        # Wait for output or timeout
        remaining = deadline - time.monotonic()
        if not file_listener.select(timeout=remaining):
            tap.diagnostic(
                "Timed out while waiting for trigger notification listener output."
            )
            break

        actual_output.append(
            trigger_notification_listener.stdout.readline().decode("utf-8").rstrip()
        )
        # print(f"lttng-listen output: Line {len(actual_output)}: {actual_output[-1]}")

    # Stop listening. Send the SIGTERM signal.
    trigger_notification_listener.terminate()

    # Wait for processes to exit
    trace_event_generator_app.wait_for_exit()
    trigger_notification_listener.wait()

    # No need to explicitly remove the triggers since the session
    # daemon is torn down after each test.

    # Check expected output
    #
    # If the outputs are identical, the no diff output will be
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
    trigger1_name = "trigger1"
    client._run_cmd(
        f"add-trigger --name '{trigger1_name}' --condition event-rule-matches "
        "--type user --name tp:tptest --action start-session '{session.name}'"
    )

    trigger2_name = "trigger2"
    client._run_cmd(
        f"add-trigger --name '{trigger2_name}' --condition event-rule-matches "
        "--type user --name tp:end --action notify"
    )

    # Launch lttng-listen
    tap.diagnostic("Launch the trigger notification listener")
    try:
        trigger_notification_listener = client.launch_trigger_notification_listener()
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
        Event (trigger trigger2)""").strip()

    # TODO: Use wait_for_n_lines() instead?
    # Wait for the trigger notifications to be received
    tap.diagnostic("Wait for the output to be received")
    actual_output = []
    file_listener = selectors.DefaultSelector()
    file_listener.register(trigger_notification_listener.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + TIMEOUT_DURATION_SECONDS

    while len(actual_output) < len(expected_output.splitlines()):
        if trigger_notification_listener.poll() is not None:
            tap.diagnostic("The trigger notification listener has exited.")
            break

        # Wait for output or timeout
        remaining = deadline - time.monotonic()
        if not file_listener.select(timeout=remaining):
            tap.diagnostic(
                "Timed out while waiting for trigger notification listener output."
            )
            break

        actual_output.append(
            trigger_notification_listener.stdout.readline().decode("utf-8").rstrip()
        )
        # print(f"lttng-listen output: Line {len(actual_output)}: {actual_output[-1]}")

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
    # If the outputs are identical, the no diff output will be
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
            triggers=[trigger1_name, trigger2_name]
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


if __name__ == "__main__":
    tests = [
        test_listen_all_triggers,
        test_listen_two_triggers_with_event_field_captures,
        test_listen_trigger_consumed_data_size_becomes_greater_than,
        test_listen_two_triggers_same_event_rule,
        test_listen_without_notify_action,
        test_listen_trigger_does_not_exist,
    ]
    tap = lttngtest.TapGenerator(len(tests))

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
