# Copyright (c) 2021 The Regents of the University of California
# All Rights Reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from itertools import chain
from typing import List

from m5.objects import (
    NULL,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
    RubyNetwork,
    RubyCache,
    RRIPRP,
)
from m5.objects.SubSystem import SubSystem

from gem5.coherence_protocol import CoherenceProtocol
from gem5.utils.requires import requires

requires(coherence_protocol_required=CoherenceProtocol.CHI)

from gem5.components.boards.abstract_board import AbstractBoard
from gem5.components.cachehierarchies.abstract_cache_hierarchy import (
    AbstractCacheHierarchy,
)
from gem5.components.cachehierarchies.ruby.abstract_ruby_cache_hierarchy import (
    AbstractRubyCacheHierarchy,
)
from gem5.components.cachehierarchies.chi.nodes.abstract_node import (
    AbstractNode,
)
from gem5.components.cachehierarchies.ruby.topologies.simple_pt2pt import (
    SimplePt2Pt,
)
from gem5.components.processors.abstract_core import AbstractCore
from gem5.isas import ISA
from gem5.utils.override import overrides

from .nodes.directory import SimpleDirectory
from .nodes.dma_requestor import DMARequestor
from .nodes.memory_controller import MemoryController
from .nodes.private_l1_moesi_cache import PrivateL1MOESICache



# Copyright (c) 2021 The Regents of the University of California.
# All Rights Reserved
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are
# met: redistributions of source code must retain the above copyright
# notice, this list of conditions and the following disclaimer;
# redistributions in binary form must reproduce the above copyright
# notice, this list of conditions and the following disclaimer in the
# documentation and/or other materials provided with the distribution;
# neither the name of the copyright holders nor the names of its
# contributors may be used to endorse or promote products derived from
# this software without specific prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
# "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
# LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
# A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
# OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from m5.objects import (
    SimpleExtLink,
    SimpleIntLink,
    SimpleNetwork,
    Switch,
)


class ChiNoC(SimpleNetwork):
    """A custom hierarchical network. This doesn't not use garnet -yet-."""

    def __init__(self, ruby_system):
        super().__init__()
        self.netifs = []

        # TODO: These should be in a base class
        # https://gem5.atlassian.net/browse/GEM5-1039
        self.ruby_system = ruby_system

    def connectControllers(self, controllers):
        """
        """
        # all_controllers = l1_controllers + l2_controllers + l3_controllers + [mem_ctrl]

        # Create one router/switch per controller in the system
        self.routers = [Switch(router_id=i) for i in range(len(controllers))]

        # Make a link from each controller to the router. The link goes
        # externally to the network.
        self.ext_links = [
            SimpleExtLink(link_id=i, ext_node=c, int_node=self.routers[i])
            for i, c in enumerate(controllers)
        ]


        link_count = 0
        int_links = []

        # Internal links between L3 (10) and L2s (9, 8)
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[9], dst_node=self.routers[10]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[8], dst_node=self.routers[10]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[10], dst_node=self.routers[9]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[10], dst_node=self.routers[8]))

        # cluster_0 (I:0, D:1) → L2_0 (8)
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[0], dst_node=self.routers[8]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[1], dst_node=self.routers[8]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[8], dst_node=self.routers[0]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[8], dst_node=self.routers[1]))

        # cluster_1 (I:2, D:3) → L2_0 (8)
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[2], dst_node=self.routers[8]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[3], dst_node=self.routers[8]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[8], dst_node=self.routers[2]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[8], dst_node=self.routers[3]))

        # cluster_2 (I:4, D:5) → L2_1 (9)
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[4], dst_node=self.routers[9]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[5], dst_node=self.routers[9]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[9], dst_node=self.routers[4]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[9], dst_node=self.routers[5]))

        # cluster_3 (I:6, D:7) → L2_1 (9)
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[6], dst_node=self.routers[9]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[7], dst_node=self.routers[9]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[9], dst_node=self.routers[6]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[9], dst_node=self.routers[7]))

        # L3 (10) ↔ MemCtrl (11)
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[10], dst_node=self.routers[11]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[11], dst_node=self.routers[10]))

        self.int_links = int_links