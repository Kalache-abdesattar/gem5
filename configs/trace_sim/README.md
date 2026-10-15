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
| 5. Replication | **working** | replay an N-core trace onto M > N cores for weak-scaling studies |
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

## CHI replay -- working, and a gem5 bug found on the way

`replay_chi.py --hierarchy gem5-chi-l2` replays elastic traces through a
Ruby/CHI hierarchy, with or without the arbiter. Verified on a 2-core trace
with **shared** addresses:

```
2-core shared trace on CHI+L2, nosync : done @ tick 38785000
2-core shared trace on CHI+L2, sync   : done @ tick 38785000
```

`trace_board.py` supplies the small `AbstractCore`/board surface the stdlib
hierarchies call (`TraceCPU` is a `ClockedObject`, so it has no MMU, interrupt
controller or walker), and wires a real CPU when one is present so
`chi_smoke.py` is a meaningful control. Interrupt wiring follows
`base_cpu_core.py`: `interrupt_requestor` -> **both** `pio` and
`int_responder`; `interrupt_responce` -> `int_requestor`.

### gem5 bug: PrivateL1CacheHierarchy breaks under concurrent write sharing

`chi_memtest_repro.py` reproduces it with **no trace and none of this code in
the request path** -- just gem5's own `MemTest` coherence stressor on gem5's
own stdlib CHI hierarchy, two agents sharing an address space:

| hierarchy | `enable_DMT` | result |
|---|---|---|
| `PrivateL1CacheHierarchy` | True (stock) | `CHI-cache-actions.sm:1616` |
| `PrivateL1CacheHierarchy` | False | `CHI-cache-actions.sm:2463` |
| `PrivateL1SharedL2CacheHierarchy` | True | **completes** |
| `PrivateL1SharedL2CacheHierarchy` | False | **completes** |

Run it: `gem5.opt configs/trace_sim/chi_memtest_repro.py`, with `HIER=l1|l2`
and `DMT=0|1`.

The two asserts:

1. `:1616` -- `assert(use_DMT == false || reqType is Atomic)` at
   `Send_ReadNoSnp`.
2. `:2463` -- a second `CompData_UC` arriving at the HN-F for a transaction
   whose TBE already holds valid **and** dirty data. The `ProtocolTrace` shows
   two `CompData_UC` for one line in one transaction, which is a protocol
   duplicate, not a malformed request.

So the L1-only CHI configuration -- where the directory is the home node with
no cache behind it -- cannot handle concurrent write sharing. Adding a shared
L2 avoids it entirely. **Use `--hierarchy gem5-chi-l2`.**

### How this was narrowed

Each row changes exactly one variable:

| client | cores | addresses | hierarchy | result |
|---|---|---|---|---|
| TimingSimpleCPU | 1, 2 | - | L1-only | OK |
| TraceCPU | 1 | - | L1-only | OK |
| TraceCPU | 2 | shared | L1-only | assert |
| TraceCPU | 2 | disjoint (`--private-remap`) | L1-only | OK |
| PyTrafficGen | 3 | shared | L1-only | same assert |
| **gem5 MemTest** | 2 | shared | L1-only | **same assert** |
| gem5 MemTest | 2 | shared | L1+L2 | OK |
| TraceCPU | 2 | shared | L1+L2 | **OK** |

The MemTest row is the one that settles it: the traces were never implicated.

### Separately: config/chi/l3_cache_hierarchy.py does not work in SE mode

A stock stdlib `SimpleBoard` + `SimpleProcessor` running `hello` on it panics
at `CHI-cache-ports.sm:117` (`assert(bitMask.count() > 0)`). Every existing
consumer of that hierarchy is FS mode, so the SE path looks unexercised. Select
it with `--hierarchy repo-l3` if you want to work on it.


## Validating the Pin path (and a correction to the headline accuracy)

`st_kernel` (single-threaded, 512 KiB working set, deterministic) captured three
ways over the **same 3,000,000 instructions** and replayed through an identical
classic hierarchy:

