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

import m5
from m5.objects import (
    NULL,
    RubyPortProxy,
    RubySequencer,
    RubySystem,
    RubyNetwork,
    RubyCache,
    RRIPRP,
    AddrRange,
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
from gem5.components.cachehierarchies.chi.chi_noc import (
    ChiNoC,
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




class SharedL2(AbstractNode):
    """A home node (HNF) with a shared cache"""

    def __init__(
        self,
        size: str,
        assoc: int,
        network: RubyNetwork,
        cache_line_size: int,
    ):
        super().__init__(network, cache_line_size)

        self.cache = RubyCache(
            size=size,
            assoc=assoc,
            # Can choose any replacement policy
            replacement_policy=RRIPRP(),
        )


        # Only used for L1 controllers
        self.send_evictions = False
        self.sequencer = NULL

        # No prefetcher (home nodes don't support prefetchers right now)
        self.use_prefetcher = False
        self.prefetcher = NULL

        # Set up home node that allows three hop protocols
        self.is_HN = False 
        self.enable_DMT = False
        self.enable_DCT = False
        self.allow_SD = True


        # Some reasonable default TBE params
        self.number_of_TBEs = 32
        self.number_of_repl_TBEs = 32
        self.number_of_snoop_TBEs = 1
        self.number_of_DVM_TBEs = 1  # should not receive any dvm
        self.number_of_DVM_snoop_TBEs = 1  # should not receive any dvm
        self.unify_repl_TBEs = False

        # MOESI / Mostly inclusive for shared / Exclusive for unique
        self.alloc_on_seq_acc = True
        self.alloc_on_seq_line_write = True
        self.alloc_on_readshared = True
        self.alloc_on_readunique = True
        self.alloc_on_readonce = True
        self.alloc_on_writeback = True
        self.alloc_on_atomic = True
        
        ## Avoid conflicting “alloc & dealloc on same request”
        self.dealloc_on_unique       = False
        self.dealloc_on_shared       = False

        ## Enforce inclusion via child evictions/downgrades (back-inv)
        self.dealloc_backinv_unique  = True
        self.dealloc_backinv_shared  = True
        
        # Latencies 
        self.read_hit_latency = 12
        self.read_miss_latency = 14
        self.atomic_op_latency = 12
        self.write_fe_latency = 12  # Front-end: Rcv req -> Snd req
        self.write_be_latency = 12  # Back-end: Rcv ack -> Snd data
        self.fill_latency = 12
        self.snp_latency = 12
        self.snp_inv_latency = 12     



class SharedL3(AbstractNode):
    """A home node (HNF) with a shared cache"""

    def __init__(
        self,
        size: str,
        assoc: int,
        network: RubyNetwork,
        cache_line_size: int,
    ):
        super().__init__(network, cache_line_size)

        self.cache = RubyCache(
            size=size,
            assoc=assoc,
            # Can choose any replacement policy
            replacement_policy=RRIPRP(),
        )


        # Only used for L1 controllers
        self.send_evictions = False
        self.sequencer = NULL

        # No prefetcher (home nodes don't support prefetchers right now)
        self.use_prefetcher = False
        self.prefetcher = NULL

        # Set up home node that allows three hop protocols
        self.is_HN = True
        self.enable_DMT = False
        self.enable_DCT = False
        self.allow_SD = True


        # Some reasonable default TBE params
        self.number_of_TBEs = 32
        self.number_of_repl_TBEs = 32
        self.number_of_snoop_TBEs = 1
        self.number_of_DVM_TBEs = 1  # should not receive any dvm
        self.number_of_DVM_snoop_TBEs = 1  # should not receive any dvm
        self.unify_repl_TBEs = False

        # MOESI / Mostly inclusive for shared / Exclusive for unique
        self.alloc_on_seq_acc = True
        self.alloc_on_seq_line_write = True
        self.alloc_on_readshared = True
        self.alloc_on_readunique = True
        self.alloc_on_readonce = True
        self.alloc_on_writeback = True
        self.alloc_on_atomic = True
        self.dealloc_on_unique = False
        self.dealloc_on_shared = False
        self.dealloc_backinv_unique = False
        self.dealloc_backinv_shared = False

        # Latencies 
        self.read_hit_latency = 39
        self.read_miss_latency = 41
        self.atomic_op_latency = 39
        self.write_fe_latency = 39  # Front-end: Rcv req -> Snd req
        self.write_be_latency = 41  # Back-end: Rcv ack -> Snd data
        self.fill_latency = 39
        self.snp_latency = 39
        self.snp_inv_latency = 39       




class L3CacheHierarchy(AbstractRubyCacheHierarchy):
    """A two level cache based on CHI
    """

    def __init__(self, l1_size: str, l1_assoc: int, l2_size: str, l2_assoc: int, l3_size: str, l3_assoc: int, cores_per_cluster: int):
        """
        :param l1_size: The size of the priavte I/D caches in the hierarchy.
        :param l1_assoc: The associativity of each cache.
        :param l2_size: The size of the shared L2 cache.
        :param l2_assoc: The associativity of the shared L2 cache.
        """
        super().__init__()

        self._l1_size = l1_size
        self._l1_assoc = l1_assoc
        self._l2_size = l2_size
        self._l2_assoc = l2_assoc
        self._l3_size = l3_size
        self._l3_assoc = l3_assoc
        self._cores_per_cluster = cores_per_cluster

    def incorporate_cache(self, board):

        # Create the Ruby System. This is a singleton that is required for
        # all ruby protocols. Must be exactly 1 in the simulation.
        # Most Ruby controllers, etc. need a pointer to this.
        self.ruby_system = RubySystem()

        num_cores = len(board.get_processor().get_cores())

        # Ruby's global network.
        self.ruby_system.network = ChiNoC(self.ruby_system, num_cores, self._cores_per_cluster, board.has_dma_ports())

        # Network configurations
        # virtual networks: 0=request, 1=snoop, 2=response, 3=data
        self.ruby_system.number_of_virtual_networks = 4
        self.ruby_system.network.number_of_virtual_networks = 4

        # Create a single centralized L3/Home node
        self.l3cache = SharedL3(
            size=self._l3_size,
            assoc=self._l3_assoc,
            network=self.ruby_system.network,
            cache_line_size=board.get_cache_line_size()
        )
        self.l3cache.ruby_system = self.ruby_system

        
        num_l2_caches = num_cores // self._cores_per_cluster 
        l2_caches = [] 
        for i in range(num_l2_caches): 
            # Create an L2 node 
            l2_cache = SharedL2( 
                size=self._l2_size, 
                assoc=self._l2_assoc, 
                network=self.ruby_system.network, 
                cache_line_size=board.get_cache_line_size() 
            ) 
            l2_caches.append(l2_cache) 
            l2_caches[i].ruby_system = self.ruby_system


        # Create one core cluster with a split I/D cache for each core
        self.core_clusters = [
            self._create_core_cluster(core, i, board, l2_caches, self._cores_per_cluster)
            for i, core in enumerate(board.get_processor().get_cores())
        ]


        # Create the coherent side of the memory controllers
        self.memory_controllers = self._create_memory_controllers(board)


        # In CHI, you must explicitly set downstream controllers
        for cache in l2_caches:
            cache.downstream_destinations = self.l3cache

        self.l3cache.downstream_destinations = self.memory_controllers


        # Create the DMA Controllers, if required as in FS mode
        if board.has_dma_ports():
            dma_controllers = self._create_dma_controllers(board)
            self.ruby_system.num_of_sequencers = len(
                self.core_clusters
            ) * 2 + len(dma_controllers)
        else:
            dma_controllers = [] 
            self.ruby_system.num_of_sequencers = len(self.core_clusters) * 2


        # Connect the controllers within the network. Note that this function
        # makes assumptions on the order of the controllers. If you want to
        # use more complex topologies like mesh it would be a good idea to
        # tightly couple the network with the cache hierarchy.
        self.ruby_system.network.connectControllers(
            list(
                chain.from_iterable(  # Grab the controllers from each cluster
                    [
                        (cluster.dcache, cluster.icache)
                        for cluster in self.core_clusters
                    ]
                )
            )
            + l2_caches
            + [self.l3cache]
            + self.memory_controllers
            + dma_controllers
        )

        self.ruby_system.network.setup_buffers()

        # Set up a proxy port for the system_port. Used for load binaries and
        # other functional-only things.
        self.ruby_system.sys_port_proxy = RubyPortProxy()
        self.ruby_system.sys_port_proxy.ruby_system = self.ruby_system
        board.connect_system_port(self.ruby_system.sys_port_proxy.in_ports)


    def _create_core_cluster(
        self, core, core_num: int, board, l2_caches, cores_per_cluster
    ) -> SubSystem:
        """Given the core and the core number this function creates a cluster
        for the core with a split I/D cache.
        """
        # Create a cluster for each core.
        cluster = SubSystem()

        # Create the caches
        cluster.dcache = PrivateL1MOESICache(
            size=self._l1_size,
            assoc=self._l1_assoc,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
        )
        cluster.icache = PrivateL1MOESICache(
            size=self._l1_size,
            assoc=self._l1_assoc,
            network=self.ruby_system.network,
            core=core,
            cache_line_size=board.get_cache_line_size(),
            target_isa=board.get_processor().get_isa(),
            clk_domain=board.get_clock_domain(),
        )

        # The sequencers are used to connect the core to the cache
        cluster.icache.sequencer = RubySequencer(
            version=core_num, dcache=NULL, clk_domain=cluster.icache.clk_domain, ruby_system=self.ruby_system
        )
        cluster.dcache.sequencer = RubySequencer(
            version=core_num,
            dcache=cluster.dcache.cache,
            clk_domain=cluster.dcache.clk_domain,
            ruby_system=self.ruby_system
        )

        # If full system, connect the IO bus to the sequencer
        if board.has_io_bus():
            cluster.dcache.sequencer.connectIOPorts(board.get_io_bus())

        cluster.dcache.ruby_system = self.ruby_system
        cluster.icache.ruby_system = self.ruby_system

        # Connect the core "classic" ports to the sequencers
        core.connect_icache(cluster.icache.sequencer.in_ports)
        core.connect_dcache(cluster.dcache.sequencer.in_ports)

        # Same thing for the page table walkers
        core.connect_walker_ports(
            cluster.dcache.sequencer.in_ports,
            cluster.icache.sequencer.in_ports,
        )

        # Connect the interrupt ports
        if board.get_processor().get_isa() == ISA.X86:
            int_req_port = cluster.dcache.sequencer.interrupt_out_port
            int_resp_port = cluster.dcache.sequencer.in_ports
            core.connect_interrupt(int_req_port, int_resp_port)
        else:
            core.connect_interrupt()

        
        # Set the downstream destinations for the caches
        l2_idx = core_num // cores_per_cluster
        
        cluster.dcache.downstream_destinations = [l2_caches[l2_idx]]
        cluster.icache.downstream_destinations = [l2_caches[l2_idx]]
        
        return cluster


    def _create_memory_controllers(
        self, board
    ):
        """This creates the CHI objects that interact with gem5's memory
        controllers
        """
        memory_controllers = []
        for rng, port in board.get_mem_ports():
            mc = MemoryController(self.ruby_system.network, rng, port)
            mc.ruby_system = self.ruby_system
            memory_controllers.append(mc)
        return memory_controllers

    def _create_dma_controllers(
        self, board
    ):
        dma_controllers = []
        for i, port in enumerate(board.get_dma_ports()):
            ctrl = DMARequestor(
                self.ruby_system.network,
                board.get_cache_line_size(),
                board.get_clock_domain(),
            )
            version = len(board.get_processor().get_cores()) + i
            ctrl.sequencer = RubySequencer(version=version, in_ports=port)
            ctrl.sequencer.dcache = NULL

            ctrl.ruby_system = self.ruby_system
            ctrl.sequencer.ruby_system = self.ruby_system

            ctrl.downstream_destinations = [self.l3cache]

            dma_controllers.append(ctrl)

        return dma_controllers