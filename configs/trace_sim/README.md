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


## Simulation speed (measured, same host)

x264, simsmall, `-n 2`, ROI-gated, 2M instructions per worker thread ->
**16 threads, 18,027,621 instructions, 18,176,307 records**.

| stage | throughput | wall |
|---|---|---|
| Pin capture (`etrace`, before optimisation) | 0.33 M insts/s | - |
| Pin capture (`etrace`, optimised) | **0.77 M insts/s** | - |
| Pin capture (`l1filter`, for comparison) | 4.15 M insts/s | - |
| text -> protobuf conversion | 0.11 M records/s | 173 s |
| **gem5 replay, 16 cores** | **0.75 M ops/s** | 24.3 host s |
| gem5 replay, 16 cores, `--sync` | 0.72 M ops/s | 25.3 host s |

Replay results: `done @ tick 1,387,695,000` (nosync) and `1,391,467,000`
(sync).

### Replay speedup over detailed simulation

Per core, on `st_kernel`:

| | throughput | |
|---|---|---|
| gem5 O3 detailed | 0.188 M insts/s | |
| TraceCPU replay | 1.123 M ops/s | **6.0x faster** |

That matches the Elastic Traces paper's claimed 6-8x.

### The capture is now the bottleneck, not the replay

This is the uncomfortable number. `etrace` captures at 0.77 M insts/s against
gem5 O3's 0.188 M insts/s -- only **4x** faster. The Pin path's whole premise
was capturing cheaply on real hardware, and at this rate capturing a trace costs
a meaningful fraction of just simulating the thing.

The 2.3x already won (0.33 -> 0.77) came from two changes on the hot path:

* `lastWriter` was a `std::map<REG, UINT64>`. REG is a small dense enum, so a
  flat `std::vector` indexed by register removes a tree lookup per source and
  per destination operand of every instruction.
* The record was written with a dozen chained `ostream <<` calls. One
  `snprintf` into a staging buffer plus one `write` replaces them.

What is still on the table, in rough order of expected win:

* **Binary records instead of text.** Formatting integers to decimal is the
  remaining hot cost, and the converter re-parses them immediately afterwards.
* `lastAccess` is still a `std::map<ADDRINT, UINT64>`; a small direct-mapped
  hash of recently-touched lines would serve, since order dependencies only
  matter within the dependency window.
* The per-memory-operand `InstDesc` copy at instrumentation time allocates;
  it could be flattened into one descriptor with an operand array.

By contrast `l1filter` runs at 4.15 M insts/s doing cache modelling, which
shows the gap is this tool's bookkeeping rather than Pin itself.

### Determinism

The optimised tool is deterministic: two runs with ASLR disabled
(`setarch -R`) produce byte-identical traces. Comparing a trace against one
captured *before* the optimisation shows differences, but they are an ASLR
artifact, not a regression -- stack addresses land at different offsets within
a cache line, which changes how accesses group into lines and therefore which
order dependencies are recorded. Disable ASLR for any run-to-run comparison.

## Why gem5's own dependency graph over-serialises: comp_delay absorbs memory back-pressure

The gem5-trace replay was consistently slower than the O3 reference it was
captured from, and by wildly different margins per kernel:

| kernel | O3 reference | gem5-trace replay | error |
|--------|-------------:|------------------:|------:|
| l1     |  3.390e9 |  3.462e9 |  +2.1% |
| l2     |  6.854e9 |  6.914e9 |  +0.9% |
| chase  | 94.246e9 | 98.723e9 |  +4.7% |
| seq    | 24.571e9 | 26.958e9 |  +9.7% |
| rand   |148.803e9 |212.762e9 | +43.0% |
| stride | 23.903e9 | 36.197e9 | +51.4% |

Earlier notes blamed replay queue sizing. That explains part of `seq` and
`rand` but it provably does not explain `stride`: replaying `stride` with the
ROB/LQ/SQ raised from 192/32/32 to 512/128/128 moves it from 36.197e9 to
36.053e9, a 0.4% change against a 51% error. The limiter is not a queue.

### What the trace actually contains

`ElasticTrace` defines a node's `comp_delay` as the gap between a producer
completing and the consumer executing (`compDelayRob`,
`compDelayPhysRegDep`), where "executing" means, per `TraceInfo::getExecuteTick`:

* a load -> the tick its request was **sent**,
* a store -> the tick it **committed** (when O3 sends it from the store buffer),
* anything else -> the tick it wrote back.

Nothing in that definition restricts the gap to computation. Any time the
consumer sat in the load queue with its address already computed -- waiting for
a free MSHR, for the cache to unblock, for in-order commit to drain past a
missing load at the ROB head -- lands in `comp_delay`.

Dumping the full `stride` trace (10,516,024 records) shows how much:

```
                 records          sum comp_delay      >=20k ticks
  LOAD           810,326         168,891,271,994      774,977 recs
  STORE        1,322,849           5,877,656,000       50,791 recs
  COMP         8,382,849          25,165,810,488      255,808 recs
  total                          199,934,738,482      96.5% of all
                                                      comp_delay is
                                                      in records >=20k
```

