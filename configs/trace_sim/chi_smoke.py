# SPDX-License-Identifier: BSD-3-Clause
"""
Split test: drive TraceBoard's CHI hierarchy with a REAL CPU.

Purpose is diagnostic, not simulation. replay_chi.py asserts inside the CHI
protocol (zero-length data bitmask at the L3). Two candidate causes:

  a) the TraceBoard/TraceCore shim sets the hierarchy up wrongly, or
  b) something about how TraceCPU issues requests does not suit CHI.

Running the identical board and hierarchy with a TimingSimpleCPU separates
them: if this asserts too, it is (a); if it runs, it is (b).
"""

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.util import fatal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_board import TraceBoard                       # noqa: E402
from replay_chi import chi_cache_configs                 # noqa: E402

_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__),
                                     "..", "..", ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--cmd", default="tests/test-progs/hello/bin/x86/linux/hello")
    p.add_argument("--num-cores", type=int, default=1)
    p.add_argument("--mem-size", default="512MB")
    p.add_argument("--maxinsts", type=int, default=100000)
    p.add_argument("--hierarchy", choices=("gem5-chi","repo-l3"),
                   default="gem5-chi")
    args = p.parse_args()

    if not os.path.isfile(args.cmd):
        fatal("workload not found: %s", args.cmd)

    from gem5.isas import ISA

    cpus = [X86TimingSimpleCPU(cpu_id=i) for i in range(args.num_cores)]
    board = TraceBoard(cpus, ISA.X86, mem_size=args.mem_size)

    board.workload = SEWorkload.init_compatible(args.cmd)
    for i, cpu in enumerate(cpus):
        proc = Process(pid=100 + i)
        proc.executable = args.cmd
        proc.cmd = [args.cmd]
        cpu.workload = proc
        cpu.createThreads()
        if args.maxinsts:
            cpu.max_insts_any_thread = args.maxinsts

    if args.hierarchy == "gem5-chi":
        from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy \
            import PrivateL1CacheHierarchy
        hierarchy = PrivateL1CacheHierarchy(size="32KiB", assoc=8)
    else:
        l1_config, l2_config, l3_config = chi_cache_configs()
        from config.chi.l3_cache_hierarchy import L3CacheHierarchy
        hierarchy = L3CacheHierarchy(
            l1_config=l1_config, l2_config=l2_config, l3_config=l3_config,
            cores_per_cluster=1,
        )
    board.cache_hierarchy = hierarchy
    hierarchy.incorporate_cache(board)

    root = Root(full_system=False, system=board)
    root.system.mem_mode = "timing"
    m5.instantiate()

    print(f"[chi_smoke] {args.num_cores} TimingSimpleCPU on the CHI hierarchy")
    e = m5.simulate()
    print(f"[chi_smoke] done @ tick {m5.curTick()}: {e.getCause()}")


if __name__ == "__m5_main__":
    main()
