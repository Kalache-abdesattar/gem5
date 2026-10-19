# SPDX-License-Identifier: BSD-3-Clause
"""
trace_sim -- run real O3 cores on the same CHI hierarchy replay_chi.py uses.

This exists to answer one question: is trace replay actually cheaper than
detailed simulation when the memory system is Ruby/CHI? Comparing TraceCPU+CHI
against the O3 capture run is not a fair comparison, because that capture used
the classic L1/L2/XBar. Both sides have to sit on the same hierarchy.

Each core runs its own independent copy of the workload, which mirrors what
replicate_traces.py produces for the replay side (N copies of one trace, one
per core). It is weak scaling, not a split workload -- see the README.

    build/X86_CHI/gem5.opt --outdir=m5out/o3chi configs/trace_sim/o3_chi.py \
        --cmd <binary> --options "stride" --num-cores 4
"""

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.util import fatal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_board import TraceBoard          # noqa: E402


def o3_cpu_class():
    """The O3 class name is ISA-prefixed; pick whichever this build has."""
    for name in ("X86O3CPU", "O3CPU", "DerivO3CPU"):
        cls = getattr(sys.modules["m5.objects"], name, None)
        if cls is not None:
            return cls
    fatal("no O3 CPU class in this build")


def parse_args():
    p = argparse.ArgumentParser(description="trace_sim: O3 on the CHI hierarchy")
    p.add_argument("--cmd", required=True)
    p.add_argument("--options", default="")
    p.add_argument("--num-cores", type=int, default=1)
    p.add_argument("--clock", default="2GHz")
    p.add_argument("--mem-size", default="4GB")
    p.add_argument("--rob", type=int, default=192)
    p.add_argument("--lq", type=int, default=32)
    p.add_argument("--sq", type=int, default=32)
    p.add_argument("--l1-size", default="32kB")
    p.add_argument("--l2-size", default="256kB")
    p.add_argument("--maxinsts", type=int, default=0,
                   help="stop after N instructions per core (0 = run to "
                        "completion). Use this when profiling, so every core "
                        "count does the same amount of work.")
    p.add_argument("--hierarchy", choices=("gem5-chi-l2", "gem5-chi-l1"),
                   default="gem5-chi-l2")
    p.add_argument("--send-evicts", action="store_true", default=True,
                   help="O3 keeps speculative LSQ state, so the L1 must "
                        "notify it of evictions. Unlike TraceCPU this is not "
                        "optional.")
    return p.parse_args()


def main():
    args = parse_args()
    if not os.path.isfile(args.cmd):
        fatal("workload not found: %s", args.cmd)

    from gem5.isas import ISA

    o3 = o3_cpu_class()
    cpus = [o3(cpu_id=i) for i in range(args.num_cores)]
    for cpu in cpus:
        cpu.numROBEntries = args.rob
        cpu.LQEntries = args.lq
        cpu.SQEntries = args.sq

    # TraceBoard already speaks the interface a stdlib CHI hierarchy calls,
    # and TraceCore wires an MMU and APIC when the core has them, which an O3
    # core does. Reusing it keeps both sides of the comparison on identical
    # board, memory and clock setup.
    board = TraceBoard(cpus, ISA.X86, clk=args.clock, mem_size=args.mem_size,
                       send_evicts=args.send_evicts)

    proc_args = args.options.split() if args.options else []
    board.workload = SEWorkload.init_compatible(args.cmd)
    for i, cpu in enumerate(cpus):
        proc = Process(pid=100 + i)
        proc.executable = args.cmd
        proc.cmd = [args.cmd] + proc_args
        cpu.workload = proc
        cpu.createThreads()
        if args.maxinsts:
            cpu.max_insts_any_thread = args.maxinsts

    if args.hierarchy == "gem5-chi-l2":
        from gem5.components.cachehierarchies.chi\
            .private_l1_shared_l2_cache_hierarchy \
            import PrivateL1SharedL2CacheHierarchy
        hierarchy = PrivateL1SharedL2CacheHierarchy(
            l1_size=args.l1_size, l1_assoc=8,
            l2_size=args.l2_size, l2_assoc=8)
    else:
        from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy \
            import PrivateL1CacheHierarchy
        hierarchy = PrivateL1CacheHierarchy(size=args.l1_size, assoc=8)

    board.cache_hierarchy = hierarchy
    hierarchy.incorporate_cache(board)

    # Same DMT workaround replay_chi.py needs: CHI-cache-actions.sm asserts
    # use_DMT is false for a non-atomic request reaching Send_ReadNoSnp.
    directory = getattr(hierarchy, "directory", None)
    dirs = directory if isinstance(directory, list) else [directory]
    for d in dirs:
        if d is not None and hasattr(d, "enable_DMT"):
            d.enable_DMT = False

    root = Root(full_system=False, system=board)
    root.system.mem_mode = "timing"
    m5.instantiate()

    print(f"[trace_sim] O3 on CHI: {args.num_cores} core(s) "
          f"hierarchy={args.hierarchy} ROB={args.rob} "
          f"L1={args.l1_size} L2={args.l2_size} "
          f"maxinsts={args.maxinsts or 'unlimited'}")
    exit_event = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {exit_event.getCause()}")


if __name__ == "__m5_main__":
    main()
