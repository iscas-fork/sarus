# Sarus
#
# Copyright (c) 2018-2023, ETH Zurich. All rights reserved.
#
# Please, refer to the LICENSE file in the root directory.
# SPDX-License-Identifier: BSD-3-Clause

import unittest
import os

import common.util as util


class TestCommandLoad(unittest.TestCase):
    """
    These test verify that the Sarus's load command works correctly.
    """

    def test_command_load_with_local_repository(self):
        self._test_command_load(is_centralized_repository=False)

    def test_command_load_with_centralized_repository(self):
        self._test_command_load(is_centralized_repository=True)

    def _test_command_load(self, is_centralized_repository):
        util.remove_image_if_necessary(is_centralized_repository, "load/library/alpine:latest")
        image_name = "alpine"
        image_archive = os.path.dirname(os.path.realpath(__file__)) + "/saved_image.tar"

        util.load_image(is_centralized_repository, image_archive, image_name)
        assert util.is_image_available(is_centralized_repository, "load/library/alpine:latest")

        prettyname = util.run_image_and_get_prettyname(is_centralized_repository, "load/library/alpine:latest")
        assert prettyname.startswith("Alpine Linux")
