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
* (done -- see "Sync markers in the Pin path" below.)


## Filtering: capture unfiltered, let gem5 model the L1

The first version of `inject.py` put an L1 in front of every injector while
feeding it an `l1filter` trace -- which is *already* the L1 miss stream. That
double-filters: 50,000 injected packets produced only 35,238 L2 accesses,
because a second L1 absorbed ~30% of traffic that had already survived one.

Removing the L1 turned out to be the wrong fix. `BaseTrafficGen` connected
directly to a crossbar trips
`assert(retryPktTick != 0)` in `retryReq()` -- a gem5 bug in the retry path
(`retryPkt` non-null with `retryPktTick` already zeroed) that an intervening
L1 had been masking. It reproduces with and without `--elastic`, with L2 and
without, and with `max_outstanding_reqs = 0`.

The correct arrangement, which also avoids that bug:

```
pin -t l1filter.so -nofilter 1 -roi 1 -o unfiltered.trace -- <binary>
python configs/trace_sim/l1filter_to_packets.py unfiltered.trace out/ --pb2-dir <d>
gem5.opt configs/trace_sim/inject.py --trace-dir out/ --elastic --l1d
```

`-nofilter 1` emits every line reference rather than only misses, so gem5's own
L1 decides what reaches the fabric. That is strictly better for the intended
studies: with a pre-filtered trace the L1 is frozen at whatever Pin modelled,
so L1 sizing cannot be swept at all.

Cost: traces are much larger. blackscholes simsmall is 153M line references
against ~207k L1-filtered records, so bound captures with `-max`.

Verified, 400k unfiltered references, 3 threads:

```
system.gen1.numPackets              200697
system.gen1.l1d.overallAccesses     200697
system.gen1.l1d.overallMisses         1845   <- gem5's L1 doing the filtering
system.gen*.numSuppressed                0
```

**Rule:** `--l1d` only with `-nofilter 1`. Default `l1filter` traces go
straight into the hierarchy below the L1.


## Sync markers in the Pin path

The Pin route now feeds the arbiter, so the whole ElasticSimMATE loop can run
from a hardware capture.

**Markers cannot be gem5 m5 ops.** `m5_work_begin` emits opcode `0F 04`, which
is an *illegal instruction* on real x86 -- a gem5-marked binary SIGILLs the
moment Pin runs it natively. The Pin path instead marks regions with ordinary
no-op functions hooked by name:

```c
void __attribute__((noinline)) __trace_sync_begin(unsigned long type) {}
void __attribute__((noinline)) __trace_sync_end(unsigned long type) {}
```

`etrace` hooks them with `RTN_FindByName` and reads `type` via
`IARG_FUNCARG_ENTRYPOINT_VALUE` (0 parallel, 1 critical, 2 barrier). Override
the symbol names with `-sync_begin` / `-sync_end`.

**Units matter.** The marker records the tool's running *record count*, not an
instruction count, because that is what `TraceCPU` counts in at replay -- so
the arbiter compares like with like.

### Two deadlocks this exposed, both now fixed

1. **A CPU with an empty sync trace used to register anyway.** `syncEnabled`
   was false, so it never reached a barrier and never retired, and the arbiter
   counted it as active forever. This is the *normal* case for a main thread
   that only spawns and joins. `initSyncTrace()` now warns and does not
   register.
2. **`replay.py` passed `num_cpus=args.num_cores`.** With one CPU legitimately
   not registering, the barrier waited forever for an arrival that could never
   come. The arbiter now infers its participant count from who registers
   (`num_cpus=0`).

### Verified

`pin_sync_demo`, 2 worker threads + main, 18 markers per worker:

```
cpu0: sync trace contains no events; replays unsynchronised, not counted
cpu1: loaded 18 synchronisation points (arbiter id 0)
cpu2: loaded 18 synchronisation points (arbiter id 1)

24051500: SyncArbiter: cpu 0 arrived (1/2)
24052000: SyncArbiter: cpu 1 arrived (2/2)
24052000: SyncArbiter: releasing barrier 1
24052000: SyncArbiter: cpu 1 entered critical
```

2 barriers released, 1 critical grant, 3 stalls, 2 clean retires, no deadlock.

## Persistent locations

The protobuf tooling and traces live outside the session scratchpad, which is
wiped between sessions:

* `~/fedora/pintools/pbenv/venv` -- protobuf runtime (cannot be installed
  system-wide: pacman would pull libprotobuf 34 and break the gem5 build)
* `~/fedora/pintools/pbenv/pb2` -- generated `*_pb2.py`
* `~/fedora/pintools/workloads` -- marker-annotated demo workloads
* `~/fedora/pintools/traces` -- captured and converted traces

## CHI replay -- WORK IN PROGRESS, does not run yet

`replay_chi.py` + `trace_board.py` attach TraceCPUs to this repo's Ruby/CHI
hierarchy (`config/chi/l3_cache_hierarchy.py`) instead of the classic L1/L2.

### The shim

`TraceCPU` is a `ClockedObject`, not a `BaseCPU`, so it is not an
`AbstractCore`: no MMU, no interrupt controller, no page-table walker. The
stdlib hierarchies need only a small surface, which `trace_board.py` supplies:

* core: `connect_icache`, `connect_dcache`, `connect_walker_ports` (no-op -- a
  replayed trace carries physical addresses and never walks),
  `connect_interrupt` (no-op), `requires_send_evicts`, `is_kvm_core`,
  `get_isa`, `set_switched_out`, `get_mmu`
* board: `get_processor`, `get_cache_line_size`, `get_clock_domain`,
  `get_mem_ports`, `has_io_bus`/`get_io_bus`, `has_dma_ports`/`get_dma_ports`,
  `connect_system_port`

The L1/L2/L3 dicts are lifted verbatim from `config/bench/x86-parsec.py` so a
trace replay and a normal benchmark run model the same caches; diverging there
would make the two incomparable.

### Status: builds and starts, then asserts

The hierarchy constructs, `m5.instantiate()` succeeds and the CHI protocol
runs. It then fails with:

```
CHI-cache-ports.sm:117: assert failure for machine
  ...core_clusters.dcache.downstream_destinations.downstream_destinations
```

which is `assert((bitMask.count() <= data_channel_size) && (bitMask.count() > 0))`
on the L3's data-in port -- a data message with a zero-length bitmask.

What has been ruled out:

* **Not the Pin traces.** It reproduces identically with a gem5-captured
  trace (`m5out/capture`), so it is the hierarchy setup, not the trace.
* **Not request size.** `data_channel_size` is 64 everywhere in `config/chi`;
  the traces' largest access is 32 B, and none is zero-length.
* **Not page-straddling from the converter's remapping.** Zero accesses in the
  trace cross a 4 KiB boundary (one crosses a cache line, which TraceCPU
  splits normally).
* **Not `requires_send_evicts`.** Fails the same with it true and false.

Next things to try: whether `RubySequencer`/`num_of_sequencers` accounting is
right when the "cores" are not real CPUs; whether the L3 slice address ranges
cover `TraceBoard.mem_ranges`; and running the same hierarchy with a
`TimingSimpleCPU` through this board to separate the shim from `TraceCPU`.
