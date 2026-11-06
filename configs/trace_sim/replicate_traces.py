#!/usr/bin/env python3
"""Replicate N captured traces onto M >= N cores for weak-scaling studies.

ElasticSimMATE section III-A4b. Core i replays source trace (i mod N) with its
*data* addresses shifted by (i // N) * stride.

Address offsetting is the part the paper leaves out, and it says so: without it
"all cores issue requests to the same addresses ... which results in unrealistic
data sharing during replay", and they measure "well above 80% data sharing in
most experiments". Weak scaling means each core gets its own slice of a larger
problem, so replicas must not collide.

Only *data* addresses are shifted. The program counter is left alone: replicas
are threads of one program, so they share code, and shifting the PC would
inflate the instruction footprint with copies that do not exist.

    replicate_traces.py <src-dir> <out-dir> --cores 8 --pb2-dir <dir>
"""

import argparse
import glob
import gzip
import os
import re
import sys


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def read_varint(f):
    shift = result = 0
    while True:
        b = f.read(1)
        if not b:
            return None
        b = b[0]
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result
        shift += 7


def copy_stream(src, dst, msg_factory, mutate=None):
    """Copy a gem5 protobuf stream, optionally mutating each record."""
    n = 0
    with gzip.open(src, "rb") as fi, gzip.open(dst, "wb") as fo:
        magic = fi.read(4)
        if magic != b"gem5":
            sys.exit(f"{src}: bad magic {magic!r}")
        fo.write(b"gem5")
        # header
        ln = read_varint(fi)
        hdr_blob = fi.read(ln)
        fo.write(varint(ln) + hdr_blob)
        while True:
            ln = read_varint(fi)
            if ln is None:
                break
            blob = fi.read(ln)
            if mutate is None:
                fo.write(varint(ln) + blob)
            else:
                m = msg_factory()
                m.ParseFromString(blob)
                mutate(m)
                out = m.SerializeToString()
                fo.write(varint(len(out)) + out)
            n += 1
    return n


def _one_core(job):
    """Replicate one target core.  Runs in a worker process."""
    i, src_idx, rep, off, srcdir, outdir, pb2_dir = job
    if pb2_dir and pb2_dir not in sys.path:
        sys.path.insert(0, pb2_dir)
    import inst_dep_record_pb2 as dep_pb2
    import packet_pb2

    base = os.path.join(srcdir, f"cpu{src_idx}")
    out = os.path.join(outdir, f"cpu{i}")

    def shift_data(m):
        if off and m.HasField("p_addr"):
            m.p_addr += off
        # pc deliberately untouched: replicas share code.

    nd = copy_stream(base + ".data.trace.gz", out + ".data.trace.gz",
                     dep_pb2.InstDepRecord, shift_data if off else None)
    ni = copy_stream(base + ".inst.trace.gz", out + ".inst.trace.gz",
                     packet_pb2.Packet, None)
    ns = 0
    sync_src = base + ".sync.trace.gz"
    if os.path.exists(sync_src):
        import sync_event_pb2
        ns = copy_stream(sync_src, out + ".sync.trace.gz",
                         sync_event_pb2.SyncEvent, None)
    return (i, src_idx, rep, off, nd, ni, ns)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("srcdir")
    ap.add_argument("outdir")
    ap.add_argument("--cores", type=int, required=True, help="M, the target core count")
    ap.add_argument("--pb2-dir", required=True)
    ap.add_argument("--stride", type=lambda x: int(x, 0), default=0,
                    help="bytes between replica address spaces. 0 = derive "
                         "from the observed footprint, rounded up to a power "
                         "of two.")
    ap.add_argument("--jobs", type=int, default=0,
                    help="worker processes (default: all cores)")
    ap.add_argument("--no-offset", action="store_true",
                    help="replicate without shifting addresses, reproducing "
                         "the paper's behaviour. Every replica then hits the "
                         "same lines, which manufactures sharing that the "
                         "original program never had.")
    args = ap.parse_args()

    sys.path.insert(0, args.pb2_dir)
    import inst_dep_record_pb2 as dep_pb2
    import packet_pb2

    srcs = sorted(glob.glob(os.path.join(args.srcdir, "cpu*.data.trace.gz")),
                  key=lambda p: int(re.search(r"cpu(\d+)", p).group(1)))
    if not srcs:
        sys.exit(f"no cpu*.data.trace.gz in {args.srcdir}")
    n_src = len(srcs)
    if args.cores < n_src:
        sys.exit(f"--cores {args.cores} is fewer than the {n_src} source traces")
    os.makedirs(args.outdir, exist_ok=True)

    # Derive the stride from the highest data address actually touched.
    stride = args.stride
    if stride == 0 and not args.no_offset:
        hi = 0
        for src in srcs:
            with gzip.open(src, "rb") as fi:
                fi.read(4)                      # magic
                fi.read(read_varint(fi))        # header
                while True:
                    ln = read_varint(fi)
                    if ln is None:
                        break
                    m = dep_pb2.InstDepRecord()
                    m.ParseFromString(fi.read(ln))
                    if m.HasField("p_addr"):
                        end = m.p_addr + (m.size if m.HasField("size") else 0)
                        hi = max(hi, end)
        stride = 1
        while stride < max(hi, 1):
            stride <<= 1
        print(f"[repl] data footprint top 0x{hi:x} -> stride 0x{stride:x}")

    # Each output core is an independent stream, so fan the work out across
    # processes.  Rewriting 16 copies of a multi-million-record trace is
    # otherwise half an hour of single-threaded protobuf work.
    jobs = []
    for i in range(args.cores):
        jobs.append((i, i % n_src, i // n_src,
                     0 if args.no_offset else (i // n_src) * stride,
                     args.srcdir, args.outdir, args.pb2_dir))

    nproc = min(args.jobs or (os.cpu_count() or 1), len(jobs))
    if nproc > 1:
        import multiprocessing as mp
        with mp.Pool(nproc) as pool:
            results = pool.map(_one_core, jobs)
    else:
        results = [_one_core(j) for j in jobs]

    total = 0
    for i, src_idx, rep, off, nd, ni, ns in results:
        print(f"[repl] cpu{i} <- cpu{src_idx} replica {rep} offset 0x{off:x} "
              f"({nd} data, {ni} inst, {ns} sync)")
        total += nd

    print(f"[repl] {args.cores} cores from {n_src} source trace(s), "
          f"{total} data records total"
          + ("  [NO OFFSET - replicas collide, as in the paper]"
             if args.no_offset else ""))


if __name__ == "__main__":
    main()
