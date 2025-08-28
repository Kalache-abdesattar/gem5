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

    def __init__(self, ruby_system, num_cores, cores_per_cluster, has_dma_ports):
        super().__init__()
        self.netifs = []

        # TODO: These should be in a base class
        # https://gem5.atlassian.net/browse/GEM5-1039
        self.ruby_system = ruby_system

        self._num_cores = num_cores
        self._cores_per_cluster = cores_per_cluster
        self._has_dma_ports = has_dma_ports

    def connectControllers(self, controllers):
        """
        """

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

        
        for core_idx in range(self._num_cores):
            l2_idx = 2 * self._num_cores + core_idx // self._cores_per_cluster

            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[2*core_idx], dst_node=self.routers[l2_idx]))

            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[2*core_idx+1], dst_node=self.routers[l2_idx]))

            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[l2_idx], dst_node=self.routers[2*core_idx]))

            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[l2_idx], dst_node=self.routers[2*core_idx+1]))
        


        # Internal links between L3 (10) and L2s (9, 8)
        num_l2s = self._num_cores // self._cores_per_cluster
        l3_idx = 2 * self._num_cores + num_l2s

        for i in range(num_l2s):
            l2_idx = 2 * self._num_cores + i 

            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[l2_idx], dst_node=self.routers[l3_idx]))
            
            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[l3_idx], dst_node=self.routers[l2_idx]))


        mem_ctrl_idx = l3_idx + 1
        # L3  ↔ MemCtrl 
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[l3_idx], dst_node=self.routers[mem_ctrl_idx]))
        link_count += 1
        int_links.append(SimpleIntLink(link_id=link_count, src_node=self.routers[mem_ctrl_idx], dst_node=self.routers[l3_idx]))

    
        if self._has_dma_ports: 
            dma0_idx = mem_ctrl_idx + 1
            # DMA0 ↔ L3 
            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count,
                                        src_node=self.routers[dma0_idx],
                                        dst_node=self.routers[l3_idx]))
            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count,
                                        src_node=self.routers[l3_idx],
                                        dst_node=self.routers[dma0_idx]))

            # DMA1 ↔ L3
            dma1_idx = mem_ctrl_idx + 2

            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count,
                                        src_node=self.routers[dma1_idx],
                                        dst_node=self.routers[l3_idx]))
            link_count += 1
            int_links.append(SimpleIntLink(link_id=link_count,
                                        src_node=self.routers[l3_idx],
                                        dst_node=self.routers[dma1_idx]))


        self.int_links = int_links