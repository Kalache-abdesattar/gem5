# SPDX-License-Identifier: BSD-3-Clause
"""
trace_sim -- replay elastic traces through this repo's CHI hierarchy.

Same TraceCPUs and SyncArbiter as replay.py, but the classic L1/L2/XBar is
replaced by the Ruby CHI hierarchy from config/chi, so a recorded workload can
be driven at the RN-F / HN-F fabric.

Usage:
    build/X86_CHI/gem5.opt --outdir=m5out/chi configs/trace_sim/replay_chi.py \
        --trace-dir <dir> --num-cores 2 --sync
"""

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.util import fatal

# config/ lives two levels above gem5/configs/trace_sim
_REPO = os.path.abspath(os.path.join(os.path.dirname(__file__),
                                     "..", "..", ".."))
if _REPO not in sys.path:
    sys.path.insert(0, _REPO)

from trace_board import TraceBoard          # noqa: E402  (same directory)


def parse_args():
    p = argparse.ArgumentParser(description="trace_sim: CHI replay")
    p.add_argument("--trace-dir", required=True)
    p.add_argument("--num-cores", type=int, default=2)
    p.add_argument("--stats-period", type=int, default=0, metavar="TICKS",
                   help="dump stats every TICKS (0 = only at the end)")
    p.add_argument("--sync", action="store_true")
    p.add_argument("--clock", default="2GHz")
    p.add_argument("--mem-size", default="4GB")
    p.add_argument("--rob", type=int, default=192)
    p.add_argument("--lq", type=int, default=32)
    p.add_argument("--sq", type=int, default=32)
    p.add_argument("--l1-size", default="32kB")
    p.add_argument("--l2-size", default="256kB")
    p.add_argument("--l3-size", default="1MB")
    p.add_argument("--cores-per-cluster", type=int, default=1)
    p.add_argument("--num-l3-slices", type=int, default=1)
    p.add_argument("--l3-mshr-sample-period", type=int, default=0,
                   metavar="CYCLES",
                   help="repo-l3 only: sample each L3 slice's MSHR "
                        "occupancy and secondary misses every CYCLES L3 "
                        "cycles, into <slice>.mshr_samples.csv.gz in the "
                        "output directory (0 = off)")
    p.add_argument("--chi-data-channel-size", type=int, default=32,
                   help="CHI DAT channel width in bytes (repo-l3 only); 64 "
                        "matches the Questa DUT but breaks gem5's L3 path")
    p.add_argument("--hierarchy", choices=("gem5-chi-l2", "gem5-chi-l1",
                                          "repo-l3"),
                   default="gem5-chi-l2",
                   help="gem5-chi-l2 = stdlib PrivateL1SharedL2CacheHierarchy "
                        "(the only one that survives concurrent write sharing). "
                        "gem5-chi-l1 = stdlib PrivateL1CacheHierarchy, which "
                        "asserts under sharing even for gem5's own MemTest. "
                        "repo-l3 = config/chi/l3_cache_hierarchy.py, the "
                        "three-level hierarchy this repo's PARSEC runs use")
    p.add_argument("--no-dmt", action="store_true", default=True,
                   help="disable Direct Memory Transfer on the home node "
                        "(required for TraceCPU; see README)")
    p.add_argument("--send-evicts", action="store_true",
                   help="have Ruby notify the core of evictions")
    p.add_argument("--cpu-model", choices=("trace", "interval"),
                   default="trace",
                   help="trace = TraceCPU (recorded schedule). interval = "
                        "IntervalCPU (types, addresses and register deps "
                        "only; computes its own timing; no inst trace "
                        "needed). See replay.py.")
    p.add_argument("--dispatch-width", type=int, default=8,
                   help="IntervalCPU only (O3 default: 8)")
    p.add_argument("--issue-width", type=int, default=8,
                   help="IntervalCPU only (O3 default: 8)")
    p.add_argument("--commit-width", type=int, default=8,
                   help="IntervalCPU only (O3 default: 8)")
    p.add_argument("--iq", type=int, default=64,
                   help="IntervalCPU issue queue entries (O3 default: 64)")
    return p.parse_args()


