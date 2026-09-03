# trace_sim — ElasticSimMATE-style trace-driven multicore simulation

Goal: reproduce the approach of Nocua et al., *ElasticSimMATE: a Fast and
Accurate gem5 Trace-Driven Simulator for Multicore Systems* (ReCoSoC 2017) —
Elastic Traces extended with inter-core synchronisation so that multicore
replay is meaningful.

Built incrementally. Each step is independently runnable.

## Why stock gem5 isn't enough

gem5 ships Elastic Traces (`ElasticTrace` probe + `TraceCPU`), but:

* `configs/common/CpuConfig.py:config_etrace()` assigns **the same trace
  filename to every CPU**, with an upstream comment saying it "must be
  revisited when creating elastic traces for multi processor systems".
* `TraceCPU` has no notion of another TraceCPU. Replaying N traces gives N
  independent streams that never wait for each other, so every barrier,
  lock and condvar in the original program silently disappears.

ElasticSimMATE's contribution is a third trace — synchronisation events — plus
an arbiter that stalls TraceCPUs at the recorded sync points.

## Steps

| Step | State | What it adds |
|------|-------|--------------|
| 1. Single-core capture + replay | **working, validated** | `capture.py`, `replay.py`; validates the ET pipeline end to end and gives a replay-vs-O3 accuracy baseline |
| 2. Per-core traces, N-core replay | **working** | one trace pair per core; N TraceCPUs sharing a cache hierarchy. Still *no* sync — the known-wrong baseline that motivates step 3 |
| 3. Synchronisation trace | **working** | record sync events (barrier / critical / parallel-region) with instruction + dependency counts per region |
| 4. Arbiter | **working** | a SimObject that locks/unlocks TraceCPUs at region boundaries, per Fig. 4b of the paper |
| 5. Replication | planned | replay an N-core trace onto M > N cores for weak-scaling studies |
| 6. CHI / Ruby back end | planned | swap the classic hierarchy for the CHI setup this repo already uses |

## Prerequisite: protobuf

Elastic Traces needs protobuf. This tree previously capped `protoc` at 3.99.99
in `src/proto/SConsopts`, which disabled `TraceCPU` and `ElasticTrace`
entirely (`HAVE_PROTOBUF 0`). The cap is lifted on this branch: protoc 33.1 was
verified to generate and compile gem5's `.proto` files under C++17, and
`src/proto/protobuf.cc` builds against libprotobuf 33.1 unchanged.

Verify with:

```
build/X86_CHI/gem5.opt --quiet -c \
  'import m5.objects; print(hasattr(m5.objects,"TraceCPU"))'
```

## Usage

Capture (reference run, O3 cores):

```
build/X86_CHI/gem5.opt --outdir=m5out/capture \
    configs/trace_sim/capture.py \
    --cmd tests/test-progs/hello/bin/x86/linux/hello \
    --num-cores 1
```

Replay (TraceCPU, same memory system):

```
build/X86_CHI/gem5.opt --outdir=m5out/replay \
    configs/trace_sim/replay.py \
    --trace-dir m5out/capture --num-cores 1
```

Compare `simTicks` / `system.l2.overallMisses` between the two directories.
Elastic Traces claims <10% error and 6–8x speedup versus O3 on a single core.

## Notes on fidelity

Capture deliberately runs with an oversized ROB/LQ/SQ (512/128/128) so the
reference run does not stall on resource limits — such stalls would be baked
into the trace as compute delay. The real sizes are modelled at replay by
TraceCPU (`--rob/--lq/--sq`).


## Validated results (step 1)

x86, `tests/test-progs/hello`, classic L1+L2, ROB/LQ/SQ matched at 192/32/32
between the reference O3 run and the TraceCPU replay:

| | simTicks | hostSeconds | L2 misses |
|---|---|---|---|
| Reference O3 (`--no-etrace`) | 16,991,500 | 0.06 | 410 |
| TraceCPU replay | 17,009,000 | 0.01 | 406 |

**+0.1% error, 6.0x speedup** — in line with the Elastic Traces paper's <10%
error and 6-8x speedup.

Getting a meaningful error number requires three runs, not two:

1. `--no-etrace` -> reference, stock O3 sizing (192/32/32)
2. default -> capture, ROB/LQ/SQ deliberately oversized to 512/128/128 so the
   reference run does not stall on resources
3. `replay.py --rob 192 --lq 32 --sq 32` -> replay, sized to match run 1

Comparing the replay against the *capture* run instead of the reference gives a
meaningless -34% because the capture CPU is 512-wide.

Step 2 (2 cores, `--maxinsts 4000`): four distinct per-core traces, both
TraceCPUs replay through a shared L2, 12,531,000 ticks / 577 L2 misses. No
synchronisation yet -- the streams are independent, which is exactly the
limitation step 3/4 exist to remove.

