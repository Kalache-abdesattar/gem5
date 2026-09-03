# Copyright (c) 2026
# SPDX-License-Identifier: BSD-3-Clause
"""
trace_sim step 1/2 -- TRACE CAPTURE.

Runs an SE-mode workload on N out-of-order cores with the ElasticTrace probe
attached, producing per-core instruction-fetch and data-dependency traces that
`replay.py` can play back through TraceCPU.

This differs from configs/common/CpuConfig.py:config_etrace(), which assigns
*the same* trace filename to every CPU -- fine for one core, wrong for N.  Here
each core gets its own pair of files, which is the prerequisite for multicore
replay (and, later, for the synchronisation trace).

Usage:
    build/RISCV_CHI/gem5.opt configs/trace_sim/capture.py \
        --cmd tests/test-progs/hello/bin/riscv/linux/hello \
        --num-cores 1 --out-dir m5out/capture
"""

import argparse
import os

import m5
from m5.objects import *
from m5.util import fatal


def o3_cpu_class():
    """Resolve the O3 CPU class for whichever ISA this gem5 was built for."""
    import m5.objects as objs
    for name in ("X86O3CPU", "RiscvO3CPU", "ArmO3CPU", "O3CPU"):
        if hasattr(objs, name):
            return getattr(objs, name)
    fatal("no O3 CPU class found; elastic trace capture requires an O3 build")


def parse_args():
    p = argparse.ArgumentParser(description="trace_sim: elastic trace capture")
    p.add_argument("--cmd", required=True, help="workload binary")
    p.add_argument("--options", default="", help="workload arguments")
    p.add_argument("--num-cores", type=int, default=1)
    p.add_argument("--threaded", action="store_true",
                   help="run ONE multithreaded process across all cores "
                        "(pthreads land on free thread contexts) instead of "
                        "one independent process per core")
    p.add_argument("--cpu-clock", default="2GHz")
    p.add_argument("--mem-size", default="512MB")
    p.add_argument("--l1i-size", default="32kB")
    p.add_argument("--l1d-size", default="32kB")
    p.add_argument("--l2-size", default="1MB")
    p.add_argument("--no-l2", action="store_true")
    p.add_argument("--no-etrace", action="store_true",
                   help="do not attach the probe: produces a REFERENCE O3 run "
                        "with realistic ROB/LQ/SQ, for accuracy comparison "
                        "against replay")
    p.add_argument("--maxinsts", type=int, default=0,
                   help="stop after N instructions (0 = run to completion)")
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


def build_system(args):
    system = System()
    system.clk_domain = SrcClockDomain(
        clock=args.cpu_clock, voltage_domain=VoltageDomain())
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange(args.mem_size)]
    system.membus = SystemXBar()

    o3 = o3_cpu_class()
    system.cpu = [o3(cpu_id=i) for i in range(args.num_cores)]

    if not args.no_l2:
        system.l2bus = L2XBar()
        system.l2 = L2(size=args.l2_size)
        system.l2.cpu_side = system.l2bus.mem_side_ports
        system.l2.mem_side = system.membus.cpu_side_ports

    for i, cpu in enumerate(system.cpu):
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
        cpu.createInterruptController()
        # x86 needs the local APIC wired to the bus; other ISAs do not have
        # these ports at all.
        if hasattr(cpu.interrupts[0], "pio"):
            cpu.interrupts[0].pio = system.membus.mem_side_ports
            cpu.interrupts[0].int_requestor = system.membus.cpu_side_ports
            cpu.interrupts[0].int_responder = system.membus.mem_side_ports

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR3_1600_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports
    system.system_port = system.membus.cpu_side_ports
    return system


def attach_probes(system, args):
    """Per-core elastic trace probes. One file pair per core.

    ElasticTrace resolves its output as
        <gem5 outdir>/<simobject path>.<param>
    (see src/cpu/o3/probe/elastic_trace.cc), so the parameter must be a bare
    filename -- passing a path embeds it inside the outdir.  The simobject path
    already differs per core, so the files are unique; normalise_names() renames
    them afterwards to the cpuN.* scheme replay.py expects.
    """
    for i, cpu in enumerate(system.cpu):
        cpu.traceListener = ElasticTrace(
            instFetchTraceFile="inst.trace.gz",
            dataDepTraceFile="data.trace.gz",
            depWindowSize=3 * cpu.numROBEntries,
        )
        # Deliberately oversized so the reference run does not stall on
        # resource limits; those stalls would be baked into the trace as
        # compute delay. TraceCPU models the real ROB/LQ/SQ at replay.
        cpu.numROBEntries = 512
        cpu.LQEntries = 128
        cpu.SQEntries = 128


def normalise_names(system, out_dir):
    """Rename <path>.inst.trace.gz -> cpuN.inst.trace.gz (stable for replay)."""
    for i, cpu in enumerate(system.cpu):
        base = cpu.traceListener.path()
        for kind in ("inst", "data", "sync"):
            src = os.path.join(out_dir, f"{base}.{kind}.trace.gz")
            dst = os.path.join(out_dir, f"cpu{i}.{kind}.trace.gz")
            if os.path.exists(src):
                os.replace(src, dst)
                print(f"[trace_sim]   {os.path.basename(dst)}: "
                      f"{os.path.getsize(dst)} bytes")
            else:
                print(f"[trace_sim]   MISSING {src}")


def main():
    args = parse_args()
    if not os.path.isfile(args.cmd):
        fatal("workload not found: %s", args.cmd)

    # ElasticTrace always writes into gem5's own outdir; --out-dir would be a
    # lie, so the traces land wherever --outdir points.
    out_dir = m5.options.outdir
    system = build_system(args)

    proc_args = args.options.split() if args.options else []
    system.workload = SEWorkload.init_compatible(args.cmd)
    if args.threaded:
        # One process, N thread contexts. pthread_create clones onto the free
        # contexts, which is how a real multithreaded workload runs -- and a
        # different path from N independent processes.
        proc = Process(pid=100)
        proc.executable = args.cmd
        proc.cmd = [args.cmd] + proc_args
        for cpu in system.cpu:
            cpu.workload = proc
            cpu.createThreads()
    else:
        for i, cpu in enumerate(system.cpu):
            proc = Process(pid=100 + i)
            proc.executable = args.cmd
            proc.cmd = [args.cmd] + proc_args
            cpu.workload = proc
            cpu.createThreads()

    # Must happen before instantiate(): probes are SimObjects too.
    if args.no_etrace:
        print("[trace_sim] reference mode: no probe, stock O3 sizing")
    else:
        attach_probes(system, args)

    if args.maxinsts:
        for cpu in system.cpu:
            cpu.max_insts_any_thread = args.maxinsts

    root = Root(full_system=False, system=system)
    m5.instantiate()

    print(f"[trace_sim] capture: {args.num_cores} core(s), traces -> {out_dir}")
    exit_event = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {exit_event.getCause()}")
    if not args.no_etrace:
        normalise_names(system, out_dir)


if __name__ == "__m5_main__":
    main()