def chi_cache_configs():
    """L1/L2/L3 dicts in the shape config/chi/nodes expects.

    Lifted verbatim from config/bench/x86-parsec.py so trace replay and the
    normal benchmark runs model the same caches; diverging here would make the
    two incomparable.
    """
    l1_config = {
        "size": "32KiB",
        "assoc": 8,
        "latencies": {
            "allocation_latency": 1,
            "read_hit_latency": 1,
            "read_miss_latency": 1,
            "atomic_op_latency": 1,
            "write_fe_latency": 1,
            "write_be_latency": 1,
            "fill_latency": 2,
            "snp_latency": 1,
            "snp_inv_latency": 1,
            "data_access_latency": 4,
            "tag_access_Latency": 1,
            "atomic_latency": 0,  # already included in atomic_op_latency
        },
        "TBE_params": {
            "number_of_TBEs": 8,
            "number_of_repl_TBEs": 8,
            "number_of_snoop_TBEs": 4,
            "number_of_DVM_TBEs": 8,
            "number_of_DVM_snoop_TBEs": 4,
            "unify_repl_TBEs": False,
        },
        "allow_SD": False,
        "prefetcher": None,
        "use_prefetcher": False,
        "clusivity": {
            "alloc_on_seq_acc": True,
            "alloc_on_seq_line_write": False,
            "alloc_on_readshared": True,
            "alloc_on_readunique": True,
            "alloc_on_readonce": True,
            "alloc_on_writeback": False,
            "alloc_on_atomic": False,
            "dealloc_on_unique": False,
            "dealloc_on_shared": False,
            "dealloc_backinv_unique": True,
            "dealloc_backinv_shared": True,
        },
    }

    l2_config = {
        "size": "256KiB",
        "assoc": 8,
        "latencies": {
            "allocation_latency": 1,
            "read_hit_latency": 3,
            "read_miss_latency": 4,
            "atomic_op_latency": 2,
            "write_fe_latency": 2,
            "write_be_latency": 2,
            "fill_latency": 4,
            "snp_latency": 3,
            "snp_inv_latency": 3,
            "data_access_latency": 6,
            "tag_access_Latency": 2,
            "atomic_latency": 0,  # already included in atomic_op_latency
        },
        "TBE_params": {
            "number_of_TBEs": 8,
            "number_of_repl_TBEs": 8,
            "number_of_snoop_TBEs": 4,
            "number_of_DVM_TBEs": 1,
            "number_of_DVM_snoop_TBEs": 1,
            "unify_repl_TBEs": False,
        },
        "allow_SD": False,
        "prefetcher": None,
        "use_prefetcher": False,
        "clusivity": {
            "alloc_on_seq_acc": True,
            "alloc_on_seq_line_write": True,
            "alloc_on_readshared": True,
            "alloc_on_readunique": True,
            "alloc_on_readonce": True,
            "alloc_on_writeback": True,
            "alloc_on_atomic": True,
            "dealloc_on_unique": False,
            "dealloc_on_shared": False,
            "dealloc_backinv_unique": True,
            "dealloc_backinv_shared": True,
        },
    }

    l3_config = {
        "size": "16MiB",
        "assoc": 16,
        "latencies": {
            "allocation_latency": 2,
            "read_hit_latency": 4,
            "read_miss_latency": 5,
            "atomic_op_latency": 6,
            "write_fe_latency": 2,
            "write_be_latency": 2,
            "fill_latency": 4,
            "snp_latency": 5,
            "snp_inv_latency": 5,
            "data_access_latency": 11,
            "tag_access_Latency": 3,
            "atomic_latency": 0,  # already included in atomic_op_latency
        },
        "TBE_params": {
            "number_of_TBEs": 64,
            "number_of_repl_TBEs": 64,
            "number_of_snoop_TBEs": 16,
            "number_of_DVM_TBEs": 1,
            "number_of_DVM_snoop_TBEs": 1,
            "unify_repl_TBEs": False,
        },
        "prefetcher": None,
        "allow_SD": False,
        "use_prefetcher": False,
        "clusivity": {
            "alloc_on_seq_acc": True,
            "alloc_on_seq_line_write": True,
            "alloc_on_readshared": True,
            "alloc_on_readunique": True,
            "alloc_on_readonce": True,
            "alloc_on_writeback": True,
            "alloc_on_atomic": True,
            "dealloc_on_unique": False,
            "dealloc_on_shared": False,
            "dealloc_backinv_unique": False,
            "dealloc_backinv_shared": False,
        },
    }
    return l1_config, l2_config, l3_config


