# SPDX-License-Identifier: BSD-3-Clause
"""
trace_sim -- inject Pin traffic into a Ruby/CHI hierarchy.

Diagnostic and practical route in one. TraceCPU + CHI hits a chain of protocol
asserts (see README); traffic generators are a well-trodden Ruby client, so if
this runs, Ruby/CHI is fine and the problem is TraceCPU-specific -- and this is
then the usable way to drive the CHI fabric from a hardware capture.

Each core gets two generators because a CHI core cluster has separate I and D
sequencers and a TrafficGen has a single port: the D generator replays the
trace, the I generator idles.
"""

import argparse
import glob
import json
import os
import sys

import m5
from m5.objects import *
from m5.util import fatal

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from trace_board import TraceBoard, TraceCore, TraceProcessor   # noqa: E402


def main():
    p = argparse.ArgumentParser(description="trace_sim: CHI traffic injection")
    p.add_argument("--trace-dir", required=True)
    p.add_argument("--num-cores", type=int, default=0)
    p.add_argument("--clock", default="2GHz")
    p.add_argument("--mem-size", default="512MB")
    p.add_argument("--l1-size", default="32kB")
    p.add_argument("--elastic", action="store_true")
    p.add_argument("--max-outstanding", type=int, default=16)
    p.add_argument("--no-dmt", action="store_true", default=True)
    args = p.parse_args()

    found = sorted(glob.glob(os.path.join(args.trace_dir, "cpu*.trace.gz")))
    found = [f for f in found if ".inst." not in f and ".sync." not in f]
    if args.num_cores:
        found = found[:args.num_cores]
    if not found:
        fatal("no cpu*.trace.gz in %s", args.trace_dir)

    meta_path = os.path.join(args.trace_dir, "traces.json")
    last_tick = 0
    if os.path.exists(meta_path):
        meta = json.load(open(meta_path))
        last_tick = max((v["last_tick"] for v in meta.values()), default=0)
    duration = int(last_tick * 4 + 1e10)

    from gem5.isas import ISA

    dgens = [PyTrafficGen(elastic_req=args.elastic,
                          max_outstanding_reqs=args.max_outstanding,
                          progress_check=f"{duration * 2}t")
             for _ in found]
    igens = [PyTrafficGen(progress_check=f"{duration * 2}t") for _ in found]

    board = TraceBoard(dgens, ISA.X86, clk=args.clock, mem_size=args.mem_size)
    board.igen = igens
    # Each "core" exposes the D generator on the dcache port and the idle I
    # generator on the icache port.
    cores = []
    for d, i in zip(dgens, igens):
        core = TraceCore(d, ISA.X86)
        core.connect_icache = (lambda port, g=i: setattr(g, "port", port))
        cores.append(core)
    board._processor = TraceProcessor(cores, ISA.X86)

    from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy \
        import PrivateL1CacheHierarchy
    hierarchy = PrivateL1CacheHierarchy(size=args.l1_size, assoc=8)
    board.cache_hierarchy = hierarchy
    hierarchy.incorporate_cache(board)

    if args.no_dmt:
        directory = getattr(hierarchy, "directory", None)
        for d in (directory if isinstance(directory, list) else [directory]):
            if d is not None and hasattr(d, "enable_DMT"):
                d.enable_DMT = False

    root = Root(full_system=False, system=board)
    root.system.mem_mode = "timing"
    m5.instantiate()

    def replay(gen, path):
        yield gen.createTrace(duration, path, 0)
        yield gen.createExit(0)

    def idle(gen):
        yield gen.createIdle(duration)
        yield gen.createExit(0)

    for g, path in zip(dgens, found):
        g.start(replay(g, path))
    for g in igens:
        g.start(idle(g))

    print(f"[trace_sim] CHI injection: {len(found)} generator(s), "
          f"elastic={args.elastic}")
    e = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {e.getCause()}")


if __name__ == "__m5_main__":
    main()
