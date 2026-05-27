#
# Copyright 2008,2009 Free Software Foundation, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#

import os

# Import the C++ module (with no base classes)
try:
    from .custom_dpdk_python import *
except ModuleNotFoundError:
    pass

# Import and export the Python wrapper classes
from .blocks import dpdk_downlink_sink, dpdk_uplink_source

__all__ = ['dpdk_downlink_sink', 'dpdk_uplink_source']
