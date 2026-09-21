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
    p.add_argument("--send-evicts", action="store_true",
                   help="have Ruby notify the core of evictions")
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

    traces = []
    for i in range(args.num_cores):
        inst_f = os.path.join(args.trace_dir, f"cpu{i}.inst.trace.gz")
        data_f = os.path.join(args.trace_dir, f"cpu{i}.data.trace.gz")
        sync_f = os.path.join(args.trace_dir, f"cpu{i}.sync.trace.gz")
        for f in (inst_f, data_f):
            if not os.path.isfile(f):
                fatal("missing trace: %s", f)
        if args.sync and not os.path.isfile(sync_f):
            fatal("--sync given but missing %s", sync_f)
        traces.append((inst_f, data_f, sync_f if args.sync else ""))

    from gem5.isas import ISA

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

    l1_config, l2_config, l3_config = chi_cache_configs()
    l1_config["size"] = args.l1_size
    l2_config["size"] = args.l2_size
    l3_config["size"] = args.l3_size

    from config.chi.l3_cache_hierarchy import L3CacheHierarchy

    hierarchy = L3CacheHierarchy(
        l1_config=l1_config,
        l2_config=l2_config,
        l3_config=l3_config,
        cores_per_cluster=args.cores_per_cluster,
    )
    board.cache_hierarchy = hierarchy
    hierarchy.incorporate_cache(board)

    root = Root(full_system=False, system=board)
    root.system.mem_mode = "timing"
    m5.instantiate()

    print(f"[trace_sim] CHI replay: {args.num_cores} TraceCPU(s) "
          f"L1={args.l1_size} L2={args.l2_size} L3={args.l3_size} "
          f"sync={'on' if args.sync else 'OFF'}")
    exit_event = m5.simulate()
    print(f"[trace_sim] done @ tick {m5.curTick()}: {exit_event.getCause()}")


if __name__ == "__m5_main__":
    main()
