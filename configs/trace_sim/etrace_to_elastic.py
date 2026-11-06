#!/usr/bin/env python3
"""Convert the etrace pintool's output into gem5 elastic-trace protobuf files.

Produces, per thread, the pair TraceCPU consumes:
    cpuN.inst.trace.gz   (packet.proto)          -- instruction fetches
    cpuN.data.trace.gz   (inst_dep_record.proto) -- dependency graph

*** comp_delay is SYNTHESISED, not measured. ***

In real elastic traces comp_delay is a difference of O3 pipeline timestamps
(execute/toCommit/commit) per dependency edge, taken as the minimum across
edges. Pin has no pipeline and no cycles, so there is nothing to derive it
from directly.

Rather than a fixed per-type constant, the default --delay-model table assigns
each record the median comp_delay that gem5's own elastic traces show for its
(record type, distance-to-nearest-dependency-producer). Producer distance is
the one strong predictor Pin can compute exactly -- it already tracks
last-writer per register -- and on a pointer chase it separates the two timing
modes completely. See the _DELAY_TABLE comment below for the measurements.

The result is still a *modelled* timing source driving a real memory system --
it is NOT Elastic Traces,
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


# ─── Delay model ─────────────────────────────────────────────────────────────
# A flat per-type constant is badly wrong for loads.  Measured against gem5's
# own elastic traces (capped at 12000, which is what reproduces an O3 run to
# ~0.0007% at one core), the ground truth in steady state is:
#
#   LOAD   median 12000 -- loads sit at the cap in almost every workload, and
#          not because of cache misses: even the L1-resident kernel shows 99.6%
#          of loads there.  It is issue queueing, which Pin cannot observe
#          directly but can infer, see below.
#   STORE  median 1000-1500
#   COMP   median 0-500
#
# The one strong, Pin-observable predictor is the *sequence distance* from an
# instruction back to its nearest dependency producer.  Pin already tracks
# last-writer per register, so it knows this exactly.  On a pointer chase the
# distance alone separates the two modes completely: distance 1 -> 500,
# distance 9-16 -> 12000.  The mechanism is that an address computed in the
# immediately preceding micro-op lets the load issue right behind it, whereas a
# producer several micro-ops back means the load has been sitting in the queue.
#
# Table below is (type, distance bucket) -> median comp_delay, pooled over the
# l1/l2/chase/stride/seq/fp gem5 traces sampled past program startup.
# Buckets are (low, high] in sequence-number distance; 0 means "no dependency".
_DELAY_TABLE = {
    # type:  [(max_distance, delay), ...]  first match wins
    LOAD:  [(0, 500), (1, 500), (4, 6000), (10**9, 12000)],
    STORE: [(0, 1500), (1, 1500), (4, 1500), (8, 1000), (32, 1500), (10**9, 12000)],
    COMP:  [(0, 500), (1, 500), (2, 0), (4, 500), (8, 1000), (16, 500),
            (32, 1500), (10**9, 12000)],
}


def modelled_delay(typ, dist):
    """comp_delay for one record, from its type and producer distance."""
    for lim, d in _DELAY_TABLE[typ]:
        if dist <= lim:
            return d
    return 500


# ─── Opcode classification (shared with the table derivation) ────────────────
# objdump and Pin spell mnemonics differently, so both sides reduce to a
# functional-unit class instead of matching raw names.
import re
_CLASS = [
    ("FP_DIV",  re.compile(r"^(div|sqrt)(sd|ss|pd|ps)$|^fdiv|^fsqrt", re.I)),
    ("FP_MUL",  re.compile(r"^(mul)(sd|ss|pd|ps)$|^fmul", re.I)),
    ("FP_ADD",  re.compile(r"^(add|sub|max|min)(sd|ss|pd|ps)$|^fadd|^fsub", re.I)),
    ("FP_CVT",  re.compile(r"^cvt", re.I)),
    ("FP_MOV",  re.compile(r"^(movs[sd]|movap[sd]|movup[sd]|movdq[au]|movq|movd)$", re.I)),
    ("INT_DIV", re.compile(r"^i?div$", re.I)),
    ("INT_MUL", re.compile(r"^i?mul$", re.I)),
    ("BRANCH",  re.compile(r"^(jmp|j[a-z]{1,3}|call|ret|loop)", re.I)),
    ("STACK",   re.compile(r"^(push|pop|leave|enter)", re.I)),
    ("INT_ALU", re.compile(r"^(add|sub|and|or|xor|cmp|test|lea|inc|dec|neg|not|"
                           r"shl|shr|sar|sal|rol|ror|adc|sbb)", re.I)),
    ("MOV",     re.compile(r"^mov", re.I)),
    ("NOP",     re.compile(r"^(nop|endbr)", re.I)),
]
def opclass(mn):
    if not mn: return "OTHER"
    mn = mn.strip().lower()
    mn = re.sub(r"^(lock|rep|repne|repz)\s+", "", mn)
    for name, rx in _CLASS:
        if rx.match(mn): return name
    return "OTHER"


_DIST_BUCKETS = [(0, 0), (1, 1), (2, 2), (3, 4), (5, 8), (9, 16), (17, 32),
                 (33, 1 << 30)]


def _dbucket(d):
    for lo, hi in _DIST_BUCKETS:
        if lo <= d <= hi:
            return f"{lo}" if lo == hi else (f"{lo}+" if hi > 1 << 29
                                             else f"{lo}-{hi}")
    return "33+"


# (record type, distance bucket, opcode class) -> median comp_delay, measured
# from gem5's own elastic traces.  Sampled at three depths (30/55/80%) of every
# kernel's trace so no single phase or workload dominates -- the earlier
# two-feature table was fitted on a fixed 3-4M record window, which excluded
# all steady-state data from the longest traces and mispredicted them badly.
#
# The opcode class is what separates cases that (type, distance) conflates:
# at LOAD distance 5-8, an INT_ALU load takes 12000 ticks while an FP_MOV load
# takes 500.
_DELAY_TABLE3 = {
    'COMP|17-32|INT_ALU': 1000,
    'COMP|17-32|INT_MUL': 4500,
    'COMP|1|BRANCH': 0,
    'COMP|1|FP_ADD': 500,
    'COMP|1|FP_MOV': 500,
    'COMP|1|INT_ALU': 500,
    'COMP|1|INT_DIV': 500,
    'COMP|1|INT_MUL': 12000,
    'COMP|1|MOV': 500,
    'COMP|2|BRANCH': 0,
    'COMP|2|FP_MOV': 1000,
    'COMP|2|FP_MUL': 500,
    'COMP|2|INT_ALU': 0,
    'COMP|2|INT_DIV': 500,
    'COMP|2|INT_MUL': 500,
    'COMP|2|MOV': 500,
    'COMP|3-4|BRANCH': 0,
    'COMP|3-4|FP_DIV': 1000,
    'COMP|3-4|FP_MOV': 500,
    'COMP|3-4|INT_ALU': 0,
    'COMP|3-4|INT_DIV': 500,
    'COMP|33+|INT_DIV': 8000,
    'COMP|33+|INT_MUL': 12000,
    'COMP|33+|MOV': 12000,
    'COMP|5-8|INT_ALU': 500,
    'COMP|5-8|MOV': 0,
    'COMP|9-16|INT_ALU': 0,
    'COMP|9-16|MOV': 500,
    'LOAD|1|FP_ADD': 3500,
    'LOAD|1|MOV': 12000,
    'LOAD|3-4|FP_ADD': 12000,
    'LOAD|5-8|FP_MOV': 500,
    'LOAD|5-8|INT_ALU': 12000,
    'LOAD|5-8|MOV': 500,
    'LOAD|9-16|MOV': 12000,
    'STORE|1|FP_MOV': 1500,
    'STORE|1|MOV': 1500,
}


def modelled_delay3(typ, dist, opclass_name):
    """comp_delay from (type, producer distance, opcode class).

    Falls back to the (type, distance) median and then to a per-type constant
    when a combination was not observed often enough to measure.
    """
    tname = {LOAD: "LOAD", STORE: "STORE", COMP: "COMP"}[typ]
    v = _DELAY_TABLE3.get(f"{tname}|{_dbucket(dist)}|{opclass_name}")
    if v is not None:
        return v
    # Fall back to the two-feature table, which holds population medians.
    #
    # NOT to the median of the class-specific values at this (type, distance):
    # classes seen fewer than 300 times were dropped when the table was built,
    # so the survivors skew toward the slow ones.  That fallback returned
    # 12000 for LOAD at distance 1 where the population median is 500, and
    # over-serialised replay by +228%.
    return modelled_delay(typ, dist)


# ─── Self-consistent delays: a width-limited in-order scheduler ─────────────
# Copying gem5's comp_delay values onto a Pin-derived graph does not work, even
# when the graph matches structurally.  gem5 defines comp_delay as the gap
# between a producer completing and the consumer *issuing*, taken as the
# minimum over that node's edges -- so the number is only meaningful against
# gem5's own edge set.  Measured: delay distributions matching gem5 to within
# 7% still over-serialised replay by 4x, and adding edges to match gem5's
# rob_dep count without their matching delays made it 5x worse.
#
# So derive the delays from the graph we actually have, using gem5's own
# definition.  Walk the records in order, give each one an issue tick that
# respects both its dependencies and a per-cycle issue width, and emit
#     comp_delay = issue_tick - (latest producer completion)
# which is exactly gem5's quantity.  The width is what injects the queueing
# that a per-type constant cannot express: the O3 reference sustains ~2.95
# ops/cycle (20.0M ops in 6.78M cycles), and that rate is the one parameter.

class _Sched:
    """Issue records in order, limited by both dependencies and issue rate.

    The issue rate is kept as ticks-per-op rather than integer slots per
    cycle, so a fractional width like 2.95 ops/cycle is exact.
    """

    def __init__(self, width, cycle_ticks, lat, ring=8192):
        self.per_op = cycle_ticks / float(width) if width > 0 else cycle_ticks
        self.lat = lat                 # record type -> completion latency
        self.ring = ring
        self.done = [0] * ring         # seq % ring -> completion tick
        self.free = 0.0                # next free issue tick

    def issue(self, seq, typ, deps):
        ready = 0
        have_dep = False
        for d in deps:
            c = self.done[d % self.ring]
            have_dep = True
            if c > ready:
                ready = c
        issue_tick = self.free if self.free > ready else ready
        self.free = issue_tick + self.per_op
        self.done[seq % self.ring] = int(issue_tick) + self.lat.get(typ, 500)
        if not have_dep:
            # No dependency to measure against, so there is no delay to
            # report.  Subtracting a zero "ready" would hand back the
            # absolute issue tick -- the same mistake ElasticTrace's own
            # firstWin fallback makes, and here it put up to 3.4e9 ticks on
            # 40% of records and inflated the trace's total delay to 8e15.
            return 0
        d = int(issue_tick) - ready
        return d if d > 0 else 0


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
    ap.add_argument("--delay-model",
                    choices=("flat", "table", "table3", "uop", "sched"),
                    default="sched",
                    help="sched (default) = derive comp_delay from this "
                         "trace's own dependency graph with a width-limited "
                         "in-order scheduler, using gem5's definition "
                         "(issue tick minus producer completion). Self "
                         "consistent, unlike a table copied from gem5. "
                         "uop = like table3, but aware that a "
                         "cracked instruction is a chain of micro-ops: gem5 "
                         "charges the intermediate ones zero and bills only "
                         "the last, e.g. jne -> cd=0, cd=0, cd=500. Needs "
                         "-crack 1 traces. "
                         "table3 = per-(type, producer-distance, "
                         "opcode class); needs the opcode field the pintool "
                         "emits plus its <prefix>.opcodes.txt legend. "
                         "table = per-(type, producer-distance) "
                         "medians measured from gem5's own elastic traces. "
                         "flat = one constant per record type, the original "
                         "behaviour, which is ~24x too small for loads and "
                         "under-serialises replay by 50-68%%.")
    ap.add_argument("--issue-width", type=float, default=2.95,
                    help="ops per cycle the scheduler may issue "
                         "(--delay-model sched). The O3 reference sustains "
                         "~2.95 on these kernels.")
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
    use_sched = (args.delay_model == "sched")
    use_table = (args.delay_model == "table")
    use_uop = (args.delay_model == "uop")
    use_table3 = (args.delay_model == "table3") or use_uop
    # The pintool writes <prefix>.opcodes.txt beside the per-thread traces;
    # opcode ids are Pin-build specific so the legend must travel with them.
    opcode_class = {}
    if use_table3:
        legends = glob.glob(os.path.join(args.indir, "*.opcodes.txt"))
        if not legends:
            print("[conv] no *.opcodes.txt legend found; falling back to "
                  "--delay-model table (no opcode information)")
            use_table3, use_table = False, True
        else:
            for line in open(legends[0]):
                parts = line.split()
                if len(parts) == 2:
                    opcode_class[int(parts[0])] = opclass(parts[1])
            print(f"[conv] opcode legend: {len(opcode_class)} entries "
                  f"from {os.path.basename(legends[0])}")

    # .txt.gz too: a whole multithreaded PARSEC run is ~17 GB of text, so it
    # is captured through gzip (etrace writes into a named pipe).
    files = sorted(glob.glob(os.path.join(args.indir, "*.tid*.txt")) +
                   glob.glob(os.path.join(args.indir, "*.tid*.txt.gz")),
                   key=lambda p: int(re.search(r"tid(\d+)", p).group(1)))
    if not files:
        sys.exit(f"no *.tidN.txt in {args.indir}")
    # Spawn/join events name threads by Pin thread number and the replay
    # names CPUs by position, so the two must coincide.
    tids = [int(re.search(r"tid(\d+)", f).group(1)) for f in files]
    if tids != list(range(len(files))):
        print(f"[conv] WARNING: thread files {tids} are not 0..{len(files)-1};"
              " spawn/join targets will not match cpu numbers")

    for cpu, path in enumerate(files):
        # One scheduler per thread; latencies are the completion times the
        # model assumes, not what the replay will measure (the replay's own
        # memory system supplies the real load latency).
        sched = _Sched(args.issue_width, 500,
                       {LOAD: 2000, STORE: 500, COMP: 500}) if use_sched else None
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

        n = nsync = nbr = 0
        # A branch's record waits for the next instruction's, whose PC
        # tells whether it was taken: (record, pc, length).
        held = None
        last_fetch_line = None
        tick = 0
        with (gzip.open(path, "rt") if path.endswith(".gz")
              else open(path)) as src:
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
                    # Spawn (3) and join (4) carry a fifth field: the
                    # child's trace number, which is what tid holds for
                    # them. Every other type keeps the thread's own.
                    ev.tid = int(p[4]) if len(p) > 4 else cpu
                    sstream.write(ev)
                    nsync += 1
                    continue
                if p[0] != "S":
                    continue
                seq = int(p[1]); pc = int(p[2]); typ = int(p[3])
                if held is not None:            # always a new instruction
                    hrec, hpc, hlen = held
                    if pc != hpc + hlen:
                        hrec.flags |= 1 << 11           # taken
                    dstream.write(hrec)
                    held = None
                ea = int(p[4]); size = int(p[5])
                i = 6
                nreg = int(p[i]); i += 1
                regdeps = [int(x) for x in p[i:i + nreg]]; i += nreg
                nord = int(p[i]); i += 1
                orddeps = [int(x) for x in p[i:i + nord]]

                rec = dep_pb2.InstDepRecord()
                rec.seq_num = seq
                rec.type = typ
                # Producer distance must be measured against the dependency
                # the REPLAY will wait on, i.e. after the window filter below
                # drops anything older than depWindowSize.  Using the raw list
                # gives a huge distance for a dropped dependency, which lands
                # in the `33+` bucket and charged 12000 ticks to records that
                # the replay sees as dependency-free -- 83% of all COMP delay
                # in one measurement.
                _lo = seq - args.window
                _all = [d for d in regdeps + orddeps if d > _lo]
                _dist = (seq - max(_all)) if _all else 0
                if use_sched:
                    rec.comp_delay = sched.issue(
                        seq, typ, [d for d in regdeps + orddeps if d > seq - args.window])
                elif use_table3:
                    # trailing fields appended by the pintool, after the
                    # dependency lists: opcode id, then the micro-op flag
                    _op = int(p[i + nord]) if len(p) > i + nord else -1
                    _last = (int(p[i + nord + 1])
                             if len(p) > i + nord + 1 else 1)
                    if use_uop and not _last and typ == COMP:
                        # Intermediate COMP micro-op of a cracked instruction.
                        # gem5 gives these zero: `jne` is cd=0, cd=0, cd=500,
                        # so billing each one the operation delay triples the
                        # cost of every branch.
                        rec.comp_delay = 0
                    else:
                        rec.comp_delay = modelled_delay3(
                            typ, _dist, opcode_class.get(_op, "OTHER"))
                elif use_table:
                    rec.comp_delay = modelled_delay(typ, _dist)
                else:
                    rec.comp_delay = delay_of[typ]
                rec.pc = remap(pc)
                if typ != COMP:
                    rec.p_addr = remap(ea)
                    rec.size = size
                    rec.flags = 0
                    # etrace puts 1 in a load's op-class field for the load
                    # of a read-modify-write (gem5 ldst/ldstl); bit 31 of
                    # flags carries it (IntervalCPU sends READ_MODIFY_WRITE).
                    # 2 = locked RMW (ldstl), also non-speculative in O3:
                    # bit 30.
                    if typ == LOAD and len(p) > i + nord + 2 and \
                            p[i + nord + 2] in ("1", "2"):
                        rec.flags = 1 << 31
                        if p[i + nord + 2] == "2":
                            rec.flags |= 1 << 30
                else:
                    # etrace -uoptable appends the micro-op's op class after
                    # opcode and last-flag. A COMP has no request flags, so
                    # it rides in `flags` (IntervalCPU reads it there;
                    # TraceCPU never looks at a COMP's flags). 0 = IntAlu.
                    _ocls = (int(p[i + nord + 2])
                             if len(p) > i + nord + 2 else 0)
                    if _ocls:
                        rec.flags = _ocls
                    # etrace then appends branch kind and instruction
                    # length; on the instruction's last micro-op they ride
                    # above the op class: bits 8-10 kind (1 cond, 2 jmp,
                    # 3 call, 4 indirect call, 5 return, 6 indirect jmp),
                    # bit 11 taken, bits 12-15 length. IntervalCPU's branch
                    # predictor reads them.
                    _kind = (int(p[i + nord + 3])
                             if len(p) > i + nord + 3 else 0)
                    _ilen = (int(p[i + nord + 4])
                             if len(p) > i + nord + 4 else 0)
                    _islast = (int(p[i + nord + 1])
                               if len(p) > i + nord + 1 else 1)
                    # Length on every COMP (fetch needs the line holding
                    # an instruction's last byte); kind on a branch's last.
                    if 0 < _ilen < 16:
                        rec.flags |= _ilen << 12
                    if _kind and _islast and 0 < _ilen < 16:
                        rec.flags |= _kind << 8
                # Drop dependencies older than the window, as ElasticTrace does.
                lo = seq - args.window
                for d in regdeps:
                    if d > lo:
                        rec.reg_dep.append(d)
                for d in orddeps:
                    if d > lo:
                        rec.rob_dep.append(d)
                # NOTE: do *not* chain a dependency-free record to its
                # immediate predecessor to imitate ElasticTrace's
                # updateIssueOrderDep.  Measured: it serialises the whole
                # stream (every record then issues one comp_delay after the
                # last, ~20M x 500 ticks) and took replay from -20.7% to
                # +544%.  gem5's version walks back to a record that has
                # *already completed* and derives that node's comp_delay from
                # that specific edge, so the edge costs nothing extra.  An
                # edge added without its matching delay is pure latency.
                if typ == COMP and (rec.flags >> 8) & 7:
                    held = (rec, pc, (rec.flags >> 12) & 15)
                    nbr += 1
                else:
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

        if held is not None:
            dstream.write(held[0])
        istream.close(); dstream.close(); sstream.close()
        print(f"[conv] cpu{cpu}: {n} records, {nsync} sync events, "
              f"{nbr} branches")

    top = args.phys_base + (len(frames) << pgshift)
    print(f"[conv] remapped {len(frames)} pages -> footprint {top/2**20:.1f} MiB")
    print(f"[conv] comp_delay SYNTHETIC: comp={args.comp_delay} "
          f"load={args.load_delay} store={args.store_delay} ticks")


if __name__ == "__main__":
    main()
