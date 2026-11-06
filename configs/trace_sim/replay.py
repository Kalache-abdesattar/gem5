# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""
trace_sim step 1/2 -- TRACE REPLAY.

Replaces every CPU with a TraceCPU that replays the per-core traces produced by
capture.py.  The cache hierarchy, interconnect and memory are still fully
simulated, so this measures how the *memory system* responds to a recorded
instruction/dependency stream.

Step 1 (working): N independent TraceCPUs, no inter-core synchronisation.
Step 3 (planned): an arbiter that stalls TraceCPUs at recorded sync events,
which is what ElasticSimMATE adds over stock Elastic Traces.

Usage:
    build/RISCV_CHI/gem5.opt configs/trace_sim/replay.py \
        --trace-dir m5out/capture --num-cores 1
"""

import argparse
import os

import m5
from m5.objects import *
from m5.util import fatal


def parse_args():
    p = argparse.ArgumentParser(description="trace_sim: elastic trace replay")
    p.add_argument("--trace-dir", required=True,
                   help="directory holding cpuN.{inst,data}.trace.gz")
    p.add_argument("--num-cores", type=int, default=1)
    p.add_argument("--cpu-clock", default="2GHz")
    p.add_argument("--mem-size", default="512MB")
    p.add_argument("--l1i-size", default="32kB")
    p.add_argument("--l1d-size", default="32kB")
    p.add_argument("--l2-size", default="1MB")
    p.add_argument("--no-l2", action="store_true")
    # TraceCPU models these at replay; capture ran with them oversized.
    p.add_argument("--progress", type=int, default=0, metavar="N",
                   help="print a progress line every N committed ops "
                        "(0 = silent). Useful to tell a slow replay from "
                        "a stalled one.")
    p.add_argument("--rob", type=int, default=40)
    p.add_argument("--lq", type=int, default=16)
    p.add_argument("--sq", type=int, default=16)
    p.add_argument("--sync", action="store_true",
                   help="honour the per-core sync traces via a SyncArbiter "
                        "(ElasticSimMATE). Without this the TraceCPUs replay "
                        "independently and every barrier is invisible.")
    p.add_argument("--cpu-model", choices=("trace", "interval"),
                   default="trace",
                   help="trace = TraceCPU, replays the recorded schedule "
                        "(comp_delay, rob_dep). interval = IntervalCPU, "
                        "uses only types, addresses and register deps from "
                        "the same data trace and computes its own timing; "
                        "the inst trace is not needed (it fetches by PC).")
    p.add_argument("--dispatch-width", type=int, default=4,
                   help="IntervalCPU only")
    p.add_argument("--issue-width", type=int, default=6,
                   help="IntervalCPU only")
    p.add_argument("--commit-width", type=int, default=4,
                   help="IntervalCPU only")
    p.add_argument("--iq", type=int, default=64,
                   help="IntervalCPU issue queue entries")
    p.add_argument("--no-fetch", action="store_true",
                   help="IntervalCPU: ideal front end (no icache fetches)")
    return p.parse_args()


class L1I(Cache):
    assoc = 2; tag_latency = 1; data_latency = 1; response_latency = 1
    mshrs = 4; tgts_per_mshr = 20; is_read_only = True
    writeback_clean = True


class L1D(Cache):
    assoc = 2; tag_latency = 1; data_latency = 1; response_latency = 1
    mshrs = 4; tgts_per_mshr = 20; write_buffers = 8


class L2(Cache):
    assoc = 16; tag_latency = 12; data_latency = 12; response_latency = 12
    mshrs = 20; tgts_per_mshr = 12; write_buffers = 8; writeback_clean = True


def main():
    args = parse_args()

    interval = args.cpu_model == "interval"

    traces = []
    for i in range(args.num_cores):
        inst_f = os.path.join(args.trace_dir, f"cpu{i}.inst.trace.gz")
        data_f = os.path.join(args.trace_dir, f"cpu{i}.data.trace.gz")
        for f in ((data_f,) if interval else (inst_f, data_f)):
            if not os.path.isfile(f):
                fatal("missing trace: %s", f)
        sync_f = os.path.join(args.trace_dir, f"cpu{i}.sync.trace.gz")
        if args.sync and not os.path.isfile(sync_f):
            fatal("--sync given but missing %s", sync_f)
        traces.append((inst_f, data_f, sync_f if args.sync else ""))

    system = System()
    system.clk_domain = SrcClockDomain(
        clock=args.cpu_clock, voltage_domain=VoltageDomain())
    system.mem_mode = TraceCPU.memory_mode()
    system.mem_ranges = [AddrRange(args.mem_size)]
    system.membus = SystemXBar()

    if args.sync:
        # num_cpus=0 means "infer from whoever registers". Passing
        # args.num_cores would be wrong: a CPU whose sync trace is empty
        # (a main thread that only spawns and joins) deliberately does
        # not register, and the barrier would wait forever for it.
        system.arbiter = SyncArbiter(num_cpus=0)

    if interval:
        system.cpu = [
            IntervalCPU(
                dataTraceFile=traces[i][1],
                syncTraceFile=traces[i][2],
                threadId=i,
                arbiter=(system.arbiter if args.sync else NULL),

                dispatchWidth=args.dispatch_width,
                issueWidth=args.issue_width,
                commitWidth=args.commit_width,
                robSize=args.rob,
                iqSize=args.iq,
                lqSize=args.lq,
                sqSize=args.sq,
                fetchEnabled=not args.no_fetch,
            )
            for i in range(args.num_cores)
        ]
    else:
        system.cpu = [
            TraceCPU(
                instTraceFile=traces[i][0],
                dataTraceFile=traces[i][1],
                syncTraceFile=traces[i][2],
                arbiter=(system.arbiter if args.sync else NULL),
                sizeROB=args.rob,
                sizeLoadBuffer=args.lq,
                sizeStoreBuffer=args.sq,
                progressMsgInterval=args.progress,
            )
            for i in range(args.num_cores)
        ]

    if not args.no_l2:
        system.l2bus = L2XBar()
        system.l2 = L2(size=args.l2_size)
        system.l2.cpu_side = system.l2bus.mem_side_ports
        system.l2.mem_side = system.membus.cpu_side_ports

    for cpu in system.cpu:
        cpu.icache = L1I(size=args.l1i_size)
        cpu.dcache = L1D(size=args.l1d_size)
        cpu.icache_port = cpu.icache.cpu_side
        cpu.dcache_port = cpu.dcache.cpu_side
        if args.no_l2:
            cpu.icache.mem_side = system.membus.cpu_side_ports
            cpu.dcache.mem_side = system.membus.cpu_side_ports
        else:
            cpu.icache.mem_side = system.l2bus.cpu_side_ports
            cpu.dcache.mem_side = system.l2bus.cpu_side_ports

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports

    # TraceCPU replays a recorded stream; there is no workload to load and no
    # interrupt controller to wire (it is a ClockedObject, not a BaseCPU).

    root = Root(full_system=False, system=system)
    m5.instantiate()

    model = ("IntervalCPU" if interval else "TraceCPU")
    widths = (f"W={args.dispatch_width}/{args.issue_width}/"
              f"{args.commit_width} IQ={args.iq} " if interval else "")
    print(f"[trace_sim] replay: {args.num_cores} {model}(s) {widths}"
          f"ROB={args.rob} LQ={args.lq} SQ={args.sq} "
          f"sync={'on' if args.sync else 'OFF (independent streams)'}")
    exit_event = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {exit_event.getCause()}")


if __name__ == "__m5_main__":
    main()