def main():
    args = parse_args()

    interval = args.cpu_model == "interval"

    traces = []
    for i in range(args.num_cores):
        inst_f = os.path.join(args.trace_dir, f"cpu{i}.inst.trace.gz")
        data_f = os.path.join(args.trace_dir, f"cpu{i}.data.trace.gz")
        sync_f = os.path.join(args.trace_dir, f"cpu{i}.sync.trace.gz")
        for f in ((data_f,) if interval else (inst_f, data_f)):
            if not os.path.isfile(f):
                fatal("missing trace: %s", f)
        if args.sync and not os.path.isfile(sync_f):
            fatal("--sync given but missing %s", sync_f)
        traces.append((inst_f, data_f, sync_f if args.sync else ""))

    from gem5.isas import ISA

    if interval:
        cpus = [
            IntervalCPU(
                dataTraceFile=traces[i][1],
                syncTraceFile=traces[i][2],
                threadId=i,

                dispatchWidth=args.dispatch_width,
                issueWidth=args.issue_width,
                commitWidth=args.commit_width,
                robSize=args.rob,
                iqSize=args.iq,
                lqSize=args.lq,
                sqSize=args.sq,
            )
            for i in range(args.num_cores)
        ]
    else:
        cpus = [
            TraceCPU(
                instTraceFile=traces[i][0],
                dataTraceFile=traces[i][1],
                syncTraceFile=traces[i][2],
                sizeROB=args.rob,
                sizeLoadBuffer=args.lq,
                sizeStoreBuffer=args.sq,
            )
            for i in range(args.num_cores)
        ]

    board = TraceBoard(cpus, ISA.X86, clk=args.clock, mem_size=args.mem_size,
                       send_evicts=args.send_evicts)

    if args.sync:
        # num_cpus=0: infer participants from who registers, since a CPU with
        # an empty sync trace deliberately does not.
        board.arbiter = SyncArbiter(num_cpus=0)
        for cpu in cpus:
            cpu.arbiter = board.arbiter

    if args.hierarchy == "gem5-chi-l2":
        from gem5.components.cachehierarchies.chi\
            .private_l1_shared_l2_cache_hierarchy \
            import PrivateL1SharedL2CacheHierarchy
        hierarchy = PrivateL1SharedL2CacheHierarchy(
            l1_size=args.l1_size, l1_assoc=8,
            l2_size=args.l2_size, l2_assoc=8)
    elif args.hierarchy == "gem5-chi-l1":
        from gem5.components.cachehierarchies.chi.private_l1_cache_hierarchy \
            import PrivateL1CacheHierarchy
        hierarchy = PrivateL1CacheHierarchy(size=args.l1_size, assoc=8)
    else:
        # gem5's CHI asserts when the DAT channel is as wide as the cache line;
        # the repo default of 64 matches the Questa DUT, so narrow it here.
        import config.chi.nodes.abstract_node as _an
        _an.CHI_DATA_CHANNEL_SIZE = args.chi_data_channel_size
        l1_config, l2_config, l3_config = chi_cache_configs()
        l1_config["size"] = args.l1_size
        l2_config["size"] = args.l2_size
        l3_config["size"] = args.l3_size
        l3_config["mshr_sample_period"] = args.l3_mshr_sample_period
        from config.chi.l3_cache_hierarchy import L3CacheHierarchy
        hierarchy = L3CacheHierarchy(
            l1_config=l1_config,
            l2_config=l2_config,
            l3_config=l3_config,
            cores_per_cluster=args.cores_per_cluster,
            num_l3_slices=args.num_l3_slices,
        )

    board.cache_hierarchy = hierarchy
    hierarchy.incorporate_cache(board)

    if args.no_dmt:
        # The stdlib CHI home node enables Direct Memory Transfer, but
        # CHI-cache-actions.sm:1616 asserts use_DMT is false for any
        # non-atomic request reaching Send_ReadNoSnp. TraceCPU's request mix
        # hits that path; a TimingSimpleCPU on the same hierarchy does not.
        directory = getattr(hierarchy, "directory", None)
        dirs = directory if isinstance(directory, list) else [directory]
        for d in dirs:
            if d is not None and hasattr(d, "enable_DMT"):
                d.enable_DMT = False
        print("[trace_sim] DMT disabled on the home node(s)")

    root = Root(full_system=False, system=board)
    root.system.mem_mode = "timing"
    m5.instantiate()
    if args.stats_period:
        m5.stats.periodicStatDump(args.stats_period)

    print(f"[trace_sim] CHI replay: {args.num_cores} "
          f"{'IntervalCPU' if interval else 'TraceCPU'}(s) "
          f"hierarchy={args.hierarchy} L1={args.l1_size} "
          f"sync={'on' if args.sync else 'OFF'}")
    exit_event = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {exit_event.getCause()}")


if __name__ == "__m5_main__":
    main()