Mean `comp_delay` per load is **208,423 ticks = 208 ns**, and 93.5% of loads
fall in the 100k-500k tick band:

```
  bucket         records   share         sum ticks   share
  100k-200k      327,167   40.4%    42,774,070,000   25.3%
  200k-500k      430,312   53.1%   118,141,453,000   70.0%
```

The longest latency any functional unit in the capture config can have is 24
cycles (`src/cpu/o3/FuncUnitConfig.py`), which at 2GHz is **12,000 ticks**. No
x86 instruction computes for 208 ns. Those values are memory latency wearing a
compute label.

### Why that double-counts

`TraceCPU` already models the memory system. It holds a node until its
dependencies complete, waits `comp_delay`, *then* issues the request and waits
for the real response. So congestion the reference run experienced is charged
twice: once as the baked-in `comp_delay`, once more when the replay's own
memory system regenerates it. The two do not overlap, because `comp_delay` is a
hard pre-issue timer rather than something the memory system can absorb.

This also explains the per-kernel spread exactly. Mean load `comp_delay` tracks
the error monotonically:

| kernel | mean load comp_delay | replay error |
|--------|---------------------:|-------------:|
| chase  |  53,635 ticks | +4.7% |
| seq    |  71,344 ticks | +9.7% |
| stride | 208,423 ticks | +51.4% |

`chase` is a pointer chase: 61.3% of its loads have `comp_delay` under 1000
ticks, because each load's address genuinely depends on the previous load's
data and the dependency edge already carries that latency. There is no
memory-level parallelism, so there is no back-pressure to misattribute. Its
large-delay loads belong to the high-parallelism summation phase, which is not
on the critical path. `stride` is the opposite: high parallelism, a deep load
queue, and therefore a large recorded wait per load.

This is the same thing as the measured symptom reported earlier -- `stride`
replay sustains 2.24 misses in flight against the reference's 3.36. The
`comp_delay` timers space the requests out, so the replay cannot reach the
reference's overlap no matter how large its queues are.

### The oversizing in capture.py does nothing (measured)

`attach_probes()` raises the capture ROB/LQ/SQ to 512/128/128 with the comment
that resource stalls "would be baked into the trace as compute delay". The
intent is right but it does not work, because the back-pressure here is MSHRs
and memory bandwidth, which no amount of queue capacity removes.

