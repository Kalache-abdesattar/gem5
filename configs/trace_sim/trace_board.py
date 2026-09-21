# SPDX-License-Identifier: BSD-3-Clause
"""
Shims that let TraceCPU drive a gem5-stdlib Ruby/CHI cache hierarchy.

The stdlib hierarchies (including this repo's config/chi/L3CacheHierarchy and
Chiplet) call a small, fixed interface on the board and on each core:

    board: get_processor / get_cache_line_size / get_clock_domain /
           get_mem_ports / has_io_bus / get_io_bus / has_dma_ports /
           get_dma_ports / connect_system_port
    core:  connect_icache / connect_dcache / connect_walker_ports /
           connect_interrupt

TraceCPU is a ClockedObject, not a BaseCPU, so it is not an AbstractCore and
has no MMU, no interrupt controller and no page-table walker. TraceCore
supplies that interface and ignores what does not apply, which is safe because
a replayed trace carries physical addresses and takes no faults.
"""

from m5.objects import (
    AddrRange,
    DDR4_2400_8x8,
    MemCtrl,
    Port,
    SrcClockDomain,
    SubSystem,
    System,
    TraceCPU,
    VoltageDomain,
)


class TraceCore:
    """AbstractCore-compatible wrapper around a TraceCPU."""

    def __init__(self, trace_cpu, isa=None, send_evicts=False):
        self.core = trace_cpu
        self._isa = isa
        self._send_evicts = send_evicts

    # -- the four methods a stdlib cache hierarchy calls on a core ----------
    def connect_icache(self, port: Port) -> None:
        self.core.icache_port = port

    def connect_dcache(self, port: Port) -> None:
        self.core.dcache_port = port

    def connect_walker_ports(self, port1: Port, port2: Port) -> None:
        # A TraceCPU has no MMU: a replayed trace carries physical addresses
        # and never walks. A real CPU wrapped for the CHI smoke test does,
        # so wire it when present.
        mmu = getattr(self.core, "mmu", None)
        if mmu is not None and hasattr(mmu, "connectWalkerPorts"):
            mmu.connectWalkerPorts(port1, port2)

    def connect_interrupt(self, interrupt_requestor=None,
                          interrupt_responce=None) -> None:
        # TraceCPU has no interrupt controller. A real CPU does, and on x86
        # the APIC must be wired. Mapping per
        # gem5/components/processors/base_cpu_core.py:
        #   interrupt_requestor -> pio and int_responder
        #   interrupt_responce  -> int_requestor
        if not hasattr(self.core, "createInterruptController"):
            return
        self.core.createInterruptController()
        if interrupt_requestor is not None:
            self.core.interrupts[0].pio = interrupt_requestor
            self.core.interrupts[0].int_responder = interrupt_requestor
        if interrupt_responce is not None:
            self.core.interrupts[0].int_requestor = interrupt_responce

    # -- occasionally probed by stdlib helpers -----------------------------
    def requires_send_evicts(self) -> bool:
        # Ruby uses this to decide whether the L1 must notify the core of
        # evictions, which exists for CPU models that keep their own
        # speculative state (O3's LSQ). A TraceCPU replays committed accesses
        # and keeps none, so in principle evictions need not be sent.
        return self._send_evicts

    def is_kvm_core(self) -> bool:
        return False

    def get_isa(self):
        return self._isa

    def set_switched_out(self, value: bool) -> None:
        pass

    def get_mmu(self):
        return None

    def get_simobject(self):
        return self.core


class TraceProcessor:
    """AbstractProcessor-compatible wrapper around a list of TraceCores."""

    def __init__(self, cores, isa):
        self._cores = cores
        self._isa = isa

    def get_cores(self):
        return self._cores

    def get_isa(self):
        return self._isa

    def get_num_cores(self):
        return len(self._cores)


class TraceBoard(System):
    """
    Minimal board exposing what a Ruby/CHI hierarchy needs.

    Not an stdlib AbstractBoard: those assume a workload, a processor that can
    be started and a full memory system. A trace replay has none of those, and
    inheriting the real thing would mean stubbing more than it saves.
    """

    def __init__(self, trace_cpus, isa, clk="2GHz", mem_size="4GB",
                 cache_line_size=64, send_evicts=False):
        super().__init__()
        self.clk_domain = SrcClockDomain(clock=clk,
                                         voltage_domain=VoltageDomain())
        self.mem_mode = "timing"
        self.cache_line_size = cache_line_size
        self.mem_ranges = [AddrRange(mem_size)]

        self.cpu = trace_cpus
        self._processor = TraceProcessor(
            [TraceCore(c, isa, send_evicts) for c in trace_cpus], isa)

        self.mem_ctrl = MemCtrl()
        self.mem_ctrl.dram = DDR4_2400_8x8()
        self.mem_ctrl.dram.range = self.mem_ranges[0]

    # -- board interface ----------------------------------------------------
    def get_processor(self):
        return self._processor

    def get_cache_line_size(self):
        return self.cache_line_size

    def get_clock_domain(self):
        return self.clk_domain

    def get_mem_ports(self):
        return [(self.mem_ranges[0], self.mem_ctrl.port)]

    def has_io_bus(self):
        return False

    def get_io_bus(self):
        raise NotImplementedError("TraceBoard has no IO bus")

    def has_dma_ports(self):
        return False

    def get_dma_ports(self):
        return []

    def connect_system_port(self, port: Port) -> None:
        self.system_port = port

    def has_coherent_io(self):
        return False

    def get_mem_side_coherent_io_port(self):
        raise NotImplementedError("TraceBoard has no coherent IO")
