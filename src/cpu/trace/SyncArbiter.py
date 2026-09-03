# SPDX-License-Identifier: BSD-3-Clause
from m5.params import *
from m5.SimObject import SimObject


class SyncArbiter(SimObject):
    """Releases a set of TraceCPUs together at recorded synchronisation points.

    This is the piece ElasticSimMATE (Nocua et al., ReCoSoC 2017, Fig. 4b) adds
    over stock Elastic Traces: without it, N TraceCPUs replay independently and
    every barrier in the original program is invisible.
    """

    type = "SyncArbiter"
    cxx_header = "cpu/trace/sync_arbiter.hh"
    cxx_class = "gem5::SyncArbiter"

    num_cpus = Param.Unsigned(
        0,
        "Participants expected at each barrier. 0 = infer from the number of "
        "TraceCPUs that register.",
    )