| | simTicks | L2 acc | L2 miss | L2 miss-rate |
|---|---|---|---|---|
| O3 reference | 1,519,818,500 | 342,857 | 9,714 | 2.833% |
| gem5-trace replay | 1,873,949,000 | 342,827 | 9,703 | 2.830% |
| Pin-trace replay | 957,333,000 | 336,834 | 9,789 | 2.906% |

vs the reference:

| | L2 acc | L2 miss | miss-rate | simTicks |
|---|---|---|---|---|
| gem5-trace | -0.01% | -0.11% | **-0.10%** | **+23.3%** |
| Pin-trace | -1.76% | +0.77% | **+2.57%** | **-37.0%** |

`etrace` gained `-maxinsts` for this: `-max` bounds *records*, and a memory
instruction emits one record per operand, so bounding by records compared
different amounts of program (2.23 M instructions against 3.0 M). The
instruction-matched numbers above are within 0.01% of the record-matched ones,
so the confound was not the cause -- but the knob is needed for any honest
comparison.

### Correction: the "+0.1%" figure does not generalise

The +0.1% simTicks accuracy reported earlier in this file was measured on
`hello` -- a few thousand instructions, dominated by startup. On a memory-bound
kernel the **gem5-captured** elastic trace replays **+23.3%** off the O3
reference. Elastic Traces' accuracy is workload-dependent; quote it per
workload, not as a property of the method.

### comp_delay does not calibrate, and does not need to

Sweeping the synthetic delay on the Pin trace:

| comp_delay | simTicks | vs reference | L2 miss-rate |
|---|---|---|---|
| 500 | 957,333,000 | -37.0% | 2.906% |
| 900 | 1,002,595,000 | -34.0% | 2.906% |
| 1300 | 1,130,467,000 | -25.6% | 2.906% |
| 1700 | 1,097,919,500 | -27.8% | 2.906% |

Two things to take from this:

* **Timing cannot be dialled in.** The response is non-monotonic (1700 is worse
  than 1300): on a memory-bound workload the critical path is memory latency
  and the dependency graph, not the compute delay. There is no comp_delay that
  recovers the reference.
* **Memory behaviour is invariant to it.** The L2 miss-rate is identical to
  three decimal places across the whole sweep.

### So: what the Pin path is for

**Use it for memory-system studies.** Cache behaviour reproduces within ~2.6%
on miss-rate and is independent of the fabricated timing, so footprint,
miss-rate, and interconnect-load questions are answerable from a trace captured
on real hardware at ~100 MIPS.

**Do not use it for timing or IPC.** simTicks is 25-37% off and cannot be
calibrated. Any performance claim must come from the gem5-captured path -- and
even there, validate per workload rather than trusting the `hello` number.


## Step 5: trace replication N -> M

