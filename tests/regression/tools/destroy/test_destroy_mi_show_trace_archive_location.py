#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2025 Olivier Dion <odion@efficios.com>
# SPDX-FileCopyrightText: 2025 Erica Bugden <ebugden@efficios.com>
# SPDX-License-Identifier: GPL-2.0-only

import pathlib
import sys

# TODO
# - Test one of the error paths too?
# - Put this python file in the MI tests folder instead? I think that may be a good fit.

# Import in-tree test utils
test_utils_import_path = pathlib.Path(__file__).absolute().parents[3] / "utils"
sys.path.append(str(test_utils_import_path))
import lttngtest

# Test that when destroying a session with a completed rotation, the command's machine interface prints the trace archive location

# Needs `if __name__ == "__main__":`?

tap = lttngtest.TapGenerator(total_test_count=1)

with lttngtest.test_environment(
    with_sessiond=True, log=tap.diagnostic, enable_kernel_domain=True
) as test_env:
    lttng_client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = lttng_client.create_session()
    # tracing_session.ad

    # enable some events
    # run session
    # rotate session

    # tracing_session.rotate()
    # TODO: Calling destroy explodes atm... Maybe revert my changes and add them back slowly.
    # It looks like the default is MI (see output format in `_run_cmd`)
    tracing_session.destroy()

# check that the mi output has the rotation location (should also run it manually separately)

# I don't have any kernel tests so don't need to worry about those parts.

sys.exit(0 if tap.is_successful else 1)
