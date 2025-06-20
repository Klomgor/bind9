# Copyright (C) Internet Systems Consortium, Inc. ("ISC")
#
# SPDX-License-Identifier: MPL-2.0
#
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0.  If a copy of the MPL was not distributed with this
# file, you can obtain one at https://mozilla.org/MPL/2.0/.
#
# See the COPYRIGHT file distributed with this work for additional
# information regarding copyright ownership.

import os
from pathlib import Path

from docutils import nodes

from sphinx.application import Sphinx
from sphinx.util.docutils import SphinxDirective
from sphinx.util.typing import ExtensionMetadata


BIND_BUILD_ROOT = os.getenv("BIND_BUILD_ROOT")
if BIND_BUILD_ROOT is None:
    raise RuntimeError("running outside meson?")

miscpath = Path(BIND_BUILD_ROOT) / "doc" / "misc"


class ConfigBlockDirective(SphinxDirective):
    required_arguments = 1

    def run(self) -> list[nodes.Node]:
        target = miscpath / self.arguments[0]

        block = "{}" if not target.exists() else target.read_text()

        return [nodes.literal_block(text=block)]


def setup(app: Sphinx) -> ExtensionMetadata:
    app.add_directive("configblock", ConfigBlockDirective)

    return {
        "version": "0.1",
        "parallel_read_safe": True,
        "parallel_write_safe": True,
    }
