#!/usr/bin/env python3
"""Convert an l1filter (Intel Pin) trace into gem5 packet.proto traces.

This is the SimMATE half of the ElasticSimMATE lineage: the trace is the
*fabric-visible* memory traffic (L1 misses and writebacks), and gem5 replays it
through a fully simulated cache/interconnect/memory with traffic generators
standing in for cores.  No CPU timing is modelled or invented -- unlike the
elastic-trace path, nothing here pretends to reproduce an out-of-order pipeline.

Input  (l1filter):  "<tid> <RD|WR|WB> <0xline-addr>" per line
Output (gem5):      one <prefix>cpuN.trace.gz per thread id

MemCmd encoding follows src/mem/packet.hh: ReadReq = 1, WriteReq = 4
(same convention as util/encode_packet_trace.py).

Needs the Python protobuf runtime and packet_pb2:
    protoc --python_out=<dir> -I src/proto src/proto/packet.proto

Usage:
    l1filter_to_packets.py fabric.trace out/ --pb2-dir <dir> [--interval 1000]
"""

import argparse
import gzip
import os
import sys

CMD_READ = 1
CMD_WRITE = 4


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("infile", help="l1filter trace")
    ap.add_argument("outdir", help="output directory")
    ap.add_argument("--pb2-dir", required=True, help="dir holding packet_pb2.py")
    ap.add_argument("--line-size", type=int, default=64)
    ap.add_argument("--interval", type=int, default=1000,
                    help="ticks between successive requests from one thread. "
                         "Only meaningful without elastic_req; with elastic "
                         "injection gem5 paces off completions instead.")
    ap.add_argument("--tick-freq", type=int, default=10**12)
    ap.add_argument("--max-records", type=int, default=0)
    ap.add_argument("--prefix", default="cpu")
    ap.add_argument("--remap", choices=("page", "mask", "none"), default="page",
                    help="Pin emits host VIRTUAL addresses (up to ~140 TB) that "
                         "fall outside any sane gem5 memory range and would be "
                         "silently suppressed. 'page' assigns each unique 4 KiB "
                         "page a compact frame, preserving the page offset - "
                         "what an OS does, and it keeps intra-page locality and "
                         "line offsets intact. 'mask' folds addresses modulo "
                         "--mem-size. 'none' passes them through.")
    ap.add_argument("--page-size", type=int, default=4096)
    ap.add_argument("--mem-size", type=lambda x: int(x, 0), default=1 << 32)
    ap.add_argument("--phys-base", type=lambda x: int(x, 0), default=1 << 20)
    args = ap.parse_args()

    sys.path.insert(0, args.pb2_dir)
    import packet_pb2

    os.makedirs(args.outdir, exist_ok=True)
    streams, counts = {}, {}

    # One shared mapping across all threads: shared pages must stay shared, or
    # the whole point (coherence traffic) is destroyed.
    pgshift = args.page_size.bit_length() - 1
    pgmask = args.page_size - 1
    frames = {}

    def remap(a):
        if args.remap == "none":
            return a
        if args.remap == "mask":
            return a & (args.mem_size - 1)
        vpn = a >> pgshift
        f = frames.get(vpn)
        if f is None:
            f = args.phys_base + (len(frames) << pgshift)
            frames[vpn] = f
        return f | (a & pgmask)

    def stream_for(tid):
        if tid not in streams:
            path = os.path.join(args.outdir, f"{args.prefix}{tid}.trace.gz")
            f = gzip.open(path, "wb")
            f.write(b"gem5")
            hdr = packet_pb2.PacketHeader()
            hdr.obj_id = f"l1filter.{args.prefix}{tid}"
            hdr.tick_freq = args.tick_freq
            blob = hdr.SerializeToString()
            f.write(varint(len(blob)) + blob)
            streams[tid] = f
            counts[tid] = 0
            print(f"[conv] -> {path}")
        return streams[tid]

    total = skipped = 0
    with open(args.infile) as src:
        for line in src:
            parts = line.split()
            if len(parts) != 3:
                skipped += 1
                continue
            tid, kind, addr = parts
            try:
                addr = int(addr, 16)
            except ValueError:
                skipped += 1
                continue
            f = stream_for(tid)
            pkt = packet_pb2.Packet()
            # WB is a dirty-line eviction: a write as far as the fabric cares.
            pkt.cmd = CMD_READ if kind == "RD" else CMD_WRITE
            pkt.addr = remap(addr)
            pkt.size = args.line_size
            pkt.tick = counts[tid] * args.interval
            blob = pkt.SerializeToString()
            f.write(varint(len(blob)) + blob)
            counts[tid] += 1
            total += 1
            if args.max_records and total >= args.max_records:
                break

    for f in streams.values():
        f.close()
    # Sidecar so inject.py can size the replay window without needing the
    # protobuf runtime itself.
    import json
    meta = {f"{args.prefix}{tid}": {"packets": counts[tid],
                                    "last_tick": max(0, counts[tid] - 1) * args.interval}
            for tid in counts}
    with open(os.path.join(args.outdir, "traces.json"), "w") as mf:
        json.dump(meta, mf, indent=1)
    print(f"[conv] wrote {os.path.join(args.outdir, 'traces.json')}")
    print(f"[conv] {total} packets across {len(streams)} thread(s); "
          f"{skipped} lines skipped")
    if args.remap == "page":
        top = args.phys_base + (len(frames) << pgshift)
        print(f"[conv] remapped {len(frames)} pages -> physical footprint "
              f"{top/2**20:.1f} MiB (size gem5 mem >= this)")
    for tid in sorted(counts):
        print(f"[conv]   {args.prefix}{tid}: {counts[tid]} packets")


if __name__ == "__main__":
    main()