## Fixes required to gem5 itself

`ElasticTrace` had been compiled out of this tree for long enough to bit-rot
against the probe API. On this branch:

* `src/proto/SConsopts` -- lifted the protoc <= 3.99.99 cap (see above).
* `src/cpu/o3/probe/elastic_trace.{cc,hh}` -- the O3 probe points are
  `ProbePointArg<DynInstPtr>`, but ElasticTrace connected listeners as
  `DynInstConstPtr`, so every registration hit
  "Wrong type of listener" at startup. Switched to `DynInstPtr`.
* `src/cpu/o3/probe/elastic_trace.hh` -- `SeqNumRegPair` was
  `pair<InstSeqNum, RegIndex>`; the `SquashInRename` probe point now carries
  `pair<InstSeqNum, PhysRegIdPtr>`. Matched it, and key `physRegDepMap` off
  `->flatIndex()` as the insert path already did.
* `elastic_trace.cc:flushTraces()` -- made idempotent (it deleted the stream
  objects without nulling them). Defensive; it was not the cause of the crash
  below.

## Known limitation: multi-process SE teardown

Running N cores each with its *own* SE process segfaults at exit inside stock
gem5, in `o3::Commit::clearStates` via `CPU::exitThreads` ->
`haltContext` -> `removeThread`. It reproduces with `--no-etrace`, i.e. with no
tracing involved at all, so it is not an elastic-trace bug. Because the crash is
inside `doSimLoop`, `simulate()` never returns and the exit callbacks that flush
the protobuf streams never run, leaving truncated traces.

Workaround: bound the capture with `--maxinsts` so the run ends on
"max instruction count" before the processes exit. This is what you want for
trace capture anyway -- you trace a region of interest, not a whole program.
This goes away in FS mode, which is what the paper uses and what step 3 needs.


## Step 3: the synchronisation trace

ElasticSimMATE's third trace file, emitted per core alongside the instruction
and dependency traces as `cpuN.sync.trace.gz`.

**Marking regions.** Rather than adding a new pseudo-instruction, this reuses
gem5's existing `workbegin`/`workend` m5 ops: already decoded on every ISA and
already reachable from a workload. The low byte of `workid` carries the class:

| workid & 0xff | meaning |
|---|---|
| 0 | parallel region |
| 1 | critical section |
| 2 | barrier |

**Record contents** (`src/proto/sync_event.proto`), matching the paper's list:
tick, PC, thread id, begin/end edge, event type, instructions committed and
dependency records emitted. The replayer diffs consecutive records to get the
counts *inside* each region - what the step-4 arbiter counts down before
stalling a TraceCPU.

**Plumbing added to gem5:**

* `src/cpu/sync_event.hh` - `SyncEventInfo` payload.
* `src/cpu/base.{hh,cc}` - a `SyncEvent` probe point plus a public
  `notifySyncEvent()` accessor (the probe member itself is protected).
* `src/sim/pseudo_inst.cc` - `workbegin`/`workend` notify it. `workend`
  notifies before the `exit_on_work_items` early return, so a region end is
  recorded even in that mode.
* `src/cpu/o3/probe/elastic_trace.{cc,hh}` - opens the third stream, counts
  dependency records, writes one message per marker.

**Decoding:** `configs/trace_sim/decode_sync.py <cpuN.sync.trace.gz> <pb2dir>`
after `protoc --python_out=<pb2dir> -I src/proto src/proto/sync_event.proto`.
The Python protobuf runtime is not installed system-wide here - pacman would
upgrade libprotobuf to 34.x and break both the gem5 build and libphonenumber -
so use a venv.

Verified on a workload doing three rounds of parallel / critical / barrier:

```
        tick type      edge    tid     insts      deps   d_insts   d_deps
    80837000 parallel  begin     0    140151    222795    140151   222795
    84915000 parallel  end       0    158166    252777     18015    29982
    84918000 critical  begin     0    158169    252777         3        0
    85318000 critical  end       0    159984    255826      1815     3049
    85321000 barrier   begin     0    159986    255826         2        0
    85324000 barrier   end       0    159987    255826         1        0
    ...
# 18 sync records
```

Per-region deltas repeat exactly across rounds (18,015 instructions and ~29,985
dependencies per parallel region; 1,815 / ~3,050 per critical). That invariant
is what the arbiter depends on.

## FS mode: blocked on a disk image

Step 3's machinery is mode-independent - the probe fires wherever the m5 op
executes, SE or FS. Running it on real PARSEC needs FS mode, and the x86 disk
this repo used (`/mnt/sim/x86-parsec-1.0.0`, referenced from
`config/bench/x86-parsec.py`) is no longer mounted. The kernel is still present
at `images/kernel/x86-linux-6.8.0-52`.

## Option B: SimMATE-style injection from Pin traces