Capturing `stride` at the replay's own sizes (`--cap-rob 192 --cap-lq 32
--cap-sq 32`) gives a mean load `comp_delay` of **207,227** ticks against
**208,423** for the oversized capture -- a 0.6% difference. Queue capacity is
simply not what is being waited on. (An earlier draft of this section claimed
the oversizing makes the recorded delay *longer*; it does not, and the
measurement above is why that claim was removed.)

What the oversizing *does* change is the instruction trace, and that matters
more. See "The instruction trace sets a floor" below.

### Fix: clamp comp_delay to the longest real compute latency

`ElasticTrace` gains a `compDelayCap` parameter (`--comp-delay-cap` on
`capture.py`), applied in `writeDepTrace()` at the single point where
`comp_delay` is serialised, so it covers every producing path: `compDelayRob`,
`compDelayPhysRegDep`, and the `firstWin` fallback that otherwise writes an
absolute tick count as a delay. Two stats, `numCompDelayClamped` and
`compDelayTicksClamped`, make the clamping auditable instead of silent.
Default 0 preserves upstream behaviour.

```
--comp-delay-cap 12000      # 24 cycles at 2GHz = longest FU latency
```

Anything above that bound cannot be computation by construction, so clamping it
keeps genuine compute and returns the memory latency to the component that is
supposed to model it.

A more surgical fix would instrument the O3 LSQ to record, per instruction, the
time it spent blocked on memory resources, and subtract exactly that. That
needs new probe plumbing in the LSQ; the cap gets the same effect from one
parameter and is self-auditing via the stats.

### Three bugs the clamp exposed

Clamping `comp_delay` does not change the dependency graph: the capped
`stride` trace has exactly the same 10,516,024 records, the same edges and the
same addresses. Only the delays shrink, from 199.93e9 ticks of recorded
"computation" to 19.88e9. That alone broke the replay in three separate ways,
all of them latent problems that the inflated delays had been hiding.

**1. The clamp must not touch the first-window fallback.** When a node in the
first window has no dependency at all, `writeDepTrace()` assigns its
`comp_delay` an *absolute tick* rather than a delay:

```cpp
if (firstWin && temp_ptr->compDelay == -1) {
    if (temp_ptr->isLoad())       temp_ptr->compDelay = temp_ptr->executeTick;
    else if (temp_ptr->isStore()) temp_ptr->compDelay = temp_ptr->commitTick;
    else                          temp_ptr->compDelay = temp_ptr->toCommitTick;
}
```

`TraceCPU::init()` reads the first such value back through
`ElasticDataGen::init()` and uses it to establish `traceOffset`, the replay's
time origin. Clamping it reports a bogus origin -- the debug log showed
`Execute tick of the first dependency free node 1 is 12000` where the real
value is 81,500. The clamp therefore runs *before* that fallback, so it only
touches genuine delays.

Measured impact on these workloads: **none**. Capturing `stride` twice, once
with the first-window value clamped to 12,000 and once with it left at 81,500,
gives replays that agree to the tick (23,739,466,001 both times). The reason is
that `traceOffset` is `min(first_icache_tick, first_dcache_tick)` and the
instruction trace starts at tick 0, so the offset is 0 either way and the fetch
side gates the start regardless of what the data side's first node says. The
ordering is still wrong to rely on -- a trace whose fetch stream did not begin
at 0 would shift -- but no number in this document depended on it.

**2. TraceCPU goes permanently dormant when its ready list drains (latent
upstream bug, not the cause of the stall below).** `ElasticDataGen::execute()`
schedules its next event only when `readyList` is non-empty, or when
`depFreeQueue` has a node with resources available. If both are empty, no
event is scheduled and the generator can only be woken by
`completeMemAccess()` on an outstanding response. That matters because
`nextRead`, the flag requesting the next window of the trace, is acted on only
at the *top* of `execute()`:

```cpp
if (nextRead) { readNextWindow(); nextRead = false; }
```

So the generator can set "read more next time" and then schedule no next time.
The comment in `completeMemAccess()` states the assumption being relied on:

> It could happen that the readyList is empty and we got here via a last
> remaining response. So, either the trace is complete or there are pending
> nodes in the depFreeQueue.

That holds only while nodes linger in the `readyList` with execute ticks far
in the future, which is what the inflated delays were doing. A branch was
added to `execute()` to schedule a cycle ahead when nothing is ready, nothing
is pending issue and no response is outstanding. **This did not fix the
`stride` stall** -- the stall reproduced to the digit, at `numOps` 256,877 --
so the reachable path is bug 3. The branch is kept because the hole is real.

**3. Locked-RMW replay deadlocks against TraceCPU's scheduling (the actual
stall).** x86 atomics reach the trace as `LOCKED_RMW` request flags, and
`Packet::createRead`/`createWrite` turn those flags back into
`LockedRMWReadReq`/`LockedRMWWriteReq` commands. The cache's protocol is that
a locked read holds the block for the RMW interval and only the matching
locked write releases it -- and when the locked read *misses*, the cache keeps
its MSHR targets unserviced until that write arrives, withholding the read's
response ("We are stopping servicing targets early for the Locked RMW Read
until the write comes", `BaseCache::recvTimingResp`).

TraceCPU cannot satisfy that. The write's register dependencies run through
the instructions that compute the modified value from the read, so the write
is not issued until the read's data returns. Each waits for the other. From
the capped `stride` trace:

```
seq 304694  LOAD   0xb4744  plain
seq 304701  LOAD   0xb4744  LOCKED_RMW                              <- locked read
seq 304704  STORE  0xb4744  LOCKED_RMW  reg=[304699,304703] rob=[304687,304701]
```

A locked read that *hits* is fine, because the cache fakes an MSHR and still
answers the read. That is why the deadlock is timing-dependent: with inflated
delays the ordinary load at the same block completes first and leaves the line
resident, so the locked read hits. Clamp the delays and the locked read
coalesces with the still-outstanding miss from `seq 304694`, and both stay in
flight forever. Four requests were stuck this way, two blocks' worth.

One stuck node is enough to stop everything, because TraceCPU gates issue on
the *span* between the oldest in-flight node and the candidate:

```
depGraph = 985, readyList = 0, depFreeQueue = 4, oldestInFlightRobNum = 256716
Checking resources to issue seq. num 305226: #in-flight nodes = 256931 - 256716 = 215, LQ = 4/32
Resources unavailable for seq. num 305226. Still pending issue.
```

215 against `sizeROB` of 192, forever.

Fixed in `executeMemReq()` by clearing `LOCKED_RMW` and `LLSC` from the
replayed request, counted by a new `numAtomicFlagsStripped` stat. TraceCPU
models neither protocol in the first place; replaying them as ordinary
accesses preserves the access stream and the recorded order dependency, which
is all it does model. In `stride` this touched 58 of 10,516,024 records.

### Diagnosing it: the failure looks like a DRAMPower performance problem

The stall presents as a hang, not an error, and it is worth knowing what it
looks like because the obvious reading is wrong. Simulated time keeps
advancing, because DRAM refresh events are still scheduled, so gem5 races
through empty time: the stalled `stride` replay reached **85.3 seconds** of
simulated time against a 23.9 ms reference, at 920e9 ticks per host second,
with the memory bus at 0.00% utilisation and 256,877 of 10.5M ops committed.
Every backtrace lands in DRAMPower's `power_calc` under
`DRAMInterface::Rank::processRefreshEvent`, which looks like a DRAMPower
performance bug and is not.

Two things that cost time here, worth avoiding next time:

* `kill -USR1` dumps gem5's stats mid-run, which is how the numbers above were
  obtained -- but the pid from `pgrep -f` may be the `timeout` wrapper rather
  than gem5, and signalling the wrapper silently does nothing. An empty
  `stats.txt` then looks like "the event queue is wedged" when the signal
  simply never arrived.
* `--debug-flags=TraceCPUData` with `--debug-file` is what actually localised
  this: the per-node occupancy lines name the stuck in-flight node directly.
  Piping the stream to `tail` instead does not work, since the pipeline is
  killed before `tail` flushes.

## The instruction trace sets a floor, and the capture's queue sizes set the floor

The clamp stops the data side from overshooting, which raises a second
question: what then determines the replay's runtime? For these memory-bound
kernels, the answer is the *instruction* trace.

`FixedRetryGen` replays instruction fetches at the recorded inter-fetch
deltas, not at absolute positions:

```cpp
Tick last_tick = currElement.tick;
...
delta = currElement.tick - last_tick;     // trace_cpu.cc
...
schedule(icacheNextEvent, curTick() + icacheGen.tickDelta());
```

Those deltas are additive, so they sum to a hard lower bound on the replay's
duration. Summing them directly out of `cpu0.inst.trace.gz`:

| kernel | fetch floor | O3 reference | floor vs reference |
|--------|------------:|-------------:|-------------------:|
| stride | 23,739,423,000 | 23,902,814,500 | -0.68% |
| seq    | 24,577,553,000 | 24,570,770,000 | +0.03% |
| chase  | 96,223,121,000 | 94,246,345,500 | +2.10% |

This is why the capped results must be read carefully. Capped `stride` lands
at 23,739,466,001, which is 43,001 ticks -- 0.0002% -- above its floor. The
agreement with the reference is therefore set by the fetch pacing, not by the
data-side model. `chase` cannot do better than +2.10% whatever the data side
does.

### So capture at the sizes you will replay at

The floor comes from the capture run, so the capture's ROB/LQ/SQ choice
decides it. Capturing `stride` at the replay's sizes instead of oversized:

```
capture at ROB 512 / LQ 128 / SQ 128   ->  fetch floor 23,739,423,000  (-0.68%)
capture at ROB 192 / LQ 32  / SQ 32    ->  fetch floor 23,902,812,000  (-0.00001%)
O3 reference at ROB 192 / LQ 32 / SQ 32                 23,902,814,500
```

Capturing at the replay's sizes puts the floor within **2,500 ticks** of the
reference. Those runs also confirm the probe itself is non-perturbing: given
the same sizes as the reference, the capture reports `simTicks` of
23,902,814,500 for `stride`, 94,246,345,500 for `chase`, 148,802,938,500 for
`rand` and 6,854,296,000 for `l2` -- *exactly* the reference value in all four
cases, to the tick -- so attaching `ElasticTrace` costs no simulated time. The whole -0.68%/+2.10% floor offset was the oversizing, not the probe.

This contradicts the oversizing that `attach_probes()` does by default, and
the reasoning behind it. Oversizing was meant to keep resource stalls out of
`comp_delay`; it does not achieve that (measured above), and it does shift the
fetch schedule away from the configuration being modelled. The recommended
capture is therefore the replay's sizes plus `--comp-delay-cap`.

## Measured: what the clamp recovers

All rows are one core, replayed at ROB/LQ/SQ 192/32/32 against an O3 reference
at the same sizes, and every capture is checksum-verified against native
execution before its row is used. "uncapped" and "capped" differ only in
`--comp-delay-cap 12000`; both legs ran on the same binary, and the uncapped
replays reproduce the pre-change numbers to the tick, so the clamp is the only
variable.

| kernel | O3 reference | uncapped replay | capped replay | capped, matched-size capture |
|--------|-------------:|----------------:|--------------:|-----------------------------:|
| stride | 23,902,814,500 | +51.436% | -0.683% | **+0.0002%** |
| seq    | 24,570,770,000 |  +9.724% | +0.028% | **+0.0007%** |
| rand   |148,802,938,500 | +42.983% | -0.00005% | **+0.00009%** |
| chase  | 94,246,345,500 |  +4.750% | +2.098% | **+0.0003%** |
| l2     |  6,854,296,000 |  +0.880% | -0.097% | **-0.00004%** |

The last column adds `--cap-rob 192 --cap-lq 32 --cap-sq 32`, i.e. capturing
at the sizes being replayed, for the reason in the fetch-floor section above.
With both changes the worst error across the five kernels is **+0.0007%**,
against **+51.4%** before.

The two changes are independent and fix different things. The clamp removes
the double-counted memory back-pressure, which is what the large errors were.
Matching the capture's queue sizes then removes the residual, which is not a
data-side error at all but the fetch floor -- most visible on `chase`, where
the clamp alone leaves +2.098% and the matched-size capture takes it to
+0.0003%.

### The capped replay lands on the fetch floor

The clamp does not make the data side *accurate*; it stops it from
overshooting. Once it no longer overshoots, the instruction trace's fixed
pacing decides the total, and it decides it very precisely:

| kernel | fetch floor | capped replay | above floor |
|--------|------------:|--------------:|------------:|
| stride | 23,739,423,000 | 23,739,466,001 | +0.0002% |
| seq    | 24,577,553,000 | 24,577,746,001 | +0.0008% |
| rand   |148,802,788,000 |148,802,860,001 | +0.00005% |
| chase  | 96,223,121,000 | 96,223,599,001 | +0.0005% |
| l2     |  6,847,620,000 |  6,847,620,000 | +0.0000% |

`l2` is the cleanest demonstration: its capped replay is *exactly* its fetch
floor, to the tick. And as the control for the clamp being too blunt, `l2`
matters most -- it is cache-resident, so it has almost no back-pressure to
misattribute and little for the clamp to remove. It does not regress
(+0.880% -> -0.097%), which is the evidence that clamping does not simply make
every replay faster.

So `chase`'s residual +2.098% against the reference is its floor offset, not a
data-side error, and no amount of data-side work would remove it. Read the
headline percentages with that in mind: after clamping, the error against the
reference is essentially *the capture's fetch pacing versus the reference's
runtime*, which is why matching the capture's queue sizes to the replay's
matters more than anything else in the last column.

### What the data side actually reproduces

Totals are floor-masked, so the data-side model has to be judged on rates and
latencies instead. For `stride`:

| metric | O3 reference | uncapped replay | capped replay | capped, matched-size |
|--------|-------------:|----------------:|--------------:|---------------------:|
| DRAM read bus utilisation | 17.86% | 11.79% | 17.98% | **17.86%** |
| data read avg latency | 54,871 | 55,825 | 54,897 | 55,588 |
| L2 MSHR miss latency | 86,564 | 87,549 | 86,606 | - |
| mem read queue length | 3.14 | 2.54 | 2.90 | 2.84 |
| L2 accesses | 853,881 | 853,861 | 853,866 | 853,870 |

Bus utilisation lands exactly on the reference, and the access stream was
never in doubt (within 0.002% throughout). The honest residual is **memory
level parallelism**: read queue depth 2.84-2.90 against the reference's 3.14,
about 10% short. The clamp recovers most of the lost overlap -- uncapped sits
at 2.54 -- but not all of it, because a single per-node delay cannot express
the burstiness of a real out-of-order core's issue pattern. That is the next
thing to attack, and it is a modelling limit rather than a bug.

### Cost

The clamp is free at replay time: `stride` replays in 14.67 host seconds
capped against 13.80 uncapped, and `seq` is actually faster capped (27.77s vs
36.60s) because it no longer simulates the inflated idle gaps.

## Does a capped replay still respond to the memory system?

The fetch floor raises a fair objection: if the replay's runtime is pinned to
the instruction trace's pacing, can it still be used to evaluate a cache
hierarchy or an interconnect -- which is the whole point of driving CHI with
these traces? Measured, the answer is yes, but the obvious test gives the
wrong answer first.

Replaying capped `stride` while degrading the memory system barely moves it:

| stride, capped | simTicks | vs baseline |
|----------------|---------:|------------:|
| 1MB L2         | 23,902,870,000 | - |
| 128kB L2       | 23,902,959,000 | +0.0004% |
| no L2          | 23,903,257,001 | +0.0016% |

That looks like total insensitivity, and it is not. `stride` streams through an
array with a **99.99% L2 miss rate** (853,804 of 853,870), so the L2 was doing
nothing before it was removed. The test cannot measure what it appears to.

Repeating it on `l2`, a kernel sized to live in the L2 (3.5% miss rate):

| l2 kernel, capped | simTicks | vs baseline | L2 miss rate |
|-------------------|---------:|------------:|-------------:|
| 1MB L2            |  6,854,293,500 | -       | 3.5% |
| no L2             | 11,203,371,500 | +63.5%  | -    |
| 64kB L2           | 13,365,179,000 | +95.0%  | 99.97% |

So the fetch floor is a **lower bound, not a ceiling**. A memory system at
least as fast as the captured one leaves the replay sitting on the floor; make
it slower and the data side pushes straight past it, by 63-95% here. The
uncapped traces demonstrated the same thing in reverse -- their inflated
`comp_delay` pushed `stride` 52% above the floor.

(The 64kB L2 being *worse* than no L2 at all is correct, not a bug: it adds a
lookup latency on the way to DRAM while missing 99.97% of the time.)

### Consequence for hierarchy comparisons

Replaying capped `stride` through this repo's Ruby CHI hierarchy instead of
the classic L1/L2/XBar:

```
classic hierarchy, capped   23,902,870,000   58,000 ticks above floor
CHI hierarchy, capped       23,902,815,000    3,000 ticks above floor
fetch floor                 23,902,812,000
O3 reference                23,902,814,500
```

Both land on the floor, so the two hierarchies appear identical in total
runtime. For a DRAM-bound streaming kernel they genuinely are comparable, but
the general lesson is that **total simTicks is a poor discriminator whenever
the workload is not memory-bound at the level being studied**. Compare
memory-side stats -- miss rates, MSHR latencies, bus utilisation, queue depths
-- rather than tick counts, and pick workloads that actually stress the level
of the hierarchy under test.

## Trace replication, re-measured on capped traces

The weak-scaling replication of section III-A4b was originally validated here
on uncapped traces, so it was worth re-running once `comp_delay` was fixed.
`stride`, captured at matched sizes with `--comp-delay-cap 12000`, replicated
1 -> 2 -> 4 cores:

| cores | simTicks | vs 1 core | L2 accesses | L2 miss rate |
|------:|---------:|----------:|------------:|-------------:|
| 1 | 23,902,870,000 | -       |   853,870 | 99.99% |
| 2 | 27,854,512,000 | +16.5%  | 1,707,726 | 99.99% |
| 4 | 37,710,199,500 | +57.8%  | 3,412,899 | 99.99% |

Accesses scale exactly with core count, and so does the work: replication is
**weak scaling**, so each core replays the *full* trace against its own slice
of memory. Four cores means 4x the total work, not the same work split up --
aggregate micro-ops go 10,619,504 -> 42,478,016, exactly 4.0000x, with every
core executing an identical count.

Runtime therefore has to be read as throughput, not as a slowdown:

| cores | aggregate ops | simTicks | speedup | parallel efficiency |
|------:|--------------:|---------:|--------:|--------------------:|
| 1 | 10,619,504 | 23,902,870,000 | 1.000x | 100.0% |
| 2 | 21,239,008 | 27,854,512,000 | 1.716x |  85.8% |
| 4 | 42,478,016 | 37,710,199,500 | 2.535x |  63.4% |

Four cores deliver 2.54x the throughput at 63% efficiency, which is DRAM
bandwidth saturating on a streaming kernel. (Quoting the same data as "+57.8%
runtime at 4 cores" is misleading, since the work quadrupled.)

### Why the kernel is not split across the cores instead

A single-threaded elastic trace is one sequential dependency graph: every
record carries reg_dep/rob_dep edges to earlier records in the same stream.
Partitioning it across cores would violate those edges, because one core's
records depend on values produced by records handed to another. It would also
invent parallelism the program never had, with no synchronisation where the
real program would need it.

Strong scaling requires the work to have been split *at capture time* -- one
trace per thread, each with its own dependency graph, plus the sync trace so
the arbiter can order critical sections. That needs FS mode (see above), so
every multicore number in this document measures **memory-system contention
under N symmetric streaming loads**. It says nothing about how an N-thread
program behaves: there is no divergent control flow, no load imbalance, no true
sharing, and nothing for the arbiter to arbitrate.

### Address offsetting is not optional, and the miss rate will not tell you

Replicating the same trace onto 4 cores *without* the address offset:

```
4 cores, offsets    37,710,199,500
4 cores, no offsets 23,902,948,001     <- the 1-core floor
```

Skipping the offset makes a 4-core system look **37% cheaper than it is**, and
essentially free relative to one core. The earlier note on this recorded the
effect as a collapsing L2 miss rate. On a streaming kernel it is not that at
all:

| stat                       | 4c with offset | 4c no offset |
|----------------------------|---------------:|-------------:|
| L2 miss rate               | 99.99%         | 99.99%       |
| L2 MSHR hits               | 2,412          | 2,364,217    |
| DRAM read requests         | 3,410,227      | 853,697      |
| DRAM read bus utilisation  | 45.22%         | 17.86%       |
| mem read queue length      | 12.90          | 2.73         |
| data read avg latency      | 127,624        | 55,492       |

The channel is **MSHR coalescing**, not cache hits. Four cores requesting the
same line have three of every four requests merged into the outstanding MSHR,
so DRAM sees exactly the single-core request count (853,697 against the 1-core
853,804) and the bus sits at 17.86%, the 1-core figure, instead of 45.22%.
Latency is understated by 2.3x.

The practical warning: **the L2 miss rate is identical (99.99%) in both cases**,
so the metric most people would check detects nothing. Whether replication is
double-counting sharing has to be checked on MSHR hits or DRAM request counts.
A miss rate that collapses, as seen on cache-resident workloads, is only the
version of this failure that happens to be visible.

### Inflated comp_delay also breaks multicore scaling, in the flattering direction

Replicating the *same* `stride` capture both ways, capped and uncapped, and
replaying each on 1, 2 and 4 cores:

| cores | capped          | vs 1c   | uncapped        | vs 1c   |
|------:|----------------:|--------:|----------------:|--------:|
| 1 | 23,902,870,000 | -       | 36,197,401,500 | -      |
| 2 | 27,854,512,000 | +16.5%  | 37,317,347,500 | +3.1%  |
| 4 | 37,710,199,500 | +57.8%  | 44,617,620,500 | +23.3% |

The uncapped traces understate contention by **5.3x at two cores and 2.5x at
four**. The reason is the same back-pressure double-count: a per-node delay of
~208ns before each load spaces requests out far enough that the memory system
is never driven hard enough to contend. Remove the inflation and four
replicas saturate DRAM bandwidth, as they should.

This is a second, independent reason the clamp matters. The single-core result
showed inflation breaks *absolute* timing. This shows it also breaks the
*relative* conclusions that weak-scaling studies are run to produce -- and it
breaks them optimistically, which is the worse direction. Note also how the two
errors interact misleadingly: an uncapped trace is ~50% too slow in absolute
terms while making multicore scaling look ~2.5x better than it is. Any
scaling conclusion drawn on this branch before the clamp should be re-measured.

## Replay speed with the CHI hierarchy

Measured on `stride` (6,895,009 x86 instructions / 10,618,045 micro-ops), same
host, capped trace captured at matched sizes:

| configuration | host | aggregate ops/s | aggregate insts/s | per-core insts/s |
|---------------|-----:|----------------:|------------------:|-----------------:|
| TraceCPU + classic L1/L2/XBar, 1 core |  16.0s | 0.663M | 0.431M | 0.431M |
| TraceCPU + CHI (`gem5-chi-l2`), 1 core | 120.3s | 0.088M | 0.057M | 0.057M |
| TraceCPU + CHI, 4 cores (replicated)   | 794.0s | 0.054M | 0.035M | 0.0087M |
| O3 + classic, 1 core (the capture run) |  76.6s | 0.139M | 0.090M | 0.090M |

**The CHI hierarchy costs 7.5x in host time** against the classic one for an
identical trace and identical TraceCPU (120.3s vs 16.0s). That is Ruby/SLICC
state-machine overhead and has nothing to do with the traces.

**Scaling to 4 cores is super-linear: 6.6x the host time for 4x the work**,
1.65x worse than linear. Aggregate throughput *falls* 39% (0.088 -> 0.054M
ops/s) and per-core throughput falls 6.6x. gem5's event queue is
single-threaded, so the 4 RN-Fs' coherence traffic is serialised onto one host
thread. Extrapolating the same exponent puts 16 cores at roughly 3-4 host hours
for this trace.

### The replay speedup does not survive CHI

With the classic hierarchy, replay is 4.8x faster than the O3 run it replaces
(0.431 vs 0.090 M insts/s), consistent with the 6-8x the Elastic Traces paper
claims. Route it through CHI and the replay becomes *slower* than that O3 run
(0.057 vs 0.090).

That comparison is not fair in either direction -- the O3 figure is O3 with the
*classic* hierarchy, and Ruby CHI is expensive whatever CPU drives it. The
question that decides whether trace replay is worth using for CHI studies is
TraceCPU+CHI versus **O3+CHI**, which is not measured here: `capture.py` only
builds the classic hierarchy, so it needs a config pairing an O3 CPU with
`config/chi`. Until that exists, treat the speedup premise as unverified for
CHI, and note that the 4-core figure above (0.035 M insts/s aggregate) is the
realistic budget for CHI replay work.

### One upside at 4 cores

At 1 core, CHI and the classic hierarchy are indistinguishable in simulated
time -- both sit on the fetch floor, 500 ticks apart. At 4 cores CHI is **+8.2%**
slower than classic (40,802,519,500 vs 37,710,199,500), because coherence
traffic finally shows up. So the "total simTicks cannot discriminate
hierarchies" caveat above is a property of the single-core DRAM-bound case, not
a general limit: once there is contention, the hierarchy does separate.

## Where the host time actually goes: is Ruby the bottleneck?

Profiled with `perf record -F 499`, both models on the *identical*
`PrivateL1SharedL2CacheHierarchy`, same board, clock and memory. The O3 side
needed a new config, `o3_chi.py`, because `capture.py` only builds the classic
L1/L2/XBar -- comparing TraceCPU+CHI against an O3 run on the classic
hierarchy is not a fair comparison and was the error in an earlier draft of
these notes.

### Across kernels, one core

Sorted by memory intensity (L2 demand accesses per 1000 ops; see the note on
why dcache misses are *not* a usable metric here):

```
  kernel    L2acc |  O3 Ruby  O3 CPU   O3 ops/s |  Tr Ruby  Tr CPU   Tr ops/s |  speedup  phase
  l1         0.10 |    26.0%   50.4%    0.2664M |    40.0%   16.1%    0.8024M |    3.01x     ok
  l2         5.26 |    30.8%   45.4%    0.2153M |    47.3%   13.7%    0.5472M |    2.54x   0.86
  seq        5.57 |    30.3%   46.4%    0.2082M |    46.4%   13.8%    0.5720M |    2.75x   0.57
  fp         7.15 |    25.9%   50.0%    0.2759M |    45.4%   14.3%    0.5954M |    2.16x     ok
  rand      35.03 |    37.0%   39.7%    0.1144M |    65.0%    4.5%    0.1917M |    1.68x   0.60
  chase     35.51 |    40.7%   34.8%    0.0991M |    64.6%    4.5%    0.1741M |    1.76x   0.57
  stride    80.40 |    51.2%   25.2%    0.0574M |    68.6%    3.4%    0.0866M |    1.51x   0.86
