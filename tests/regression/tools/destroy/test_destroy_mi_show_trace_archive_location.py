#!/usr/bin/env python3
#
# SPDX-FileCopyrightText: 2025 Olivier Dion <odion@efficios.com>
# SPDX-FileCopyrightText: 2025 Erica Bugden <ebugden@efficios.com>
# SPDX-License-Identifier: GPL-2.0-only

import pathlib
import sys


# Import in-tree test utils
test_utils_import_path = pathlib.Path(__file__).absolute().parents[3] / "utils"
sys.path.append(str(test_utils_import_path))
import lttngtest

tap = lttngtest.TapGenerator(total_test_count=1)

with lttngtest.test_environment(
    with_sessiond=True, log=tap.diagnostic, enable_kernel_domain=True
) as test_env:
    lttng_client = lttngtest.LTTngClient(test_env, log=tap.diagnostic)
    tracing_session = lttng_client.create_session()
    tracing_session.destroy()

sys.exit(0 if tap.is_successful else 1)