Skips `TraceCPU` entirely. The trace is the *fabric-visible* traffic (L1 misses
and writebacks) captured on real hardware by the `l1filter` Pin tool, replayed
through a fully simulated cache/interconnect/memory by traffic generators
standing in for cores.

No CPU timing is modelled, and crucially none is *invented*: the elastic-trace
path needs a `comp_delay` per dependency edge, derived from O3 pipeline
timestamps (`executeTick`/`toCommitTick`/`commitTick`), which Pin cannot
produce. This path does not need it.

```
# 1. capture on real hardware (~100 MIPS, not a simulator)
pin -t ~/fedora/pintools/l1filter/obj-intel64/l1filter.so \
    -roi 1 -o fabric.trace -- <binary> <args>

# 2. convert to gem5 packet.proto (needs the protobuf venv)
protoc --python_out=<pb2dir> -I src/proto src/proto/packet.proto
python configs/trace_sim/l1filter_to_packets.py fabric.trace out/ --pb2-dir <pb2dir>

# 3. inject
build/X86_CHI/gem5.opt --outdir=m5out/inject configs/trace_sim/inject.py \
    --trace-dir out/ --elastic
```

### Address remapping is mandatory

Pin emits host **virtual** addresses (observed up to `0x7fff8629fc40`, ~140 TB).
They fall outside any sane gem5 memory range and `BaseTrafficGen` **silently
suppresses** them -- the first run completed "successfully" with
`numPackets 0` and `numSuppressed 50000`. Check `numSuppressed` before trusting
any injection run.

`--remap page` (default) assigns each unique 4 KiB virtual page a compact
physical frame, preserving the page offset. That is what an OS does; intra-page
locality and line offsets survive. The mapping is **shared across all threads**
-- remapping per thread would destroy exactly the sharing that makes the trace
worth replaying. Cache index bits above bit 11 differ from the host, as they do
on any physically-indexed cache.

### Verified

blackscholes, 50k records, 2 threads, page-remapped into a 1.4 MiB footprint:

```
system.gen0.numPackets       327     numSuppressed 0
system.gen1.numPackets     49673     numSuppressed 0
system.l2.overallAccesses  35238
system.l2.overallMisses     1692
```

### Later: upgrading this to ElasticSimMATE

The planned route is to emit the elastic-trace format from Pin with a
**synthesised** `comp_delay` (a constant, or an opcode-latency table). Pin can
supply the dependency graph -- `reg_dep` via last-writer tracking per register,
`rob_dep` via last-store-per-address -- but not the edge weights. That variant
must be labelled as a synthetic in-order timing model, not as Elastic Traces:
the +0.1% accuracy figure above belongs to gem5-captured traces and does not
transfer.

## Option A': ElasticSimMATE from Pin (synthetic comp_delay)

Emits the elastic-trace *pair* from a Pin run, so gem5's `TraceCPU` -- with its
real dependency machinery, ROB/LQ/SQ modelling and I-side -- replays a stream
captured on hardware at ~100 MIPS instead of at gem5-O3 speed.

```
# 1. capture the dependency graph on real hardware
pin -t ~/fedora/pintools/etrace/obj-intel64/etrace.so -o et/run -max 2000000 \
    -- <binary> <args>

# 2. convert, choosing the synthetic compute delay
protoc --python_out=<pb2dir> -I src/proto src/proto/packet.proto
protoc --python_out=<pb2dir> -I src/proto src/proto/inst_dep_record.proto
python configs/trace_sim/etrace_to_elastic.py et/ out/ --pb2-dir <pb2dir> \
       --comp-delay 500

# 3. replay with the unmodified TraceCPU
build/X86_CHI/gem5.opt --outdir=m5out/pinreplay configs/trace_sim/replay.py \
    --trace-dir out/ --num-cores 1
```

### What Pin supplies, and what it cannot

| elastic-trace field | source |
|---|---|
| `seq_num`, `pc`, `type`, `size` | Pin, directly |
| `p_addr` | Pin, page-remapped (host virtual addresses are ~140 TB) |
| `reg_dep` | Pin: last-writer-per-register, using `REG_FullRegName` so partial registers alias correctly |
| `rob_dep` | Pin: last access to the same line (store->load / store->store ordering) |
| I-side `packet.proto` | synthesised: one fetch per instruction cache line touched |
| **`comp_delay`** | **SYNTHESISED. Pin cannot derive it.** |

`comp_delay` in real elastic traces is a difference of O3 pipeline timestamps
(`executeTick` / `toCommitTick` / `commitTick`), taken as the *minimum* across a
node's dependency edges. Pin has no pipeline and no cycles. The converter
assigns a fixed per-type delay instead.

### comp_delay is the CPU model

Sweeping it on a 20k-record trace shows it directly sets the timing:

| `--comp-delay` (ticks) | CPI | simTicks |
|---|---|---|
| 250 | 1.098 | 10,982,250 |
| 500 | 1.099 | 10,994,500 |
| 1000 | 1.650 | 16,495,500 |
| 2000 | 2.762 | 27,615,000 |

Flat from 250 to 500 because below ~1 cycle of compute delay the memory system
is the critical path; above it, the synthetic delay dominates. Useful as a
sanity check, and a reminder of what this is: **a synthetic in-order timing
model driving a real memory system.** The +0.1% accuracy figure earlier in this
file belongs to gem5-captured traces and does not transfer.

### Verified

20k records from `/bin/echo`, replayed through the stock `TraceCPU`:

```
system.cpu.numOps                20000
system.cpu.cpi                   1.099450
system.cpu..dside.maxDependents  50        <- dependency graph is live
system.cpu..dside.numSendFailed  17        <- and back-pressured
system.cpu.icache.overallMisses  94
```


## Step 4: the arbiter

This is the piece that makes multicore replay mean anything. Before it, N
TraceCPUs raced independently and every barrier in the original program was
invisible.

* `src/cpu/trace/sync_arbiter.{hh,cc}` + `SyncArbiter.py` -- a SimObject that
  collects arrivals and releases every participant together.
* `TraceCPU` gains `syncTraceFile` and `arbiter` params, reads the sync trace
  at construction, and counts `numOps` against each recorded boundary.
* Stalling gates `schedDcacheNextEvent()`, the single choke point for D-side
  progress; the deferred tick is remembered and replayed on release.
* A TraceCPU whose trace runs out calls `arbiter->retire()`, otherwise the
  remaining CPUs would wait forever on a core that will never arrive again.
  `retire()` also drops a pending vote and re-evaluates, so retiring the last
  outstanding participant releases the barrier instead of deadlocking.

Run it with `replay.py --sync`. Without the flag the CPUs replay independently,
which is the stock-gem5 behaviour and is kept deliberately as the contrast.

### Verified

2 cores, 13 sync points each, `--debug-flags=TraceCPUData`:

```
192531000: cpu0: Reached sync point 1 at 140171 ops; waiting.
192531000: SyncArbiter: cpu 0 arrived (1/2)
196056500: cpu1: Reached sync point 1 at 140191 ops; waiting.
196056500: SyncArbiter: cpu 1 arrived (2/2)
196056500: SyncArbiter: releasing barrier 1
```

cpu0 waited 3,525,500 ticks for cpu1 -- a barrier genuinely holding a core
back. Totals: 13 barriers released, 26 arrivals, 26 stalls, 26 resumes,
2 retires. Every stall matched by a resume; no deadlock.

simTicks 363,073,000 (no sync) vs 363,961,000 (sync). The gap is small here
only because both cores replay *identical* traces and stay in lockstep, so the
barriers rarely bite. Asymmetric traces or asymmetric cache configurations are
where this starts to matter.

### Region semantics

Each recorded boundary is dispatched on its type rather than all being treated
as barriers:

| type | begin | end |
|---|---|---|
| parallel (0) | no stall -- just a marker | **barrier** (a parallel region carries an implicit barrier at its end) |
| critical (1) | **acquire** the arbiter's lock; stall only if held | **release**, handing it to the next waiter |
| barrier (2) | **barrier** | **barrier** |

Treating a critical section as a barrier would rendezvous every thread at every
lock and systematically over-serialise lock-heavy workloads -- `dedup` (22k
spinlocks) and `fluidanimate` (2.2M mutex ops) would be badly distorted.

`checkSyncPoint()` loops rather than handling one boundary per dcache event,
because the non-blocking cases (parallel begin, uncontended acquire, release)
can fall several-to-a-batch and must not be left pending.

Verified on 2 cores:

```
215034500: SyncArbiter: cpu 1 entered critical
215054000: SyncArbiter: cpu 0 queued on critical (held by 1)
215054000: cpu0: Sync stall (critical (queued)) at point 3, 158192 ops.
216445000: SyncArbiter: cpu 1 handed critical to 0
217709500: SyncArbiter: cpu 0 left critical, now free
235049000: SyncArbiter: cpu 0 entered critical      <- roles swap next round
235137000: SyncArbiter: cpu 1 queued on critical (held by 0)
```

Counts: 6 barriers released, 8 barrier stalls + 4 parallel-end stalls (= 6
barriers across 2 CPUs), 4 critical grants of which 2 were contended. A CPU
that retires while holding the lock hands it on rather than deadlocking the
queue.

### Still missing from the paper

* **Trace replication N -> M** for weak-scaling studies (paper section III-A4b).
* The Pin path (`pintools/etrace`) records no sync events, so it cannot feed
  the arbiter yet -- it would need to hook the same workbegin/workend markers.
