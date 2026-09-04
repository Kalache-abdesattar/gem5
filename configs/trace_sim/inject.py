# SPDX-License-Identifier: BSD-3-Clause
"""
trace_sim option B -- SimMATE-style traffic injection.

Replaces cores with TrafficGen trace injectors replaying the fabric-visible
memory traffic captured by the l1filter Pin tool (L1 misses + writebacks),
converted to packet.proto by l1filter_to_packets.py.

The cache hierarchy, interconnect and memory are fully simulated; no CPU
timing model is involved, and none is invented.  This answers "what does this
address stream do to my memory system", not "how fast would a core run it".

Usage:
    build/X86_CHI/gem5.opt --outdir=m5out/inject configs/trace_sim/inject.py \
        --trace-dir <dir with cpuN.trace.gz> --num-cores 2 --elastic
"""

import argparse
import glob
import json
import os

import m5
from m5.objects import *
from m5.util import fatal


def parse_args():
    p = argparse.ArgumentParser(description="trace_sim: SimMATE-style injection")
    p.add_argument("--trace-dir", required=True)
    p.add_argument("--num-cores", type=int, default=0,
                   help="0 = one injector per cpuN.trace.gz found")
    p.add_argument("--clock", default="2GHz")
    p.add_argument("--mem-size", default="4GB")
    p.add_argument("--l1d", action="store_true",
                   help="model an L1 in front of each injector. Correct ONLY "
                        "for an unfiltered trace (l1filter -nofilter 1). A "
                        "default l1filter trace is already the L1 miss stream, "
                        "so adding an L1 here filters twice.")
    p.add_argument("--l1d-size", default="32kB")
    p.add_argument("--l2-size", default="1MB")
    p.add_argument("--no-l2", action="store_true")
    p.add_argument("--elastic", action="store_true",
                   help="pace injection off request completion instead of the "
                        "ticks in the trace (max-stress; avoids relying on a "
                        "synthesised inter-arrival time)")
    p.add_argument("--max-outstanding", type=int, default=16)
    return p.parse_args()


class L1D(Cache):
    assoc = 2; tag_latency = 1; data_latency = 1; response_latency = 1
    mshrs = 16; tgts_per_mshr = 20; write_buffers = 8


class L2(Cache):
    assoc = 16; tag_latency = 12; data_latency = 12; response_latency = 12
    mshrs = 32; tgts_per_mshr = 12; write_buffers = 8; writeback_clean = True


def main():
    args = parse_args()

    found = sorted(glob.glob(os.path.join(args.trace_dir, "cpu*.trace.gz")))
    if not found:
        fatal("no cpu*.trace.gz in %s", args.trace_dir)
    if args.num_cores:
        found = found[:args.num_cores]
    print(f"[trace_sim] injecting {len(found)} trace(s):")
    for f in found:
        print(f"[trace_sim]   {os.path.basename(f)}")

    system = System()
    system.clk_domain = SrcClockDomain(clock=args.clock,
                                       voltage_domain=VoltageDomain())
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange(args.mem_size)]
    system.membus = SystemXBar()
    system.cache_line_size = 64

    # PyTrafficGen.createTrace() replays a packet.proto file directly, so no
    # .cfg state machine is needed. It is gated on HAVE_PROTOBUF.
    # Size the replay window from the converter's sidecar. Every generator
    # gets the same duration so they all exit together; a generator whose
    # trace is shorter idles, so progress_check must exceed that idle gap.
    meta_path = os.path.join(args.trace_dir, "traces.json")
    last_tick = 0
    if os.path.exists(meta_path):
        meta = json.load(open(meta_path))
        last_tick = max((v["last_tick"] for v in meta.values()), default=0)
    duration = int(last_tick * 4 + 1e10)
    print(f"[trace_sim] replay window {duration} ticks "
          f"(longest trace ends at {last_tick})")

    system.gen = [
        PyTrafficGen(
            elastic_req=args.elastic,
            max_outstanding_reqs=args.max_outstanding,
            progress_check=f"{duration * 2}t",
        )
        for _ in found
    ]

    if not args.no_l2:
        system.l2bus = L2XBar()
        system.l2 = L2(size=args.l2_size)
        system.l2.cpu_side = system.l2bus.mem_side_ports
        system.l2.mem_side = system.membus.cpu_side_ports

    # An l1filter trace IS the L1 miss stream, so the simulated hierarchy
    # starts *below* the L1. Inserting another L1 here filters twice: the
    # first run of this config absorbed ~30% of already-filtered traffic and
    # reported 35238 L2 accesses for 50000 injected packets.
    for i, gen in enumerate(system.gen):
        if args.l1d:
            gen.l1d = L1D(size=args.l1d_size)
            gen.port = gen.l1d.cpu_side
            upstream = gen.l1d
        else:
            upstream = gen
        if args.no_l2:
            if args.l1d:
                upstream.mem_side = system.membus.cpu_side_ports
            else:
                gen.port = system.membus.cpu_side_ports
        else:
            if args.l1d:
                upstream.mem_side = system.l2bus.cpu_side_ports
            else:
                gen.port = system.l2bus.cpu_side_ports

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    root = Root(full_system=False, system=system)
    m5.instantiate()

    def replay(gen, path):
        # duration is a cap; the generator stops at end-of-trace anyway.
        yield gen.createTrace(duration, path, 0)
        yield gen.createExit(0)

    for gen, path in zip(system.gen, found):
        gen.start(replay(gen, path))

    print(f"[trace_sim] elastic={args.elastic} "
          f"max_outstanding={args.max_outstanding} "
          f"l1d={'modelled here' if args.l1d else 'not modelled'}")
    print("[trace_sim] NOTE: use --l1d only with an UNFILTERED trace "
          "(l1filter -nofilter 1). A default l1filter trace is already the L1\n"
          "[trace_sim]       miss stream; adding an L1 here filters twice.")
    exit_event = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {exit_event.getCause()}")


if __name__ == "__m5_main__":
    main()