```

`phase` is O3 ops / trace ops under an equal *host-time* budget; `ok` means
both covered the same work. Only `l1` and `fp` are exactly phase-matched, so
the other speedups are indicative. The trend holds across both groups.

**Ruby's share tracks memory intensity and the speedup is its mirror image.**
Over an 800x range in accesses per op, Ruby goes 26% -> 51% (O3) and 40% -> 69%
(TraceCPU) while the speedup falls monotonically 3.01x -> 1.51x. Ruby is work
neither model can skip, so its share in the replay sets the ceiling. Plain
Amdahl.

So "is Ruby the bottleneck" has a workload-dependent answer. For memory-bound
kernels, yes -- 65-69% for replay. For cache-resident or compute-bound ones it
is 40-47%, and on the O3 side the *CPU model* is the larger cost (50%).

### TraceCPU's own cost is not negligible

The replay machinery is 3.4-4.5% on memory-bound kernels but **13.7-16.1%** on
the fast ones, because its cost is per *record* while the simulation's total
time is dominated by *stalls*. Breaking down `l1` (the fastest, 0.80 M ops/s):

| component | share |
|-----------|------:|
| Ruby | 40.0% |
| TraceCPU dependency graph | 15.5% |
| protobuf parsing | 11.6% |
| allocation | 10.0% |
| gzip inflate | 0.9% |
| **trace machinery total** | **28.1%** |

`protobuf::TcParser::VerifyHasBitConsistency` alone is 3.0%, pure validation
overhead on a file gem5 wrote itself. `addDepsOnParent` walks a
`std::list<unsigned long>` per node, which is behind much of the allocation.
Both are optimisation targets if replay throughput ever matters.

### Scaling to 16 cores (stride)

```
  model   cores     host     agg ops     ops/s   Ruby    net  proto  alloc    CPU
  o3          1     159s   9,142,473   0.0574M  51.2%  22.7%  20.3%   6.8%  25.2%
  o3          2     159s   7,689,766   0.0484M  46.5%  21.1%  18.2%   6.4%  30.2%
  o3          4     158s   6,330,644   0.0401M  40.2%  18.3%  15.8%   5.5%  36.9%
  o3          8     154s   6,136,565   0.0399M  32.9%  15.7%  12.2%   5.1%  43.5%
  o3         16     135s   6,491,179   0.0480M  30.9%  18.5%   8.3%   4.2%  42.1%
  trace       1     123s  10,619,504   0.0866M  68.6%  30.9%  26.9%   7.2%   3.4%
  trace       2     159s  13,138,328   0.0826M  69.9%  32.4%  26.8%   7.2%   3.1%
  trace       4     158s  10,728,255   0.0678M  72.0%  34.7%  26.9%   6.5%   2.7%
  trace       8     156s  12,289,110   0.0787M  70.4%  35.8%  24.9%   6.2%   3.3%
  trace      16     150s  15,253,095   0.1017M  64.6%  38.3%  18.6%   6.1%   4.2%