`replicate_traces.py` builds M core traces from N captured ones: core *i*
replays source *(i mod N)* with its **data** addresses shifted by
*(i // N) x stride*. The stride defaults to the observed data footprint rounded
up to a power of two.

Only data addresses move. The program counter is left alone, because replicas
are threads of one program: they share code, and shifting the PC would invent
instruction footprint that does not exist.

```
python configs/trace_sim/replicate_traces.py <src> <out> --cores 8 --pb2-dir <d>
gem5.opt configs/trace_sim/replay.py --trace-dir <out> --num-cores 8
```

### Why the offset is not optional

The paper replicates **without** address offsetting and says so plainly: all
cores issue requests to the same addresses, "which results in unrealistic data
sharing during replay", and it measures "well above 80% data sharing in most
experiments". `--no-offset` reproduces that for comparison.

One source trace (512 KiB working set) replicated to 4 cores over a 1 MiB
shared L2:

| | simTicks | L2 acc | L2 miss | miss-rate |
|---|---|---|---|---|
| 1 core (baseline) | 957,333,000 | 336,834 | 9,789 | 2.906% |
| 4 cores, offset | 10,485,350,500 | 1,347,336 | 850,239 | **63.105%** |
| 4 cores, `--no-offset` | 2,252,009,000 | 736,233 | 13,847 | **1.881%** |

With offsets, L2 accesses scale 4x as they should (1,347,336 ~ 4 x 336,834) and
the miss-rate explodes to 63%: four disjoint 512 KiB working sets are 2 MiB of
live data against a 1 MiB L2, so it thrashes. That is the real weak-scaling
answer.

Without offsets the four cores collide on the same lines, so the L2 serves them
all from one copy: accesses rise only 2.2x and the **miss-rate falls to 1.881%,
below the single-core baseline**. A scaling study run that way would conclude
the L2 absorbs four cores comfortably, when in fact it thrashes -- the error
inverts the conclusion rather than merely blurring it.

Use `--no-offset` only to reproduce the paper; never to answer a question.


## Real PARSEC, unmodified

`etrace -autosync 1` derives the synchronisation trace from the **pthread calls
themselves**, so an unmodified benchmark binary yields everything the arbiter
needs. No source changes, no extra m5 ops, no PARSEC hooks beyond the ROI
markers it already has.

| hooked symbol | mapped to |
|---|---|
| `pthread_mutex_lock` | critical begin |
| `pthread_mutex_trylock` | critical begin, **only when it returns 0** |
| `pthread_mutex_unlock` | critical end |
| `pthread_barrier_wait` | barrier, bracketed `IPOINT_BEFORE` and `IPOINT_AFTER` |

That is the faithful mapping: a mutex region *is* a critical section, a barrier
wait *is* a barrier.

### trylock has to be hooked on its return value

Hooking `pthread_mutex_trylock` at `IPOINT_BEFORE` like `pthread_mutex_lock`
records acquisitions that never happened, because trylock acquires only when it
returns 0. It shows up immediately as more ends than beginnings. On
streamcluster (which the sharing study found to be trylock-dominated: 780k
trylock against 91k lock):

```
before: 45,744 critical begin   68,560 critical end
after:  57,143 critical begin   68,542 critical end
```

### Known residual imbalance

~11k more ends than beginnings remain. Two causes, both inherent:

* `pthread_cond_wait` releases and reacquires its mutex internally, without
  going through the exported `pthread_mutex_lock`/`unlock` symbols Pin hooks.
* ROI gating clips regions: a lock taken before `__parsec_roi_begin` and
  released inside emits only its end.

The arbiter tolerates this, but only after a fix that real PARSEC forced.

`releaseLock()` already ignored a release from a CPU that does not hold the
lock. The missing case was the other direction: a **second acquire by the
current holder**, which the lost releases guarantee will happen. The arbiter
queued it -- parking the CPU behind itself, which deadlocks. The 5-core
streamcluster `--sync` replay hung for the full 30-minute cap on exactly this.

`acquire()` now grants immediately when the caller already holds the lock, and
deliberately keeps **no nesting depth**: an approximate trace can lose the
matching release too, and depth counting would then leave the lock held
forever. The next release frees it.

It is not a rare path. In a 300k-instruction window of streamcluster:

```
critical entered  : 12
critical re-enter :  6     <- each one was a deadlock before the fix
critical queued   :  3
critical handed   :  3
retires           :  5
```

The critical-section model remains approximate; treat lock *counts* from this
path as a lower bound.

### Verified

streamcluster, simsmall, `-n 2`, ROI-gated, 2M instructions per thread:

```
cpu0: 2,000,027 records,      8 sync events
cpu1: 2,000,008 records, 59,652 sync events
cpu2: 2,000,018 records, 66,003 sync events
cpu3:    11,420 records,     11 sync events
cpu4:    11,305 records,     11 sync events
footprint after remap: 2.1 MiB

5-core replay: done @ tick 807,907,500
```

And with the arbiter enabled, on a 300k-instruction capture of the same
benchmark:

```
5-core PARSEC nosync : done @ tick 120,677,000
5-core PARSEC sync   : done @ tick 121,543,500
```

Note the thread structure matches what the syncgraph tool measured
independently: two heavy workers plus a main thread and two light helpers, and
no barriers in the ROI window -- streamcluster synchronises through
trylock/mutex, not `pthread_barrier_wait`.
