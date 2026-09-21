#!/usr/bin/env python3
"""Convert the etrace pintool's output into gem5 elastic-trace protobuf files.

Produces, per thread, the pair TraceCPU consumes:
    cpuN.inst.trace.gz   (packet.proto)          -- instruction fetches
    cpuN.data.trace.gz   (inst_dep_record.proto) -- dependency graph

*** comp_delay is SYNTHESISED, not measured. ***

In real elastic traces comp_delay is a difference of O3 pipeline timestamps
(execute/toCommit/commit) per dependency edge, taken as the minimum across
edges. Pin has no pipeline and no cycles, so there is nothing to derive it
from. This script assigns a fixed per-type delay. The result is a synthetic
in-order timing model driving a real memory system -- it is NOT Elastic Traces,
and the accuracy figures quoted for gem5-captured traces do not transfer.

Needs the protobuf runtime plus packet_pb2 and inst_dep_record_pb2:
    protoc --python_out=<dir> -I src/proto src/proto/packet.proto
    protoc --python_out=<dir> -I src/proto src/proto/inst_dep_record.proto
"""

import argparse
import glob
import gzip
import os
import re
import sys

COMP, LOAD, STORE = 3, 1, 2
CMD_READ = 1


def varint(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


class Stream:
    def __init__(self, path):
        self.f = gzip.open(path, "wb")
        self.f.write(b"gem5")

    def write(self, msg):
        blob = msg.SerializeToString()
        self.f.write(varint(len(blob)) + blob)

    def close(self):
        self.f.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("indir", help="dir with <prefix>.tidN.txt from the etrace tool")
    ap.add_argument("outdir")
    ap.add_argument("--pb2-dir", required=True)
    ap.add_argument("--tick-freq", type=int, default=10**12)
    ap.add_argument("--window", type=int, default=576,
                    help="dependency window; deps older than this are dropped, "
                         "mirroring ElasticTrace's own depWindowSize")
    ap.add_argument("--comp-delay", type=int, default=500,
                    help="SYNTHETIC compute delay in ticks for COMP records "
                         "(500 = one cycle at 2 GHz with a 1e12 tick freq)")
    ap.add_argument("--load-delay", type=int, default=500)
    ap.add_argument("--store-delay", type=int, default=500)
    ap.add_argument("--page-size", type=int, default=4096)
    ap.add_argument("--phys-base", type=lambda x: int(x, 0), default=1 << 20)
    ap.add_argument("--max-records", type=int, default=0)
    ap.add_argument("--private-remap", action="store_true",
                    help="give each thread its own frame pool, destroying all "
                         "inter-thread sharing. Diagnostic only: it makes the "
                         "trace unrepresentative, but isolates whether a "
                         "failure depends on shared lines.")
    args = ap.parse_args()

    sys.path.insert(0, args.pb2_dir)
    import packet_pb2
    import inst_dep_record_pb2 as dep_pb2
    import sync_event_pb2

    os.makedirs(args.outdir, exist_ok=True)

    # Shared page remap across threads: Pin gives host virtual addresses far
    # outside any gem5 memory range, and sharing must be preserved.
    pgshift = args.page_size.bit_length() - 1
    pgmask = args.page_size - 1
    frames = {}

    cur_cpu = [0]

    def remap(a):
        vpn = (a >> pgshift, cur_cpu[0]) if args.private_remap else (a >> pgshift)
        f = frames.get(vpn)
        if f is None:
            f = args.phys_base + (len(frames) << pgshift)
            frames[vpn] = f
        return f | (a & pgmask)

    delay_of = {COMP: args.comp_delay, LOAD: args.load_delay,
                STORE: args.store_delay}

    files = sorted(glob.glob(os.path.join(args.indir, "*.tid*.txt")),
                   key=lambda p: int(re.search(r"tid(\d+)", p).group(1)))
    if not files:
        sys.exit(f"no *.tidN.txt in {args.indir}")

    for cpu, path in enumerate(files):
        cur_cpu[0] = cpu
        obj = f"etrace.cpu{cpu}"
        ipath = os.path.join(args.outdir, f"cpu{cpu}.inst.trace.gz")
        dpath = os.path.join(args.outdir, f"cpu{cpu}.data.trace.gz")
        spath = os.path.join(args.outdir, f"cpu{cpu}.sync.trace.gz")
        istream, dstream, sstream = Stream(ipath), Stream(dpath), Stream(spath)

        ih = packet_pb2.PacketHeader()
        ih.obj_id, ih.tick_freq = obj, args.tick_freq
        istream.write(ih)

        dh = dep_pb2.InstDepRecordHeader()
        dh.obj_id, dh.tick_freq, dh.window_size = obj, args.tick_freq, args.window
        dstream.write(dh)

        sh = sync_event_pb2.SyncEventHeader()
        sh.obj_id, sh.tick_freq = obj, args.tick_freq
        sstream.write(sh)

        n = nsync = 0
        last_fetch_line = None
        tick = 0
        with open(path) as src:
            for line in src:
                p = line.split()
                if not p:
                    continue
                if p[0] == "Y":
                    # Sync marker: "Y <type> <isEnd> <record-count>".
                    # num_insts is the record count, which is the unit
                    # TraceCPU counts in, so the arbiter compares like with
                    # like.
                    ev = sync_event_pb2.SyncEvent()
                    ev.type = int(p[1])
                    ev.is_end = int(p[2])
                    ev.num_insts = int(p[3])
                    ev.num_deps = 0
                    ev.tick = 0
                    ev.pc = 0
                    ev.tid = cpu
                    sstream.write(ev)
                    nsync += 1
                    continue
                if p[0] != "S":
                    continue
                seq = int(p[1]); pc = int(p[2]); typ = int(p[3])
                ea = int(p[4]); size = int(p[5])
                i = 6
                nreg = int(p[i]); i += 1
                regdeps = [int(x) for x in p[i:i + nreg]]; i += nreg
                nord = int(p[i]); i += 1
                orddeps = [int(x) for x in p[i:i + nord]]

                rec = dep_pb2.InstDepRecord()
                rec.seq_num = seq
                rec.type = typ
                rec.comp_delay = delay_of[typ]
                rec.pc = remap(pc)
                if typ != COMP:
                    rec.p_addr = remap(ea)
                    rec.size = size
                    rec.flags = 0
                # Drop dependencies older than the window, as ElasticTrace does.
                lo = seq - args.window
                for d in regdeps:
                    if d > lo:
                        rec.reg_dep.append(d)
                for d in orddeps:
                    if d > lo:
                        rec.rob_dep.append(d)
                dstream.write(rec)

                # I-side: one fetch per instruction cache line touched.
                fline = remap(pc) & ~63
                if fline != last_fetch_line:
                    ipkt = packet_pb2.Packet()
                    ipkt.tick = tick
                    ipkt.cmd = CMD_READ
                    ipkt.addr = fline
                    ipkt.size = 64
                    istream.write(ipkt)
                    last_fetch_line = fline
                    tick += args.comp_delay

                n += 1
                if args.max_records and n >= args.max_records:
                    break

        istream.close(); dstream.close(); sstream.close()
        print(f"[conv] cpu{cpu}: {n} records, {nsync} sync events")

    top = args.phys_base + (len(frames) << pgshift)
    print(f"[conv] remapped {len(frames)} pages -> footprint {top/2**20:.1f} MiB")
    print(f"[conv] comp_delay SYNTHETIC: comp={args.comp_delay} "
          f"load={args.load_delay} store={args.store_delay} ticks")


if __name__ == "__main__":
    main()