```

Two things here. **O3's Ruby share falls with core count (51% -> 31%) while
TraceCPU's stays flat (65-72%).** O3 pays host time per simulated *cycle*, so
contention-induced stalls inflate its CPU-model cost; TraceCPU is event-driven
and pays per *access*, so its mix is unchanged. And **network/message handling
overtakes the protocol**: for TraceCPU, MessageBuffer/PerfectSwitch/Throttle/
routing go 30.9% -> 38.3% while CHI controller and cache work falls 26.9% ->
18.6%. At 16 cores, message plumbing is the single largest consumer of the
whole simulation.

Because the eliminable fraction (O3's CPU model) *grows* with core count,
**TraceCPU's advantage widens with scale: 1.51x, 1.71x, 1.69x, 1.97x, 2.12x**
at 1/2/4/8/16 cores.

Caveat: the `ops/s` column is not a scaling curve. An equal host-time budget
means different core counts reach different program phases (9.1M ops/core at 1
core versus 406K at 16, where the program is still in compute-bound libc
startup), which is why aggregate throughput dips then rises. The speedup
*ratios* are sound -- same core count, similar phase -- and the profiles are
within-window fractions, so both are unaffected.

### Why dcache misses are not a usable intensity metric here

The obvious metric disagrees wildly between the models -- on `l2`, 15.4 dcache
misses per 1000 ops for O3 against 63.8 for the replay. That is not an accuracy
problem:

| stat | o3_l2 | trace_l2 |
|------|------:|---------:|
| dcache accesses | 4,590,888 | 5,343,875 |
| dcache misses | 428,803 | 2,062,740 |
| L2 accesses | 146,212 | 169,834 |
| L2 misses | 8,771 | 9,172 |

If the replay really missed 2.06M times, L2 would see 2.06M accesses; it sees
169,834. The extra misses never leave the L1 -- they are **MSHR-coalesced
secondary misses**. The capped trace has high memory-level parallelism, so
several accesses to one line are outstanding at once and Ruby counts each as a
demand miss, where O3's LSQ would have merged or forwarded them before the
cache saw them. Same coalescing mechanism as the replication study above.

L2 accesses per 1000 ops is coalescing-independent and agrees to two decimals
between the models (`l1` 0.10/0.10, `l2` 5.25/5.26), which is also a further
check that the replay reproduces the memory behaviour.
